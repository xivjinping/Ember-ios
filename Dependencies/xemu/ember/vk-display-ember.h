/*
 * Ember: hand xemu's finished display image to the app.
 *
 * xemu's desktop build shares the Vulkan display image with OpenGL. iOS has
 * no OpenGL, so after the display pass the image is copied into a
 * host-visible buffer and passed to ember_display_frame() (ui/ember-core.c).
 * The image is already at the internal resolution (surface scale) and has
 * the overlay video plane composited, so this replaces the old path through
 * guest video memory, which was always 640x480.
 *
 * Included by hw/xbox/nv2a/pgraph/vk/display.c (see patch_xemu.py).
 *
 * This file is part of Ember and is distributed under the GNU GPL v2.
 */
#ifndef EMBER_VK_DISPLAY_EMBER_H
#define EMBER_VK_DISPLAY_EMBER_H

void ember_display_frame(const void *rgba, int width, int height);

static VkBuffer ember_rb_buffer;
static VmaAllocation ember_rb_allocation;
static VkDeviceSize ember_rb_size;

/*
 * Recorded right after the display render pass, while the display image is
 * still in COLOR_ATTACHMENT_OPTIMAL; leaves it in that layout.
 */
static void ember_display_record_copy(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;
    VkDeviceSize need = (VkDeviceSize)disp->width * disp->height * 4;

    if (need > ember_rb_size) {
        if (ember_rb_buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(r->allocator, ember_rb_buffer, ember_rb_allocation);
            ember_rb_buffer = VK_NULL_HANDLE;
        }
        VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = need,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VmaAllocationCreateInfo alloc_info = {
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        };
        VK_CHECK(vmaCreateBuffer(r->allocator, &buffer_info, &alloc_info,
                                 &ember_rb_buffer, &ember_rb_allocation, NULL));
        ember_rb_size = need;
    }

    pgraph_vk_transition_image_layout(pg, cmd, disp->image,
                                      VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region = {
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.layerCount = 1,
        .imageExtent.width = disp->width,
        .imageExtent.height = disp->height,
        .imageExtent.depth = 1,
    };
    vkCmdCopyImageToBuffer(cmd, disp->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           ember_rb_buffer, 1, &region);
    pgraph_vk_transition_image_layout(pg, cmd, disp->image,
                                      VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

/* Called after the command buffer has completed (single-time commands wait). */
static void ember_display_deliver(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;
    void *data = NULL;

    if (ember_rb_buffer == VK_NULL_HANDLE) {
        return;
    }
    vmaInvalidateAllocation(r->allocator, ember_rb_allocation, 0, VK_WHOLE_SIZE);
    if (vmaMapMemory(r->allocator, ember_rb_allocation, &data) != VK_SUCCESS) {
        return;
    }
    ember_display_frame(data, disp->width, disp->height);
    vmaUnmapMemory(r->allocator, ember_rb_allocation);
}

#endif
