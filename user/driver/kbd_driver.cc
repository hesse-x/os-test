// Keyboard driver (user-space): evdev-style input protocol.
// on_key_event reads HID reports via get_keycode() and fills input_event_t.
#include <stdint.h>
#include <sys/mman.h>
#include "input.h"
#include "xos/input.h"
#include "xos/shm.h"
#include "syscall.h"
#include "usb_hid.h"

// on_key_event: each callback fills one event, returns 1=has event / 0=HID empty.
// Library loops calling this and writes each event to all consumer rings + notify.
static int on_key_event(input_event_t *ev) {
    struct key_event ke;
    if (get_keycode(&ke) != 0) return 0;  // HID ring empty
    ev->timestamp_ns = sys_gettime();
    ev->type  = EV_KEY;
    ev->code  = ke.key;       // KEY_A, KEY_B, ... (already evdev-aligned in input.h)
    ev->value = ke.pressed;   // 1=press, 0=release
    return 1;
}

static void kbd_hid_init(void *hid_shm) {
    get_keycode_init(hid_shm);
}

int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv; (void)envp;
    // Library opens /dev/usb_hid_kbd, mmaps HID SHM, registers /dev/kbd (no shm),
    // enters main loop. on_key_event called repeatedly to drain HID reports.
    input_driver_run(INPUT_DEV_KBD, "kbd", "/dev/usb_hid_kbd", on_key_event, kbd_hid_init);
    return 0;
}
