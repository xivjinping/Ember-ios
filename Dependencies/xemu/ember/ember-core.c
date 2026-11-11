/*
 * Ember: starts and drives the xemu machine inside the iOS app.
 *
 * On desktop this is the job of ui/xemu.c, which also owns the SDL window
 * and the OpenGL context. Here there is no window: the machine runs on its
 * own thread, the picture is copied into a buffer the app reads
 * (ember_xemu_lock_frame), and the app's controls arrive through
 * ember_input_set (ember-frontend.c).
 *
 * Copied to ui/ by Dependencies/xemu/patch_xemu.py. GPL-2.0, like xemu.
 */
#include "qemu/osdep.h"
#include <locale.h>
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "monitor/qdev.h"
#include "hw/qdev-core.h"
#include "hw/core/cpu.h"
#include "block/aio.h"
#include "system/system.h"
#include "system/runstate.h"
#include "ui/console.h"

#include "ui/xemu-settings.h"
#include "ui/xemu-input.h"
#include "ui/ember-xemu.h"

static int ember_state = EMBER_XEMU_IDLE;
static int ember_controllers;
static char **ember_argv;
static int ember_argc;

/* ---- Picture --------------------------------------------------------------------- */

static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *frame_bits;
static pixman_image_t *frame_image;
static int frame_width, frame_height;
static uint64_t frame_serial;

static DisplayChangeListener ember_dcl;
static DisplaySurface *ember_surface;

/* Copies the whole guest surface, converting whatever format it has to BGRA. */
static void ember_copy_surface(void)
{
    DisplaySurface *s = ember_surface;
    if (!s) {
        return;
    }
    int w = surface_width(s), h = surface_height(s);
    if (w <= 0 || h <= 0) {
        return;
    }
    pthread_mutex_lock(&frame_lock);
    if (w != frame_width || h != frame_height || !frame_image) {
        if (frame_image) {
            pixman_image_unref(frame_image);
        }
        frame_bits = g_realloc(frame_bits, (size_t)w * h * 4);
        frame_image = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, frame_bits, w * 4);
        frame_width = w;
        frame_height = h;
    }
    pixman_image_composite(PIXMAN_OP_SRC, s->image, NULL, frame_image, 0, 0, 0, 0, 0, 0, w, h);
    frame_serial++;
    pthread_mutex_unlock(&frame_lock);
}

static void ember_gfx_switch(DisplayChangeListener *dcl, DisplaySurface *new_surface)
{
    ember_surface = new_surface;
    ember_copy_surface();
}

static void ember_gfx_update(DisplayChangeListener *dcl, int x, int y, int w, int h)
{
    ember_copy_surface();
}

static const DisplayChangeListenerOps ember_dcl_ops = {
    .dpy_name = "ember",
    .dpy_gfx_switch = ember_gfx_switch,
    .dpy_gfx_update = ember_gfx_update,
};

/*
 * The frame thread: about 60 times a second, bring the picture up to date.
 *
 * What the guest's GPU drew lives in the renderer (Vulkan images). Asking
 * for the framebuffer surface makes the renderer copy the displayed image
 * back into guest video memory; QEMU's VGA code then turns video memory into
 * the display surface, which ember_gfx_update() copies for the app. xemu's
 * desktop UI shows the renderer's image directly instead; that is a later
 * optimisation here.
 *
 * The first call must be made without the big QEMU lock, because the GPU
 * thread it waits for takes that lock at times. The second needs the lock.
 */
int nv2a_get_framebuffer_surface(void);
void nv2a_release_framebuffer_surface(void);
bool nv2a_ember_renderer_ready(void); /* added to pgraph.c by patch_xemu.py */

static QemuThread frame_thread;
static bool frame_thread_started, frame_thread_stop;

static void *ember_frame_thread(void *opaque)
{
    while (!qatomic_read(&frame_thread_stop)) {
        if (qatomic_read(&ember_state) == EMBER_XEMU_RUNNING && nv2a_ember_renderer_ready()) {
            nv2a_get_framebuffer_surface();
            nv2a_release_framebuffer_surface();
            bql_lock();
            graphic_hw_update(ember_dcl.con);
            bql_unlock();
        }
        g_usleep(16000);
    }
    return NULL;
}

bool ember_xemu_lock_frame(const uint8_t **bgra, uint32_t *width, uint32_t *height, uint64_t *serial)
{
    pthread_mutex_lock(&frame_lock);
    if (!frame_bits) {
        pthread_mutex_unlock(&frame_lock);
        return false;
    }
    *bgra = (const uint8_t *)frame_bits;
    *width = frame_width;
    *height = frame_height;
    *serial = frame_serial;
    return true;
}

void ember_xemu_unlock_frame(void)
{
    pthread_mutex_unlock(&frame_lock);
}

/* ---- Controllers ------------------------------------------------------------------- */

/*
 * Plugs a gamepad into a console port, the way ui/xemu-input.c does when a
 * controller is bound: the pad's own three-port hub on the console port,
 * and the pad on the hub's first port.
 */
static void ember_plug_gamepad(int index)
{
    static const int port_map[4] = { 3, 4, 1, 2 };
    char *tmp;

    QDict *hub = qdict_new();
    qdict_put_str(hub, "driver", "usb-hub");
    tmp = g_strdup_printf("1.%d", port_map[index]);
    qdict_put_str(hub, "port", tmp);
    g_free(tmp);
    qdict_put_int(hub, "ports", 3);
    QemuOpts *hub_opts = qemu_opts_from_qdict(qemu_find_opts("device"), hub, &error_abort);
    DeviceState *hub_dev = qdev_device_add(hub_opts, &error_abort);
    qobject_unref(hub);
    object_unref(OBJECT(hub_dev));

    QDict *pad = qdict_new();
    qdict_put_str(pad, "driver", DRIVER_DUKE);
    tmp = g_strdup_printf("gamepad_%d", index);
    qdict_put_str(pad, "id", tmp);
    g_free(tmp);
    qdict_put_int(pad, "index", index);
    tmp = g_strdup_printf("1.%d.1", port_map[index]);
    qdict_put_str(pad, "port", tmp);
    g_free(tmp);
    QemuOpts *pad_opts = qemu_opts_from_qdict(qemu_find_opts("device"), pad, &error_abort);
    DeviceState *pad_dev = qdev_device_add(pad_opts, &error_abort);
    qobject_unref(pad);
    object_unref(OBJECT(pad_dev));
}

/* ---- The "xemu" display --------------------------------------------------------------- */

/* QEMU's vl.c asks for "-display xemu"; this is Ember's version of it. */
static void ember_display_init(DisplayState *ds, DisplayOptions *o)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);
    assert(con != NULL);
    ember_dcl.ops = &ember_dcl_ops;
    ember_dcl.con = con;
    register_displaychangelistener(&ember_dcl);
    qemu_thread_create(&frame_thread, "ember-frame", ember_frame_thread, NULL, QEMU_THREAD_JOINABLE);
    frame_thread_started = true;

    for (int i = 0; i < ember_controllers; i++) {
        ember_plug_gamepad(i);
    }
}

static QemuDisplay ember_display = {
    .type = DISPLAY_TYPE_XEMU,
    .init = ember_display_init,
};

static void ember_register_display(void)
{
    qemu_display_register(&ember_display);
}

type_init(ember_register_display);

/* ---- Code generator memory and progress ------------------------------------------------ */

/* Added to tcg/region.c by patch_xemu.py. */
void tcg_ember_set_code_buffer(void *rx, void *rw, size_t size);

static bool ember_have_code_buffer;

void ember_xemu_set_code_buffer(void *rx, void *rw, size_t size)
{
    if (ember_xemu_state() == EMBER_XEMU_IDLE && rx && rw && size) {
        tcg_ember_set_code_buffer(rx, rw, size);
        ember_have_code_buffer = true;
    }
}

uint64_t ember_xemu_guest_pc(void)
{
    if (ember_xemu_state() != EMBER_XEMU_RUNNING) {
        return 0;
    }
    /* Read without stopping the CPU: possibly a little stale, never unsafe. */
    CPUState *cpu = first_cpu;
    if (!cpu) {
        return 0;
    }
    CPUClass *cc = CPU_GET_CLASS(cpu);
    return cc->get_pc ? cc->get_pc(cpu) : 0;
}

/* SDL refuses to start on iOS until told the app's main() has run. */
void SDL_SetMainReady(void);

/* ---- Machine thread ------------------------------------------------------------------ */

static void *ember_machine_thread(void *opaque)
{
    qemu_init(ember_argc, ember_argv);
    fprintf(stderr, "ember: machine built, entering the main loop\n");
    qatomic_set(&ember_state, EMBER_XEMU_RUNNING);
    int status = qemu_main_loop();
    fprintf(stderr, "ember: main loop returned %d\n", status);
    /* The frame thread uses the GPU emulation, which cleanup tears down. */
    qatomic_set(&ember_state, EMBER_XEMU_STARTING);
    if (frame_thread_started) {
        qatomic_set(&frame_thread_stop, true);
        bql_unlock();
        qemu_thread_join(&frame_thread);
        bql_lock();
    }
    qemu_cleanup(status);
    qatomic_set(&ember_state, EMBER_XEMU_EXITED);
    return NULL;
}

bool ember_xemu_start(const ember_xemu_config *c)
{
    static QemuThread thread;

    if (qatomic_cmpxchg(&ember_state, EMBER_XEMU_IDLE, EMBER_XEMU_STARTING) != EMBER_XEMU_IDLE) {
        return false;
    }
    setlocale(LC_NUMERIC, "C");
    SDL_SetMainReady();
    fprintf(stderr, "ember: starting xemu; code buffer %s\n",
            ember_have_code_buffer ? "from Ember's JIT region" : "NOT SET (QEMU will try to allocate its own)");

    /* xemu's vl.c builds the machine's arguments from these settings. */
    xemu_settings_set_path(c->settings_path);
    xemu_settings_load();
    g_config.general.show_welcome = false;
    /* Vulkan through MoltenVK; pgraph.c falls back to the null renderer if it cannot start. */
    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_VULKAN;
    g_config.display.quality.surface_scale = 1;
    g_config.sys.mem_limit = c->ram_mb >= 128 ? CONFIG_SYS_MEM_LIMIT_128 : CONFIG_SYS_MEM_LIMIT_64;
    xemu_settings_set_string(&g_config.sys.files.bootrom_path, c->bootrom_path);
    xemu_settings_set_string(&g_config.sys.files.flashrom_path, c->flashrom_path);
    xemu_settings_set_string(&g_config.sys.files.hdd_path, c->hdd_path);
    xemu_settings_set_string(&g_config.sys.files.dvd_path, c->dvd_path);
    xemu_settings_set_string(&g_config.sys.files.eeprom_path, c->eeprom_path);

    ember_controllers = MAX(0, MIN(4, c->controllers));

    int extra = 0;
    while (c->extra_args && c->extra_args[extra]) {
        extra++;
    }
    ember_argv = g_new0(char *, extra + 2);
    ember_argv[ember_argc++] = g_strdup("xemu");
    for (int i = 0; i < extra; i++) {
        ember_argv[ember_argc++] = g_strdup(c->extra_args[i]);
    }

    qemu_thread_create(&thread, "ember-machine", ember_machine_thread, NULL, QEMU_THREAD_DETACHED);
    return true;
}

int ember_xemu_state(void)
{
    return qatomic_read(&ember_state);
}

/* The calls below come from app threads; the work is done on QEMU's main loop. */

static void ember_bh_pause(void *opaque)
{
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    }
}

static void ember_bh_resume(void *opaque)
{
    if (!runstate_is_running()) {
        vm_start();
    }
}

static void ember_bh_stop(void *opaque)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

void ember_xemu_set_paused(bool paused)
{
    if (ember_xemu_state() == EMBER_XEMU_RUNNING) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), paused ? ember_bh_pause : ember_bh_resume, NULL);
    }
}

void ember_xemu_request_stop(void)
{
    if (ember_xemu_state() == EMBER_XEMU_RUNNING) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), ember_bh_stop, NULL);
    }
}
