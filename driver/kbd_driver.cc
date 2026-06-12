// Keyboard driver process (user-space)
// Binds to IRQ 1, reads scan codes from port 0x60, writes to kbd_ring
// Supports Shift (left/right) and CapsLock for uppercase/lowercase input
// Uses dynamic shared memory (sys_shm_create) + sleeping flag protocol
#include <stdint.h>
#include "arch/x64/utils.h"
#include "common/shm.h"
#include "common/pid.h"

static volatile kbd_ring *kbd;
static volatile driver_shm_header *shm_hdr;

static bool shift_pressed = false;
static bool capslock_on = false;

// Normal scancode to ASCII (set 1, make codes only)
static const uint8_t scancode_normal[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', 8,
    9, 'q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0, ' '
};

// Shifted scancode to ASCII (for non-letter keys: number row + punctuation)
static const uint8_t scancode_shifted[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', 8,
    9, 'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0, ' '
};

static void kbd_write(uint8_t ch) {
    uint32_t head = kbd->head;
    uint32_t next = (head + 1) % 8;
    if (next != kbd->tail) {  // ring not full
        kbd->msgs[head].type = 1;
        kbd->msgs[head].ch = ch;
        kbd->head = next;
    }
}

extern "C" void _start() {
    // Bind to keyboard IRQ (IRQ1 = vector 33)
    sys_irq_bind(33);

    int32_t consumer_pid = TERMINAL_PID;

    // Create shared memory page for KBD+KMS communication
    uint64_t shm_addr = (uint64_t)sys_shm_create(4096);
    kbd = (volatile kbd_ring *)(shm_addr + KBD_RING_OFFSET);
    shm_hdr = (volatile driver_shm_header *)shm_addr;

    // Initialize ring buffer
    kbd->head = 0;
    kbd->tail = 0;
    shm_hdr->kbd_sleeping = 0;
    shm_hdr->consumer_sleeping = 0;
    shm_hdr->kms_sleeping = 0;

    // Pure interrupt-driven loop: sleep → IRQ wakes → drain all scancodes → sleep
    while (1) {
        sys_wait(0);

        // Drain all pending scancodes from keyboard controller
        // (one IRQ may produce multiple scancodes)
        while (inb(0x64) & 0x01) {
            uint8_t scancode = inb(0x60);

            // Break codes (bit 7 set): release modifier keys
            if (scancode & 0x80) {
                uint8_t make = scancode & 0x7F;
                if (make == 0x2A || make == 0x36)  // left/right shift release
                    shift_pressed = false;
                continue;
            }

            // Make codes for modifier keys
            if (scancode == 0x2A || scancode == 0x36) {  // left/right shift press
                shift_pressed = true;
                continue;
            }
            if (scancode == 0x3A) {  // CapsLock press (toggle)
                capslock_on = !capslock_on;
                continue;
            }

            // Produce character
            uint8_t ch = scancode_normal[scancode];
            if (ch == 0) continue;

            if (ch >= 'a' && ch <= 'z') {
                if (shift_pressed ^ capslock_on) ch -= 32;
            } else {
                if (shift_pressed) ch = scancode_shifted[scancode];
            }

            kbd_write(ch);
        }

        // Notify consumer (shell) if we wrote anything
        if (shm_hdr->consumer_sleeping) {
            sys_notify(consumer_pid);
        }
    }
}
