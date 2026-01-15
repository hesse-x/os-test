// 最简C语言内核，直接操作VGA文本缓冲区打印字符串
#include <stdint.h>
#include <stddef.h>

// VGA文本缓冲区的物理地址（0xB8000，GRUB2已映射，可直接访问）
#define VGA_BUFFER 0xB8000

// VGA文本模式颜色定义（前景色+背景色）
typedef enum {
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_BLUE = 1,
    VGA_COLOR_GREEN = 2,
    VGA_COLOR_RED = 4,
    VGA_COLOR_WHITE = 15
} vga_color;

// 组合前景色和背景色
static inline uint8_t vga_entry_color(vga_color fg, vga_color bg) {
    return fg | bg << 4;
}

// 生成VGA文本条目（字符+颜色）
static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t) uc | (uint16_t) color << 8;
}

// 全局变量：VGA缓冲区指针、当前光标位置、颜色属性
static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;

// 初始化VGA终端
void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    // 设置颜色：白色前景，黑色背景
    terminal_color = vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    terminal_buffer = (uint16_t*) VGA_BUFFER;
    // 清空终端（填充空格）
    for (size_t y = 0; y < 25; y++) {
        for (size_t x = 0; x < 80; x++) {
            const size_t index = y * 80 + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
}

// 在指定位置打印一个字符
void terminal_putchar_at(char c, uint8_t color, size_t x, size_t y) {
    const size_t index = y * 80 + x;
    terminal_buffer[index] = vga_entry(c, color);
}

// 打印一个字符（自动换行）
void terminal_putchar(char c) {
    if (c == '\n') { // 处理换行符
        terminal_row++;
        terminal_column = 0;
        return;
    }
    terminal_putchar_at(c, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == 80) { // 处理行满换行
        terminal_column = 0;
        if (++terminal_row == 25)
            terminal_row = 0;
    }
}

// 打印字符串
void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

// 简化的字符串打印函数（支持字符串常量）
void terminal_writestring(const char* data) {
    size_t i = 0;
    while (data[i] != '\0')
        i++;
    terminal_write(data, i);
}

// 内核主函数（入口）
void kernel_main(void) {
    // 初始化VGA终端
    terminal_initialize();
    // 打印自定义字符串（验证内核运行）
    terminal_writestring("Hello, My OS based on GRUB2!");
}
