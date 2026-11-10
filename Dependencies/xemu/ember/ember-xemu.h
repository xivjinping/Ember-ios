/*
 * Ember: the C interface of the xemu library built for iOS.
 *
 * This is the only header the app's backend (Core/src/backend/xemu_backend.cpp)
 * includes; it deliberately uses no QEMU types. Implemented in ember-core.c
 * and ember-frontend.c. GPL-2.0, like the rest of xemu.
 */
#ifndef EMBER_XEMU_H
#define EMBER_XEMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ember_xemu_config {
    const char *settings_path; /* xemu's TOML settings file; need not exist */
    const char *bootrom_path;  /* MCPX boot ROM, 512 bytes */
    const char *flashrom_path; /* BIOS image */
    const char *hdd_path;      /* hard-disk image (qcow2 or raw) */
    const char *dvd_path;      /* disc image, or "" for an empty drive */
    const char *eeprom_path;   /* 256 bytes; generated when missing */
    int ram_mb;                /* 64 or 128 */
    int controllers;           /* gamepads plugged in at power-on, 0..4 */
    const char *const *extra_args; /* further QEMU arguments, NULL-terminated, or NULL */
} ember_xemu_config;

enum {
    EMBER_XEMU_IDLE = 0,     /* never started */
    EMBER_XEMU_STARTING = 1, /* machine is being built */
    EMBER_XEMU_RUNNING = 2,  /* main loop is running */
    EMBER_XEMU_EXITED = 3,   /* main loop returned; cannot be started again */
};

/*
 * Builds the machine and runs it on its own thread. Returns at once. QEMU
 * keeps process-wide state, so this works once per process: a second call
 * returns false. Fatal configuration errors inside QEMU end the process, so
 * the caller checks the files first.
 */
bool ember_xemu_start(const ember_xemu_config *config);

/*
 * Gives QEMU's code generator the memory it writes machine code into: two
 * views of the same pages, `rx` executable and `rw` writable (Ember's JIT
 * region, docs/JIT.md). Must be called before ember_xemu_start; iOS allows
 * no other way to get executable memory, so without it the start fails.
 */
void ember_xemu_set_code_buffer(void *rx, void *rw, size_t size);

/*
 * Where the guest CPU was last seen executing (linear address), or 0 when
 * the machine is not running. A cheap sign of life while there is no picture.
 */
uint64_t ember_xemu_guest_pc(void);
int ember_xemu_state(void);
void ember_xemu_set_paused(bool paused);
void ember_xemu_request_stop(void);

/*
 * The guest's display as 32-bit BGRA, top row first. `serial` goes up each
 * time the picture changes. Returns false, without locking, until there is
 * a picture. Every successful lock needs one unlock.
 */
bool ember_xemu_lock_frame(const uint8_t **bgra, uint32_t *width, uint32_t *height, uint64_t *serial);
void ember_xemu_unlock_frame(void);

/* Button bits and axis order as in xemu's ui/xemu-input.h. */
enum {
    EMBER_BTN_A = 1 << 0, EMBER_BTN_B = 1 << 1, EMBER_BTN_X = 1 << 2, EMBER_BTN_Y = 1 << 3,
    EMBER_BTN_DPAD_LEFT = 1 << 4, EMBER_BTN_DPAD_UP = 1 << 5,
    EMBER_BTN_DPAD_RIGHT = 1 << 6, EMBER_BTN_DPAD_DOWN = 1 << 7,
    EMBER_BTN_BACK = 1 << 8, EMBER_BTN_START = 1 << 9,
    EMBER_BTN_WHITE = 1 << 10, EMBER_BTN_BLACK = 1 << 11,
    EMBER_BTN_LSTICK = 1 << 12, EMBER_BTN_RSTICK = 1 << 13,
};
enum {
    EMBER_AXIS_LTRIG, EMBER_AXIS_RTRIG, /* 0..32767 */
    EMBER_AXIS_LSTICK_X, EMBER_AXIS_LSTICK_Y, EMBER_AXIS_RSTICK_X, EMBER_AXIS_RSTICK_Y,
    EMBER_AXIS_COUNT
};

/* May be called from any thread. port is 0..3. */
void ember_input_set(int port, uint16_t buttons, const int16_t axes[EMBER_AXIS_COUNT]);
void ember_input_get_rumble(int port, uint16_t *left, uint16_t *right);

#ifdef __cplusplus
}
#endif
#endif
