// Keyboard driver process (user-space)
// 3-layer architecture: acquire → translate → push
// Supports extended keys (0xE0 prefix), Shift/CapsLock, bind/unbind RPC protocol
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/irq.h>
#include <sys/device.h>
#include <unistd.h>
#include "common/shm.h"
#include "common/dev.h"
#include "input.h"
#include "common/errno.h"

static volatile kbd_ring *kbd;
static volatile driver_shm_header *shm_hdr;
static int32_t consumer_pid = -1;

// ===================== Translation state =====================

struct kbd_state {
    bool shift_pressed;
    bool capslock_on;
    bool ctrl_pressed;
    bool alt_pressed;
    bool e0_pending;   // 0xE0 prefix seen, next scancode is extended
};

static struct kbd_state kst;

// Scancode Set 1 → input_key mapping (make codes, 128 entries)
static const uint16_t scancode_to_key[128] = {
    KEY_RESERVED, KEY_ESC,       KEY_1,    KEY_2,    KEY_3,    KEY_4,    KEY_5,    KEY_6,
    KEY_7,        KEY_8,         KEY_9,    KEY_0,    KEY_MINUS,KEY_EQUAL,KEY_BACKSPACE, KEY_TAB,
    KEY_Q,        KEY_W,         KEY_E,    KEY_R,    KEY_T,    KEY_Y,    KEY_U,    KEY_I,
    KEY_O,        KEY_P,         KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_ENTER, KEY_LEFTCTRL,
    KEY_A,        KEY_S,         KEY_D,    KEY_F,    KEY_G,    KEY_H,    KEY_J,    KEY_K,
    KEY_L,        KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_LEFTSHIFT, KEY_BACKSLASH,
    KEY_Z,        KEY_X,         KEY_C,    KEY_V,    KEY_B,    KEY_N,    KEY_M,    KEY_COMMA,
    KEY_DOT,      KEY_SLASH,     KEY_RIGHTSHIFT, 0, KEY_LEFTALT, KEY_SPACE, KEY_CAPSLOCK,
    KEY_F1,       KEY_F2,        KEY_F3,   KEY_F4,   KEY_F5,   KEY_F6,   KEY_F7,   KEY_F8,
    KEY_F9,       KEY_F10,       KEY_NUMLOCK, KEY_SCROLLLOCK, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, KEY_F11, KEY_F12,
};

// Extended key (0xE0 prefix) scancode → input_key mapping
static const struct { uint8_t scancode; uint16_t key; } ext_key_map[] = {
    { 0x1D, KEY_RIGHTCTRL },  // not tracked yet, but mapped
    { 0x38, KEY_LEFTALT },    // right alt — treat same for now
    { 0x47, KEY_HOME },
    { 0x48, KEY_UP },
    { 0x49, KEY_PAGEUP },
    { 0x4B, KEY_LEFT },
    { 0x4D, KEY_RIGHT },
    { 0x4F, KEY_END },
    { 0x50, KEY_DOWN },
    { 0x51, KEY_PAGEDOWN },
    { 0x52, KEY_INSERT },
    { 0x53, KEY_DELETE },
    { 0, 0 }  // sentinel
};

// ===================== Layer 1: Acquire =====================
// Read scancodes from keyboard controller into buffer.
// Returns number of scancodes read.
static int kbd_irq_acquire(uint8_t *scancodes, int max) {
    int count = 0;
    while ((inb(0x64) & 0x01) && count < max) {
        scancodes[count++] = inb(0x60);
    }
    return count;
}

// ===================== Layer 2: Translate =====================
// Translate one scancode into a key_event.
// Returns 1 if a key event was produced, 0 if only modifier state changed.
static int kbd_translate(uint8_t scancode, struct kbd_state *st, struct key_event *ev) {
    // Extended key prefix
    if (scancode == 0xE0) {
        st->e0_pending = true;
        return 0;
    }

    bool is_extended = st->e0_pending;
    st->e0_pending = false;

    // Break code (bit 7 set): key release
    if (scancode & 0x80) {
        uint8_t make = scancode & 0x7F;

        if (is_extended) {
            // Extended key release — not tracked for modifiers
            return 0;
        }

        if (make == 0x2A || make == 0x36) {
            st->shift_pressed = false;
            return 0;
        }
        if (make == 0x1D) {
            st->ctrl_pressed = false;
            return 0;
        }
        if (make == 0x38) {
            st->alt_pressed = false;
            return 0;
        }
        // Other key releases: ignored (no key-up events sent to ring)
        return 0;
    }

    // Make code: key press
    if (is_extended) {
        // Look up extended key
        for (int i = 0; ext_key_map[i].scancode; i++) {
            if (ext_key_map[i].scancode == scancode) {
                ev->key = ext_key_map[i].key;
                ev->pressed = 1;
                ev->modifiers = (st->shift_pressed ? MOD_SHIFT : 0) |
                                (st->ctrl_pressed ? MOD_CTRL : 0) |
                                (st->alt_pressed ? MOD_ALT : 0) |
                                (st->capslock_on ? MOD_CAPS : 0);
                return 1;
            }
        }
        return 0;  // unmapped extended key
    }

    // Non-extended make codes: modifier keys
    if (scancode == 0x2A || scancode == 0x36) {
        st->shift_pressed = true;
        return 0;
    }
    if (scancode == 0x3A) {
        st->capslock_on = !st->capslock_on;
        return 0;
    }
    if (scancode == 0x1D) {
        st->ctrl_pressed = true;
        return 0;
    }
    if (scancode == 0x38) {
        st->alt_pressed = true;
        return 0;
    }

    // Regular key: look up in table
    if (scancode >= 128) return 0;
    uint16_t key = scancode_to_key[scancode];
    if (key == KEY_RESERVED) return 0;

    ev->key = key;
    ev->pressed = 1;
    ev->modifiers = (st->shift_pressed ? MOD_SHIFT : 0) |
                    (st->ctrl_pressed ? MOD_CTRL : 0) |
                    (st->alt_pressed ? MOD_ALT : 0) |
                    (st->capslock_on ? MOD_CAPS : 0);
    return 1;
}

// ===================== Layer 3: Push =====================
// Convert key_event to ASCII or ESC sequence and write to kbd_ring.

// Normal key → ASCII
static const uint8_t key_to_ascii_normal[] = {
    // KEY_ESC=1 → 27
    // KEY_1..KEY_0 = 2..11 → '1'..'0'
    // KEY_MINUS=12 → '-', KEY_EQUAL=13 → '=', KEY_BACKSPACE=14 → 8
    // KEY_TAB=15 → 9, KEY_Q..KEY_P = 16..25 → 'q'..'p'
    // KEY_LEFTBRACE=26 → '[', KEY_RIGHTBRACE=27 → ']', KEY_ENTER=28 → '\n'
    // KEY_A..KEY_L = 30..38 → 'a'..'l'
    // KEY_SEMICOLON=39 → ';', KEY_APOSTROPHE=40 → '\'', KEY_GRAVE=41 → '`'
    // KEY_BACKSLASH=43 → '\\'
    // KEY_Z..KEY_M = 44..50 → 'z'..'m'
    // KEY_COMMA=51 → ',', KEY_DOT=52 → '.', KEY_SLASH=53 → '/'
    // KEY_SPACE=57 → ' '
};

// Helper: write one byte to kbd_ring
static void kbd_write(uint8_t ch) {
    uint32_t head = kbd->head;
    uint32_t next = (head + 1) % 8;
    if (next != kbd->tail) {  // ring not full
        kbd->msgs[head].type = 1;
        kbd->msgs[head].ch = ch;
        kbd->head = next;
    }
}

// Helper: write ESC sequence string (null-terminated)
static void kbd_write_esc(const char *seq) {
    while (*seq) kbd_write((uint8_t)*seq++);
}

// Map a key_event to ASCII or ESC sequence and push to ring
static void kbd_push(struct key_event *ev) {
    // Ctrl+key: produce control character
    if (ev->modifiers & MOD_CTRL) {
        // Ctrl+A..Ctrl+Z → 0x01..0x1A
        // Must check all letter keys, not just KEY_A..KEY_Z range
        uint8_t letter = 0;
        switch (ev->key) {
            case KEY_Q: letter = 'q'; break; case KEY_W: letter = 'w'; break;
            case KEY_E: letter = 'e'; break; case KEY_R: letter = 'r'; break;
            case KEY_T: letter = 't'; break; case KEY_Y: letter = 'y'; break;
            case KEY_U: letter = 'u'; break; case KEY_I: letter = 'i'; break;
            case KEY_O: letter = 'o'; break; case KEY_P: letter = 'p'; break;
            case KEY_A: letter = 'a'; break; case KEY_S: letter = 's'; break;
            case KEY_D: letter = 'd'; break; case KEY_F: letter = 'f'; break;
            case KEY_G: letter = 'g'; break; case KEY_H: letter = 'h'; break;
            case KEY_J: letter = 'j'; break; case KEY_K: letter = 'k'; break;
            case KEY_L: letter = 'l'; break; case KEY_Z: letter = 'z'; break;
            case KEY_X: letter = 'x'; break; case KEY_C: letter = 'c'; break;
            case KEY_V: letter = 'v'; break; case KEY_B: letter = 'b'; break;
            case KEY_N: letter = 'n'; break; case KEY_M: letter = 'm'; break;
            default: break;
        }
        if (letter >= 'a' && letter <= 'z') {
            kbd_write((uint8_t)(letter - 'a' + 1));
            return;
        }
    }

    // Extended keys: ESC sequences
    switch (ev->key) {
        case KEY_UP:       kbd_write_esc("\033[A");  return;
        case KEY_DOWN:     kbd_write_esc("\033[B");  return;
        case KEY_RIGHT:    kbd_write_esc("\033[C");  return;
        case KEY_LEFT:     kbd_write_esc("\033[D");  return;
        case KEY_HOME:     kbd_write_esc("\033[H");  return;
        case KEY_END:      kbd_write_esc("\033[F");  return;
        case KEY_INSERT:   kbd_write_esc("\033[2~"); return;
        case KEY_DELETE:   kbd_write_esc("\033[3~"); return;
        case KEY_PAGEUP:   kbd_write_esc("\033[5~"); return;
        case KEY_PAGEDOWN: kbd_write_esc("\033[6~"); return;
        case KEY_F1:       kbd_write_esc("\033OP");  return;
        case KEY_F2:       kbd_write_esc("\033OQ");  return;
        case KEY_F3:       kbd_write_esc("\033OR");  return;
        case KEY_F4:       kbd_write_esc("\033OS");  return;
        case KEY_F5:       kbd_write_esc("\033[15~"); return;
        case KEY_F6:       kbd_write_esc("\033[17~"); return;
        case KEY_F7:       kbd_write_esc("\033[18~"); return;
        case KEY_F8:       kbd_write_esc("\033[19~"); return;
        case KEY_F9:       kbd_write_esc("\033[20~"); return;
        case KEY_F10:      kbd_write_esc("\033[21~"); return;
        case KEY_F11:      kbd_write_esc("\033[23~"); return;
        case KEY_F12:      kbd_write_esc("\033[24~"); return;
        default: break;
    }

    // Esc key
    if (ev->key == KEY_ESC) {
        kbd_write(27);
        return;
    }

    // Printable keys: map to ASCII
    bool shift = ev->modifiers & MOD_SHIFT;
    bool caps = ev->modifiers & MOD_CAPS;

    // Direct ASCII mapping by key code
    uint8_t ch = 0;
    switch (ev->key) {
        case KEY_1:    ch = shift ? '!' : '1'; break;
        case KEY_2:    ch = shift ? '@' : '2'; break;
        case KEY_3:    ch = shift ? '#' : '3'; break;
        case KEY_4:    ch = shift ? '$' : '4'; break;
        case KEY_5:    ch = shift ? '%' : '5'; break;
        case KEY_6:    ch = shift ? '^' : '6'; break;
        case KEY_7:    ch = shift ? '&' : '7'; break;
        case KEY_8:    ch = shift ? '*' : '8'; break;
        case KEY_9:    ch = shift ? '(' : '9'; break;
        case KEY_0:    ch = shift ? ')' : '0'; break;
        case KEY_MINUS:    ch = shift ? '_' : '-'; break;
        case KEY_EQUAL:    ch = shift ? '+' : '='; break;
        case KEY_BACKSPACE: ch = 8; break;
        case KEY_TAB:      ch = 9; break;
        case KEY_LEFTBRACE:  ch = shift ? '{' : '['; break;
        case KEY_RIGHTBRACE: ch = shift ? '}' : ']'; break;
        case KEY_ENTER:      ch = '\n'; break;
        case KEY_SEMICOLON:  ch = shift ? ':' : ';'; break;
        case KEY_APOSTROPHE: ch = shift ? '"' : '\''; break;
        case KEY_GRAVE:      ch = shift ? '~' : '`'; break;
        case KEY_BACKSLASH:  ch = shift ? '|' : '\\'; break;
        case KEY_COMMA:  ch = shift ? '<' : ','; break;
        case KEY_DOT:    ch = shift ? '>' : '.'; break;
        case KEY_SLASH:  ch = shift ? '?' : '/'; break;
        case KEY_SPACE:  ch = ' '; break;
        default: break;
    }

    // Letter keys: apply shift/caps
    // Keys are numbered by keyboard position, not alphabetically:
    // KEY_Q=16..KEY_P=25 (top row), KEY_A=30..KEY_L=38 (home row),
    // KEY_Z=44..KEY_M=50 (bottom row). All are in 16..50 range.
    // Non-letter keys in that range: KEY_LEFTBRACE(26), KEY_RIGHTBRACE(27),
    // KEY_ENTER(28), KEY_LEFTCTRL(29), KEY_SEMICOLON(39), KEY_APOSTROPHE(40),
    // KEY_GRAVE(41), KEY_LEFTSHIFT(42), KEY_BACKSLASH(43), KEY_COMMA(51),
    // KEY_DOT(52), KEY_SLASH(53), KEY_RIGHTSHIFT(54), KEY_LEFTALT(56),
    // KEY_SPACE(57), KEY_CAPSLOCK(58), KEY_F1-F10(59-68)
    //
    // So we just check each key individually with a switch:
    switch (ev->key) {
        case KEY_Q: ch = 'q'; break; case KEY_W: ch = 'w'; break;
        case KEY_E: ch = 'e'; break; case KEY_R: ch = 'r'; break;
        case KEY_T: ch = 't'; break; case KEY_Y: ch = 'y'; break;
        case KEY_U: ch = 'u'; break; case KEY_I: ch = 'i'; break;
        case KEY_O: ch = 'o'; break; case KEY_P: ch = 'p'; break;
        case KEY_A: ch = 'a'; break; case KEY_S: ch = 's'; break;
        case KEY_D: ch = 'd'; break; case KEY_F: ch = 'f'; break;
        case KEY_G: ch = 'g'; break; case KEY_H: ch = 'h'; break;
        case KEY_J: ch = 'j'; break; case KEY_K: ch = 'k'; break;
        case KEY_L: ch = 'l'; break; case KEY_Z: ch = 'z'; break;
        case KEY_X: ch = 'x'; break; case KEY_C: ch = 'c'; break;
        case KEY_V: ch = 'v'; break; case KEY_B: ch = 'b'; break;
        case KEY_N: ch = 'n'; break; case KEY_M: ch = 'm'; break;
        default: break;
    }
    if (ch >= 'a' && ch <= 'z' && (shift ^ caps)) ch -= 32;

    if (ch) kbd_write(ch);
}

// ===================== REQ handlers =====================

static void handle_req(struct recv_msg *msg) {
    struct kbd_req_request *req = (struct kbd_req_request *)msg->data;
    struct kbd_req_reply reply;
    for (int i = 0; i < 64; i++) ((uint8_t*)&reply)[i] = 0;

    if (req->opcode == KBD_REQ_BIND) {
        if (consumer_pid >= 0 && consumer_pid != (int32_t)msg->src) {
            reply.result = -EBUSY;
        } else {
            if (consumer_pid < 0) {
                // First bind: create SHM
                void *shm_ptr = NULL;
                shm_create(4096, &shm_ptr);
                uint64_t shm_addr = (uint64_t)shm_ptr;
                kbd = (volatile kbd_ring *)(shm_addr + KBD_RING_OFFSET);
                shm_hdr = (volatile driver_shm_header *)shm_addr;
                kbd->head = 0;
                kbd->tail = 0;
                shm_hdr->kbd_sleeping = 0;
                shm_hdr->consumer_sleeping = 0;
                consumer_pid = msg->src;
            }
            reply.result = 0;  // idempotent success
        }
    } else if (req->opcode == KBD_REQ_UNBIND) {
        consumer_pid = -1;
        kbd = nullptr;
        shm_hdr = nullptr;
        reply.result = 0;
    } else {
        reply.result = -EINVAL;
    }

    resp(&reply);
}

// ===================== Main =====================

extern "C" void _start() {
    // Bind to keyboard IRQ (IRQ1 = vector 33)
    irq_bind(33);

    // Register as KBD device
    device_register(getpid(), DEV_KBD);

    // Initialize translation state
    for (uint8_t *p = (uint8_t *)&kst; p < (uint8_t *)(&kst + 1); *p++ = 0);

    // Main loop: wait for events (IRQ or REQ)
    while (1) {
        struct recv_msg msg;
        recv(&msg, NULL, 0, 0);

        if (msg.type == RECV_REQ) {
            handle_req(&msg);
            continue;
        }

        // RECV_IRQ or RECV_NOTIFY: process if we have a consumer
        if (msg.type == RECV_IRQ && consumer_pid >= 0 && kbd) {
            uint8_t scancodes[16];
            int count = kbd_irq_acquire(scancodes, 16);

            for (int i = 0; i < count; i++) {
                struct key_event ev;
                if (kbd_translate(scancodes[i], &kst, &ev)) {
                    kbd_push(&ev);
                }
            }

            // Notify consumer if sleeping
            if (shm_hdr && shm_hdr->consumer_sleeping) {
                notify(consumer_pid);
            }
        }
        // RECV_IRQ without consumer: skip (don't read hardware, don't push)
        // RECV_NOTIFY: ignore
    }
}
