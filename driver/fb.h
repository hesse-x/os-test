#ifndef FB_H
#define FB_H

#include <stdint.h>

extern "C" {

// 初始化
void init_fb(uint32_t mbi_addr);

// 渲染
void clear();
void fb_putc(char c, uint32_t fg);
void prints(const char *s, uint32_t fg);

// 光标查询
uint32_t cursor_get_x();
uint32_t cursor_get_y();
uint32_t cursor_get_width();
uint32_t cursor_get_height();

// 光标移动
void cursor_move(uint32_t x, uint32_t y);

}

#endif // FB_H
