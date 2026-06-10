// Keyboard driver process (user-space)
// Binds to IRQ 1, reads scan codes from port 0x60, writes to KBD_SHM
// Supports Shift (left/right) and CapsLock for uppercase/lowercase input
#include <stdint.h>
#include "arch/x64/utils.h"
#include "common/shm.h"

static volatile kbd_shm *kbd = (volatile kbd_shm *)KBD_SHM_ADDR;

#define KBD_BUF_SIZE 4088

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
// Letters are handled by shift/capslock logic on the normal table
static const uint8_t scancode_shifted[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', 8,
    9, 'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0, ' '
};

static void kbd_write(uint8_t ch) {
    uint32_t head = kbd->head;
    uint32_t next = (head + 1) % KBD_BUF_SIZE;
    if (next != kbd->tail) {  // buffer not full
        kbd->data[head] = ch;
        kbd->head = next;
    }
}

extern "C" void _start() {
    // Bind to keyboard IRQ (IRQ1 = vector 33)
    sys_irq_bind(33);

    int32_t my_pid = (int32_t)sys_getpid();
    int32_t shell_pid = my_pid + 1;

    while (1) {
        // Wait for keyboard interrupt
        sys_wait();

        // Read from keyboard controller
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
        if (ch == 0) continue;  // unknown key

        if (ch >= 'a' && ch <= 'z') {
            // Letter: uppercase if (shift XOR capslock)
            if (shift_pressed ^ capslock_on) ch -= 32;
        } else {
            // Non-letter: use shifted table when shift is pressed
            if (shift_pressed) ch = scancode_shifted[scancode];
        }

        kbd_write(ch);
        sys_notify(shell_pid);
    }
}
