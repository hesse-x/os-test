#include "driver/kbd.h"
#include "arch/x86/utils.h"

static kbd_handler callback = nullptr;

// US keyboard scancode set 1 → char (no shift, lowercase only)
static const char scancode_table[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '-', 0, '5', 0, '+', 0, 0, 0, 0, 0, 0, 0,
};

void kbd_init() { callback = nullptr; }

void kbd_handle() {
  uint8_t scancode = inb(0x60);
  if (scancode & 0x80)
    return; // ignore key release
  if (scancode >= 128)
    return;
  char c = scancode_table[scancode];
  if (c && callback)
    callback(c);
}

void kbd_register_handler(kbd_handler h) { callback = h; }
