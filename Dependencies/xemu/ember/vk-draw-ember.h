/*
 * Ember: primitive types Metal cannot draw.
 *
 * The Xbox GPU draws quads, quad strips, triangle fans, polygons and line
 * loops. xemu's Vulkan renderer hands the first two to a geometry shader
 * and leaves the rest to the driver. Metal has neither geometry shaders nor
 * these primitive types, so here they are turned into plain triangle lists
 * (or a line strip) with an index list built on the CPU.
 *
 * Included by hw/xbox/nv2a/pgraph/vk/draw.c just above
 * pgraph_vk_flush_draw(); the calls into it are added by patch_xemu.py.
 * LGPL-2.1-or-later, like the file it is included in.
 */

static struct {
    bool active;          /* the current draw goes through the index list */
    GArray *indices;      /* uint32_t; all segments back to back */
    GArray *seg_first;    /* uint32_t; where each segment starts in `indices` */
    GArray *seg_count;    /* uint32_t; how many indices it has */
    VkDeviceSize offset;  /* of `indices` in the index buffer */
} ember_prims;

static bool ember_prim_needs_indices(int mode)
{
    switch (mode) {
    case PRIM_TYPE_LINE_LOOP:
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
    case PRIM_TYPE_POLYGON:
        return true;
    default:
        return false;
    }
}

/*
 * Adds one run of `count` vertices. `in` is the guest's index list, or NULL
 * when the vertices are simply first, first+1, ... Triangles keep the
 * winding of the original primitive.
 */
static void ember_prims_add(int mode, const uint32_t *in, uint32_t first, uint32_t count)
{
#define V(i) (in ? in[(i)] : first + (i))
#define EMIT(a, b, c) do { \
        uint32_t t[3] = { V(a), V(b), V(c) }; \
        g_array_append_vals(ember_prims.indices, t, 3); \
    } while (0)

    uint32_t start = ember_prims.indices->len;

    switch (mode) {
    case PRIM_TYPE_QUADS:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            EMIT(i + 3, i, i + 1);
            EMIT(i + 3, i + 1, i + 2);
        }
        break;
    case PRIM_TYPE_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < count; i += 2) {
            EMIT(i, i + 1, i + 2);
            EMIT(i + 2, i + 1, i + 3);
        }
        break;
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_POLYGON:
        for (uint32_t i = 1; i + 1 < count; i++) {
            EMIT(i, i + 1, 0);
        }
        break;
    case PRIM_TYPE_LINE_LOOP:
        if (count >= 2) {
            for (uint32_t i = 0; i < count; i++) {
                uint32_t v = V(i);
                g_array_append_val(ember_prims.indices, v);
            }
            uint32_t v = V(0);
            g_array_append_val(ember_prims.indices, v);
        }
        break;
    default:
        break;
    }

    uint32_t n = ember_prims.indices->len - start;
    g_array_append_val(ember_prims.seg_first, start);
    g_array_append_val(ember_prims.seg_count, n);
#undef EMIT
#undef V
}

/*
 * At the start of a draw: decide whether it needs an index list and, if so,
 * build it from whichever of the four vertex sources the guest used. The
 * segments are numbered the way pgraph_vk_flush_draw() issues its draws.
 */
static void ember_prims_begin(PGRAPHState *pg)
{
    if (!ember_prims.indices) {
        ember_prims.indices = g_array_new(FALSE, FALSE, sizeof(uint32_t));
        ember_prims.seg_first = g_array_new(FALSE, FALSE, sizeof(uint32_t));
        ember_prims.seg_count = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    }
    g_array_set_size(ember_prims.indices, 0);
    g_array_set_size(ember_prims.seg_first, 0);
    g_array_set_size(ember_prims.seg_count, 0);

    int mode = pg->primitive_mode;
    ember_prims.active = ember_prim_needs_indices(mode);
    if (!ember_prims.active) {
        return;
    }

    if (pg->draw_arrays_length) {
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            ember_prims_add(mode, NULL, pg->draw_arrays_start[i], pg->draw_arrays_count[i]);
        }
    } else if (pg->inline_elements_length) {
        ember_prims_add(mode, pg->inline_elements, 0, pg->inline_elements_length);
    } else if (pg->inline_buffer_length) {
        ember_prims_add(mode, NULL, 0, pg->inline_buffer_length);
    } else if (pg->inline_array_length) {
        /* Same vertex size calculation as the inline array path below. */
        unsigned int offset = 0;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count == 0) {
                continue;
            }
            offset = ROUND_UP(offset, attr->size);
            offset += attr->size * attr->count;
            offset = ROUND_UP(offset, attr->size);
        }
        if (offset) {
            ember_prims_add(mode, NULL, 0, pg->inline_array_length * 4 / offset);
        }
    }

    /* May end the current command buffer, so it has to happen up here. */
    ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                        MAX(ember_prims.indices->len, 1) * sizeof(uint32_t));
}

/* After begin_pre_draw(): put the list where the GPU will read it. */
static void ember_prims_upload(PGRAPHState *pg)
{
    if (ember_prims.active && ember_prims.indices->len) {
        ember_prims.offset = pgraph_vk_update_index_buffer(
            pg, ember_prims.indices->data,
            ember_prims.indices->len * sizeof(uint32_t));
    }
}

/* Draws segment `seg`. Returns false when the caller should draw as usual. */
static bool ember_prims_draw(PGRAPHState *pg, int seg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!ember_prims.active) {
        return false;
    }
    if (seg < ember_prims.seg_count->len) {
        uint32_t count = g_array_index(ember_prims.seg_count, uint32_t, seg);
        uint32_t first = g_array_index(ember_prims.seg_first, uint32_t, seg);
        if (count) {
            vkCmdBindIndexBuffer(r->command_buffer,
                                 r->storage_buffers[BUFFER_INDEX].buffer,
                                 ember_prims.offset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(r->command_buffer, count, 1, first, 0, 0);
        }
    }
    return true;
}
