// libinput_client: consumer-side input SHM ring poll + basic ASCII mapping.
#include "input.h"
#include <stdint.h>
#include <xos/input.h>

// Poll ring: drain all pending events. Pure drain, no sleeping flag management.
int input_client_poll(volatile void *shm, input_event_t *events,
                      int max_events) {
  if (!shm || !events || max_events <= 0)
    return 0;
  volatile input_shm_header_t *hdr = (volatile input_shm_header_t *)shm;
  if (hdr->magic != INPUT_SHM_MAGIC)
    return 0;

  uint32_t head = __atomic_load_n(&hdr->head, __ATOMIC_ACQUIRE);
  uint32_t tail = __atomic_load_n(&hdr->tail, __ATOMIC_ACQUIRE);
  uint32_t cap =
      hdr->ring_capacity ? hdr->ring_capacity : INPUT_RING_CAPACITY_DEFAULT;
  uint32_t off =
      hdr->ring_offset ? hdr->ring_offset : sizeof(input_shm_header_t);

  int n = 0;
  while (head != tail && n < max_events) {
    volatile input_event_t *slot =
        (volatile input_event_t *)((volatile uint8_t *)shm + off +
                                   tail * sizeof(input_event_t));
    events[n].timestamp_ns = slot->timestamp_ns;
    events[n].type = slot->type;
    events[n].code = slot->code;
    events[n].value = slot->value;
    n++;
    tail = (tail + 1) % cap;
  }
  __atomic_store_n(&hdr->tail, tail, __ATOMIC_RELEASE);
  return n;
}

// Basic single-byte ASCII mapping (no shift/caps/Ctrl — caller handles those).
int input_event_to_ascii(const input_event_t *ev, uint8_t *buf, int buf_len) {
  if (!ev || !buf || buf_len <= 0)
    return 0;
  if (ev->type != EV_KEY || ev->value != 1)
    return 0; // press only

  uint8_t ch = 0;
  switch (ev->code) {
  case KEY_A:
    ch = 'a';
    break;
  case KEY_B:
    ch = 'b';
    break;
  case KEY_C:
    ch = 'c';
    break;
  case KEY_D:
    ch = 'd';
    break;
  case KEY_E:
    ch = 'e';
    break;
  case KEY_F:
    ch = 'f';
    break;
  case KEY_G:
    ch = 'g';
    break;
  case KEY_H:
    ch = 'h';
    break;
  case KEY_I:
    ch = 'i';
    break;
  case KEY_J:
    ch = 'j';
    break;
  case KEY_K:
    ch = 'k';
    break;
  case KEY_L:
    ch = 'l';
    break;
  case KEY_M:
    ch = 'm';
    break;
  case KEY_N:
    ch = 'n';
    break;
  case KEY_O:
    ch = 'o';
    break;
  case KEY_P:
    ch = 'p';
    break;
  case KEY_Q:
    ch = 'q';
    break;
  case KEY_R:
    ch = 'r';
    break;
  case KEY_S:
    ch = 's';
    break;
  case KEY_T:
    ch = 't';
    break;
  case KEY_U:
    ch = 'u';
    break;
  case KEY_V:
    ch = 'v';
    break;
  case KEY_W:
    ch = 'w';
    break;
  case KEY_X:
    ch = 'x';
    break;
  case KEY_Y:
    ch = 'y';
    break;
  case KEY_Z:
    ch = 'z';
    break;
  case KEY_1:
    ch = '1';
    break;
  case KEY_2:
    ch = '2';
    break;
  case KEY_3:
    ch = '3';
    break;
  case KEY_4:
    ch = '4';
    break;
  case KEY_5:
    ch = '5';
    break;
  case KEY_6:
    ch = '6';
    break;
  case KEY_7:
    ch = '7';
    break;
  case KEY_8:
    ch = '8';
    break;
  case KEY_9:
    ch = '9';
    break;
  case KEY_0:
    ch = '0';
    break;
  case KEY_MINUS:
    ch = '-';
    break;
  case KEY_EQUAL:
    ch = '=';
    break;
  case KEY_LEFTBRACE:
    ch = '[';
    break;
  case KEY_RIGHTBRACE:
    ch = ']';
    break;
  case KEY_SEMICOLON:
    ch = ';';
    break;
  case KEY_APOSTROPHE:
    ch = '\'';
    break;
  case KEY_GRAVE:
    ch = '`';
    break;
  case KEY_BACKSLASH:
    ch = '\\';
    break;
  case KEY_COMMA:
    ch = ',';
    break;
  case KEY_DOT:
    ch = '.';
    break;
  case KEY_SLASH:
    ch = '/';
    break;
  case KEY_SPACE:
    ch = ' ';
    break;
  case KEY_ENTER:
    ch = '\n';
    break;
  case KEY_BACKSPACE:
    ch = 8;
    break;
  case KEY_TAB:
    ch = '\t';
    break;
  case KEY_ESC:
    ch = 27;
    break;
  default:
    return 0;
  }
  buf[0] = ch;
  return 1;
}
