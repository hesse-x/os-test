#ifndef DRIVERS_SCREEN_H_
#define DRIVERS_SCREEN_H_

#define WHITE_ON_BLACK 0x0f
#define RED_ON_WHITE 0xf4

/* Public kernel API */
void clear_screen();
void kprint(const char *message);
void put_char(const char c, const char attr);

#endif // DRIVERS_SCREEN_H_
