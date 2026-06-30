// Keyboard driver process (user-space)
// USB HID Boot Protocol: reads HID reports from kernel SHM ring via get_keycode()
// Supports bind/unbind REQ protocol, kbd_push converts key_event → ASCII/ESC
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include "common/shm.h"
#include "common/dev.h"
#include "common/syscall.h"
#include "input.h"
#include "usb_hid.h"
#include "common/errno.h"
#include <cerrno>

static volatile kbd_ring *kbd;
static volatile driver_shm_header *shm_hdr;
static int32_t consumer_pid = -1;

// ===================== Layer 3: Push =====================
// Convert key_event to ASCII or ESC sequence and write to kbd_ring.

static void kbd_write(uint8_t ch) {
    uint32_t head = kbd->head;
    uint32_t next = (head + 1) % 8;
    if (next != kbd->tail) {  // ring not full
        kbd->msgs[head].type = 1;
        kbd->msgs[head].ch = ch;
        kbd->head = next;
    }
}

static void kbd_write_esc(const char *seq) {
    while (*seq) kbd_write((uint8_t)*seq++);
}

static void kbd_push(struct key_event *ev) {
    // Only handle key press events (ignore releases)
    if (!ev->pressed) return;

    // Ctrl+key: produce control character
    if (ev->modifiers & MOD_CTRL) {
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
    // First 4 bytes = opcode/cmd
    uint32_t opcode = *(uint32_t *)msg->data;
    struct kbd_req_reply reply;
    for (int i = 0; i < 64; i++) ((uint8_t*)&reply)[i] = 0;

    if (opcode == KBD_REQ_BIND || opcode == KBD_IOCTL_BIND) {
        if (consumer_pid >= 0 && consumer_pid != (int32_t)msg->src) {
            reply.result = -EBUSY;
            // For ioctl _IOWR path: write arg data into reply+4 region
            if (opcode == KBD_IOCTL_BIND) {
                struct kbd_ioctl_bind_arg *out = (struct kbd_ioctl_bind_arg *)((uint8_t *)&reply + 4);
                out->pid = *(uint32_t *)(msg->data + 4);
                out->result = -EBUSY;
            }
        } else {
            // For ioctl-style, pid is at data+4 (first field of kbd_ioctl_bind_arg)
            // For legacy REQ, pid is at opcode+4 in kbd_req_request
            // Both layouts have pid at offset 4 of the data payload
            consumer_pid = (int32_t)(*(uint32_t *)(msg->data + 4));
            if (consumer_pid <= 0) consumer_pid = (int32_t)msg->src;
            reply.result = 0;  // idempotent success
            // For ioctl _IOWR path: write arg data into reply+4 region
            if (opcode == KBD_IOCTL_BIND) {
                struct kbd_ioctl_bind_arg *out = (struct kbd_ioctl_bind_arg *)((uint8_t *)&reply + 4);
                out->pid = (uint32_t)consumer_pid;
                out->result = 0;
            }
        }
    } else if (opcode == KBD_REQ_UNBIND || opcode == KBD_IOCTL_UNBIND) {
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
    // Create kbd_ring SHM via memfd_create + ftruncate
    int shm_fd = memfd_create("kbd_ring", 0);
    ftruncate(shm_fd, 4096);
    void *shm_ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    uint64_t shm_addr = (uint64_t)shm_ptr;
    kbd = (volatile kbd_ring *)(shm_addr + KBD_RING_OFFSET);
    shm_hdr = (volatile driver_shm_header *)shm_addr;
    kbd->head = 0;
    kbd->tail = 0;
    shm_hdr->kbd_sleeping = 0;
    shm_hdr->consumer_sleeping = 0;

    // Open /dev/usb_hid and mmap the kernel HID SHM
    int hid_fd = open("/dev/usb_hid", O_RDWR);
    void *hid_shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, hid_fd, 0);
    get_keycode_init(hid_shm);

    // Register as KBD device, binding kbd_ring SHM to the /dev/kbd inode
    device_register_shm("kbd", DEV_KBD, shm_fd);

    // Main loop: wait for events (EINTR from kernel ISR, or REQ from consumers)
    while (1) {
        struct recv_msg msg;
        int rc = recv(&msg, NULL, 0, 0);

        if (rc < 0) {
            // EINTR: ISR woke us — process HID reports if we have a consumer
            if (errno != EINTR) continue;
            if (consumer_pid >= 0 && kbd) {
                struct key_event ev;
                while (get_keycode(&ev) == 0) {
                    kbd_push(&ev);
                }

                // Notify consumer if sleeping
                if (shm_hdr && shm_hdr->consumer_sleeping) {
                    notify(consumer_pid);
                }
            }
            continue;
        }

        if (msg.type == RECV_REQ) {
            handle_req(&msg);
            continue;
        }
    }
}
