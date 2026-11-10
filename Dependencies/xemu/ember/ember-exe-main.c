/*
 * Ember: entry point of the qemu-system-i386 *executable* on iOS.
 *
 * The app does not use the executable; it links the same objects as a
 * library (Dependencies/xemu/package-xemu-ios.sh) and calls
 * ember_xemu_start() itself. The executable is still linked in CI because
 * that is what proves every symbol resolves for iOS. This file is left out
 * of the library.
 */
#include <unistd.h>
#include "ember-xemu.h"

int main(int argc, char **argv)
{
    ember_xemu_config config = {
        .settings_path = "xemu.toml", .bootrom_path = "", .flashrom_path = "",
        .hdd_path = "", .dvd_path = "", .eeprom_path = "eeprom.bin",
        .ram_mb = 64, .controllers = 1,
    };
    (void)argc;
    (void)argv;
    if (!ember_xemu_start(&config)) {
        return 1;
    }
    while (ember_xemu_state() != EMBER_XEMU_EXITED) {
        sleep(1);
    }
    return 0;
}
