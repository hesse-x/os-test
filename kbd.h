#ifndef KBD_H
#define KBD_H

extern "C" {
typedef void (*kbd_handler)(char);

void kbd_init();
void kbd_handle();
void kbd_register_handler(kbd_handler h);
}
#endif // KBD_H
