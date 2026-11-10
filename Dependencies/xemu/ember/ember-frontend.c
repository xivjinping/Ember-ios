/*
 * Ember: the parts of xemu's desktop front end that the emulation core still
 * calls on iOS.
 *
 * On desktop these live in ui/xemu-input.c, ui/xemu-notifications.cc and
 * ui/xemu-snapshots.c, which draw windows, read SDL gamepads and use OpenGL.
 * None of that exists in Ember: the SwiftUI app owns the window and the
 * controls. This file supplies the same functions with the behaviour the
 * core needs, and a small API (ember_input_*, declared in ember-xemu.h) for
 * the app to feed it.
 *
 * Copied to ui/ember-frontend.c by Dependencies/xemu/patch_xemu.py.
 * GPL-2.0, like the rest of xemu.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "net/clients.h"
#include "migration/qemu-file.h"

#include "ui/xemu-input.h"
#include "ui/xemu-notifications.h"
#include "ui/xemu-snapshots.h"
#include "ui/ember-xemu.h"

#define EMBER_PORTS 4

/* ---- Controllers ------------------------------------------------------------ */

/* One state per controller port. The app writes it, the USB gamepad reads it. */
static ControllerState ember_ports[EMBER_PORTS];

ControllerState *xemu_input_get_bound(int index)
{
    if (index < 0 || index >= EMBER_PORTS) {
        return NULL;
    }
    return &ember_ports[index];
}

/* Nothing to poll: the app pushes state with ember_input_set(). */
void xemu_input_update_controller(ControllerState *state)
{
    (void)state;
}

/* Rumble is read back by the app with ember_input_get_rumble(). */
void xemu_input_update_rumble(ControllerState *state)
{
    (void)state;
}

/* xemu's "test the controller while a game runs" screen does not exist here. */
int xemu_input_get_test_mode(void)
{
    return 0;
}

/* Called by the app (any thread) with the buttons and axes of one port. */
void ember_input_set(int port, uint16_t buttons, const int16_t axes[CONTROLLER_AXIS__COUNT])
{
    ControllerState *s = xemu_input_get_bound(port);
    if (!s) {
        return;
    }
    s->buttons = buttons;
    for (int i = 0; i < CONTROLLER_AXIS__COUNT; i++) {
        s->axis[i] = axes[i];
    }
}

/* Current rumble motors of one port, 0..65535 each. */
void ember_input_get_rumble(int port, uint16_t *left, uint16_t *right)
{
    ControllerState *s = xemu_input_get_bound(port);
    *left = s ? s->rumble_l : 0;
    *right = s ? s->rumble_r : 0;
}

/* ---- Notifications ------------------------------------------------------------ */

/* For now they go to the log; the app can show them later. */
void xemu_queue_notification(const char *msg)
{
    info_report("%s", msg);
}

void xemu_queue_error_message(const char *msg)
{
    error_report("%s", msg);
}

/* ---- Snapshots ------------------------------------------------------------------ */

/*
 * xemu stores a thumbnail and the disc path ahead of the machine state in a
 * snapshot. Ember writes plain machine state without that header, and
 * therefore also reads none.
 */
void xemu_snapshots_save_extra_data(QEMUFile *f)
{
    (void)f;
}

bool xemu_snapshots_offset_extra_data(QEMUFile *f)
{
    (void)f;
    return true;
}

void xemu_snapshots_mark_dirty(void)
{
}

/* ---- Packet capture --------------------------------------------------------------- */

/* iOS has no libpcap (see the pcap edits in patch_xemu.py). */
int net_init_pcap(const Netdev *netdev, const char *name,
                  NetClientState *peer, Error **errp)
{
    error_setg(errp, "packet capture is not available on iOS");
    return -1;
}
