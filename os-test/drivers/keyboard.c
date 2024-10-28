#include "os-test/drivers/keyboard.h"
#include "os-test/cpu/isr.h"
#include "os-test/drivers/screen.h"
#include "os-test/utils/os_utils.h"
#include "os-test/utils/x86.h"
#include <stdbool.h>
#include <stdint.h>

#define BACKSPACE 0x0E
#define ENTER 0x1C

static char key_buffer[256] = {'\0'};

static uint8_t state = 0;

static bool shift_state() { return state >> 7; }

static void set_shift() { state |= (1 << 7); }

static void release_shift() { state &= (0 << 7); }

static bool is_shift(char sc) { return (sc == 0x2A || sc == 0x36); }

#define SC_MAX 57
static const char *sc_name[] = {
    "ERROR",     "Esc",     "1", "2", "3", "4",      "5",
    "6",         "7",       "8", "9", "0", "-",      "=",
    "Backspace", "Tab",     "Q", "W", "E", "R",      "T",
    "Y",         "U",       "I", "O", "P", "[",      "]",
    "Enter",     "Lctrl",   "A", "S", "D", "F",      "G",
    "H",         "J",       "K", "L", ";", "'",      "`",
    "LShift",    "\\",      "Z", "X", "C", "V",      "B",
    "N",         "M",       ",", ".", "/", "RShift", "Keypad *",
    "LAlt",      "Spacebar"};

static const char sc_ascii[] = {
    '?', '?', '1', '2', '3', '4', '5', '6', '7', '8', '9',  '0', '-', '=',  '?',
    '?', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',  '[', ']', '?',  '?',
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ';', '\'', '`', '?', '\\', 'Z',
    'X', 'C', 'V', 'B', 'N', 'M', ',', '.', '/', '?', '?',  '?', ' '};

char get_letter(char sc) {
  char ascii = sc_ascii[(int)sc];
  if (ascii >= 'A' && ascii <= 'Z') {
    return ascii;
  }
  return -1;
}

static void user_input(char *input) {
  if (__strcmp(input, "END") == 0) {
    kprint("Stopping the CPU. Bye!\n");
    asm volatile("hlt");
  }
  kprint("You said: ");
  kprint(input);
  kprint("\n> ");
}

static void keyboard_callback(registers_t *regs) {
  /* The PIC leaves us the scancode in port 0x60 */
  uint8_t scancode = inb(0x60);
  if (is_shift(scancode)) {
    set_shift();
    return;
  }
  if (scancode >> 7) {
    if (is_shift(scancode & 0x7f)) {
      release_shift();
    }
    return;
  }
  if (scancode > SC_MAX)
    return;
  if (scancode == BACKSPACE) {
    size_t end = __strlen(key_buffer);
    key_buffer[end - 1] = '\0';
    put_char('\b', WHITE_ON_BLACK);
  } else if (scancode == ENTER) {
    kprint("\n");
    user_input(key_buffer); /* kernel-controlled function */
    key_buffer[0] = '\0';
  } else {
    char letter = get_letter(scancode);
    if (letter == -1) {
      letter = sc_ascii[(int)scancode];
    } else {
      if (!shift_state()) {
        letter += 32;
      }
    }
    /* Remember that kprint only accepts char[] */
    char str[2] = {letter, '\0'};
    size_t end = __strlen(key_buffer);
    key_buffer[end] = letter;
    key_buffer[end + 1] = '\0';
    kprint(str);
  }
  UNUSED(regs);
}

void init_keyboard() {
  key_buffer[0] = '\0';
  register_interrupt_handler(IRQ1, keyboard_callback);
}
