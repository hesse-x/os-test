// Keyboard driver process (user-space)
// Binds to IRQ 1, reads scan codes from port 0x60, writes to KBD_SHM
#include <stdint.h>
#include "arch/x64/utils.h"
#include "common/shm.h"

static volatile kbd_shm *kbd = (volatile kbd_shm *)KBD_SHM_ADDR;

#define KBD_BUF_SIZE 4088

// Simple scancode to ASCII (set 1, make code only)
static const uint8_t scancode_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', 8,
    9, 'q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,
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

    // Our PID is assigned by kernel. Shell is always the last process created,
    // so shell_pid = our_pid + 1 (disk_driver=kbd_driver-1, shell=kbd_driver+1).
    // Actually: PID2=disk_driver, PID3=kbd_driver, PID4=shell
    int32_t my_pid = (int32_t)sys_getpid();
    int32_t shell_pid = my_pid + 1;

    while (1) {
        // Wait for keyboard interrupt
        sys_wait();

        // Read from keyboard controller
        uint8_t scancode = inb(0x60);

        // Only handle make codes (bit 7 clear)
        if (scancode & 0x80) continue;

        uint8_t ch = scancode_to_ascii[scancode];
        if (ch) {
            kbd_write(ch);
            sys_notify(shell_pid);
        }
    }
}
