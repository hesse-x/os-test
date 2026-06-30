#ifndef USER_INPUT_H
#define USER_INPUT_H

#include <stdint.h>

// Modifier key flags
#define MOD_SHIFT  0x01
#define MOD_CTRL   0x02
#define MOD_ALT    0x04
#define MOD_CAPS   0x08

// Key event intermediate representation
struct key_event {
    uint16_t key;       // input_key enum value
    uint8_t  pressed;   // 1=key down, 0=key up
    uint8_t  modifiers; // modifier key state bitmap
};

// Unified input key enum (aligned with Linux input-event-codes.h numbering)
enum input_key {
    KEY_RESERVED = 0,
    KEY_ESC = 1,
    KEY_1 = 2,  KEY_2 = 3,  KEY_3 = 4,  KEY_4 = 5,  KEY_5 = 6,
    KEY_6 = 7,  KEY_7 = 8,  KEY_8 = 9,  KEY_9 = 10, KEY_0 = 11,
    KEY_MINUS = 12, KEY_EQUAL = 13,
    KEY_BACKSPACE = 14, KEY_TAB = 15,
    KEY_Q = 16, KEY_W = 17, KEY_E = 18, KEY_R = 19, KEY_T = 20,
    KEY_Y = 21, KEY_U = 22, KEY_I = 23, KEY_O = 24, KEY_P = 25,
    KEY_LEFTBRACE = 26, KEY_RIGHTBRACE = 27, KEY_ENTER = 28,
    KEY_LEFTCTRL = 29,
    KEY_A = 30, KEY_S = 31, KEY_D = 32, KEY_F = 33, KEY_G = 34,
    KEY_H = 35, KEY_J = 36, KEY_K = 37, KEY_L = 38,
    KEY_SEMICOLON = 39, KEY_APOSTROPHE = 40, KEY_GRAVE = 41,
    KEY_LEFTSHIFT = 42, KEY_BACKSLASH = 43,
    KEY_Z = 44, KEY_X = 45, KEY_C = 46, KEY_V = 47, KEY_B = 48,
    KEY_N = 49, KEY_M = 50,
    KEY_COMMA = 51, KEY_DOT = 52, KEY_SLASH = 53,
    KEY_RIGHTSHIFT = 54, KEY_LEFTALT = 56, KEY_SPACE = 57,
    KEY_CAPSLOCK = 58,
    KEY_F1 = 59,  KEY_F2 = 60,  KEY_F3 = 61,  KEY_F4 = 62,
    KEY_F5 = 63,  KEY_F6 = 64,  KEY_F7 = 65,  KEY_F8 = 66,
    KEY_F9 = 67,  KEY_F10 = 68, KEY_F11 = 87, KEY_F12 = 88,
    KEY_NUMLOCK = 69, KEY_SCROLLLOCK = 70,
    // Extended keys (0xE0 prefix)
    KEY_HOME = 102, KEY_UP = 103, KEY_PAGEUP = 104,
    KEY_LEFT = 105, KEY_RIGHT = 106,
    KEY_END = 107, KEY_DOWN = 108, KEY_PAGEDOWN = 109,
    KEY_INSERT = 110, KEY_DELETE = 111,
    KEY_RIGHTCTRL = 114, KEY_RIGHTALT = 100,
    // Future gamepad buttons start at 0x100
    BTN_BASE = 0x100,
};

// evdev-aligned button/axis codes (linux/input-event-codes.h subset)
#define BTN_LEFT     0x110
#define BTN_RIGHT    0x111
#define BTN_MIDDLE   0x112
#define BTN_TOUCH    0x14A
#define BTN_TOOL_FINGER 0x14B

#define REL_X        0x00
#define REL_Y        0x01
#define REL_WHEEL    0x08

#define ABS_X        0x00
#define ABS_Y        0x01
#define ABS_PRESSURE 0x18

#include "common/ioctl.h"
#include "common/input.h"

// ===================== libinput_client public API =====================

// Poll ring: drain all pending events into caller buffer (pure drain, no sleeping flag).
// Returns number of events read (0 if ring empty).
int input_client_poll(volatile void *shm, input_event_t *events, int max_events);

// Helper: input_event → basic single-byte ASCII (lowercase letters, digits, symbols without shift,
// space/enter/bs/tab/esc). shift/caps/ESC-seq/Ctrl combos are caller's responsibility.
// Returns bytes written to buf (0 = no basic ASCII mapping for this event).
int input_event_to_ascii(const input_event_t *ev, uint8_t *buf, int buf_len);

// ===================== libinput_driver public API =====================

// Init and enter main loop (does not return).
// on_event: each callback fills one event into *ev, returns 1=has event / 0=HID empty;
// library loops calling it, and after each fill writes to all consumer rings + unconditional notify.
// hid_init: optional callback invoked after the library mmaps the HID SHM, receives the shm address.
void input_driver_run(uint32_t device_type, const char *dev_name,
                      const char *hid_dev_path,
                      int (*on_event)(input_event_t *ev),
                      void (*hid_init)(void *hid_shm));

#endif // USER_INPUT_H
