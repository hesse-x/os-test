#include "os-test/drivers/screen.h"
#include "os-test/utils/x86.h"
#include <stdint.h>

#define VIDEO_ADDRESS 0xb8000
#define MAX_ROWS 25
#define MAX_COLS 80

/* Screen i/o ports */
#define REG_SCREEN_CTRL 0x3d4
#define REG_SCREEN_DATA 0x3d5

typedef struct {
  int row;
  int col;
  int offset;
} Cursor;

static Cursor cursor = {0, 0, 0};
/* Declaration of private functions */
static int get_offset(int col, int row) { return 2 * (row * MAX_COLS + col); }
static int get_offset_row(int offset) { return offset / (2 * MAX_COLS); }
static int get_offset_col(int offset) {
  return (offset - (get_offset_row(offset) * 2 * MAX_COLS)) / 2;
}

static int get_cursor_offset();
static void set_cursor_offset(int offset);

/**********************************************************
 * Public Kernel API functions                            *
 **********************************************************/
void init_screen() {
  cursor.offset = get_cursor_offset();
  cursor.row = get_offset_row(cursor.offset);
  cursor.row = get_offset_row(cursor.offset);
}

/**
 * Innermost print function for our kernel, directly accesses the video memory
 *
 * If 'col' and 'row' are negative, we will print at current cursor location
 * If 'attr' is zero it will use 'white on black' as default
 * Returns the offset of the next character
 * Sets the video cursor to the returned offset
 */
void put_char(char c, char attr) {
  uint8_t *vidmem = (uint8_t *)VIDEO_ADDRESS;
  if (!attr)
    attr = WHITE_ON_BLACK;

  switch (c) {
  case '\n': {
    cursor.col = 0;
    cursor.row += 1;
    cursor.offset = get_offset(cursor.col, cursor.row);
    break;
  }
  case '\r': {
    cursor.col = 0;
    cursor.offset = get_offset(cursor.col, cursor.row);
    break;
  }
  case '\b': {
    if (cursor.offset == 0)
      break;
    cursor.col -= 1;
    cursor.offset -= 2;
    vidmem[cursor.offset] = ' ';
    vidmem[cursor.offset + 1] = attr;
    break;
  }
  default: {
    vidmem[cursor.offset] = c;
    vidmem[cursor.offset + 1] = attr;
    cursor.col += 1;
    cursor.offset += 2;
  }
  }

  /* Check if the offset is over screen size and scroll */
  if (cursor.offset >= MAX_ROWS * MAX_COLS * 2) {
    int i;
    for (i = 1; i < MAX_ROWS; i++)
      __memmove((uint8_t *)(get_offset(0, i - 1) + VIDEO_ADDRESS),
                (uint8_t *)(get_offset(0, i) + VIDEO_ADDRESS), MAX_COLS * 2);

    /* Blank last line */
    char *last_line =
        (char *)(get_offset(0, MAX_ROWS - 1) + (uint8_t *)VIDEO_ADDRESS);
    for (i = 0; i < MAX_COLS * 2; i++)
      last_line[i] = 0;

    cursor.offset -= 2 * MAX_COLS;
  }

  set_cursor_offset(cursor.offset);
}

void clear_screen() {
  int screen_size = MAX_COLS * MAX_ROWS;
  int i;
  uint8_t *screen = (uint8_t *)VIDEO_ADDRESS;

  for (i = 0; i < screen_size; i++) {
    screen[i * 2] = ' ';
    screen[i * 2 + 1] = WHITE_ON_BLACK;
  }
  set_cursor_offset(get_offset(0, 0));
}

void kprint(const char *message) {
  /* Loop through message and print it */
  int i = 0;
  while (message[i] != 0) {
    put_char(message[i++], WHITE_ON_BLACK);
  }
}

/**********************************************************
 * Private kernel functions                               *
 **********************************************************/

int get_cursor_offset() {
  /* Use the VGA ports to get the current cursor position
   * 1. Ask for high byte of the cursor offset (data 14)
   * 2. Ask for low byte (data 15)
   */
  outb(REG_SCREEN_CTRL, 14);
  int offset = inb(REG_SCREEN_DATA) << 8; /* High byte: << 8 */
  outb(REG_SCREEN_CTRL, 15);
  offset += inb(REG_SCREEN_DATA);
  return offset * 2; /* Position * size of character cell */
}

void set_cursor_offset(int offset) {
  /* Similar to get_cursor_offset, but instead of reading we write data */
  cursor.offset = offset;
  cursor.row = get_offset_row(offset);
  cursor.col = get_offset_col(offset);
  offset /= 2;
  outb(REG_SCREEN_CTRL, 14);
  outb(REG_SCREEN_DATA, (uint8_t)(offset >> 8));
  outb(REG_SCREEN_CTRL, 15);
  outb(REG_SCREEN_DATA, (uint8_t)(offset & 0xff));
}
