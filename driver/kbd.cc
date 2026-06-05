#include "driver/kbd.h"
#include "arch/x86/utils.h"

static kbd_handler callback = nullptr;

// Ring buffer for sys_getc
#define KBD_BUF_SIZE 128
static char kbd_buf[KBD_BUF_SIZE];
static int kbd_buf_head = 0;
static int kbd_buf_tail = 0;

// US keyboard scancode set 1 -> char (no shift, lowercase only)
static const char scancode_table[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '-', 0, '5', 0, '+', 0, 0, 0, 0, 0, 0, 0,
};

void kbd_init() { callback = nullptr; kbd_buf_head = 0; kbd_buf_tail = 0; }

static void kbd_buffer_push(char c) {
    int next_tail = (kbd_buf_tail + 1) % KBD_BUF_SIZE;
    if (next_tail != kbd_buf_head) {  // not full
        kbd_buf[kbd_buf_tail] = c;
        kbd_buf_tail = next_tail;
    }
}

bool kbd_buffer_empty() { return kbd_buf_head == kbd_buf_tail; }

char kbd_buffer_pop() {
    if (kbd_buffer_empty()) return 0;
    char c = kbd_buf[kbd_buf_head];
    kbd_buf_head = (kbd_buf_head + 1) % KBD_BUF_SIZE;
    return c;
}

void kbd_handle() {
  uint8_t scancode = inb(0x60);
  if (scancode & 0x80)
    return; // ignore key release
  if (scancode >= 128)
    return;
  char c = scancode_table[scancode];
  if (c) {
    kbd_buffer_push(c);
    if (callback) callback(c);
  }
}

void kbd_register_handler(kbd_handler h) { callback = h; }
