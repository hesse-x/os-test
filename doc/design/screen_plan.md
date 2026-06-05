# Framebuffer 模块分离方案

> **已实现**。当前实现在 `driver/fb.cc`/`fb.h`，init_fb 接收 `boot_info*`（非 multiboot2 mbi_addr），
> 从 UEFI GOP 获取 framebuffer 信息。设备映射区基址 `device_vma_base` 在 `arch/x64/paging.cc` 定义。
> init_fb 在 init_mem 末尾调用，依赖 bump_alloc 和 device_vma_base 就绪。

## 概述

将 `init_mem` 中显存相关逻辑分离为独立模块 `fb.h`/`fb.cc`，提供 struct 封装、光标管理和基本渲染接口。

## 文件结构

- `fb.h`：对外函数声明（不暴露 struct 定义）
- `fb.cc`：`Framebuffer` struct、`Cursor` struct、static 全局实例、字体数据、所有实现

## Struct 定义（fb.cc 内部）

```c
struct Framebuffer {
    uint32_t width;      // 像素宽度
    uint32_t height;     // 像素高度
    uint32_t pitch;      // 每行字节数
    uint32_t bpp;        // 每像素位数
    void *vaddr;         // 映射后的虚拟地址
    size_t size;         // 显存总大小 (pitch * height)
    uint64_t phys_addr;  // 物理地址
};

struct Cursor {
    uint32_t x;          // 光标列（字符单位）
    uint32_t y;          // 光标行（字符单位）
};
```

## 全局变量

| 变量 | 位置 | 说明 |
|---|---|---|
| `static Framebuffer fb` | fb.cc | 唯一 framebuffer 实例，外部不可访问 |
| `static Cursor cursor` | fb.cc | 光标实例，外部不可访问 |
| `uintptr_t device_vma_base` | mem.h（extern）/ mem.cc（定义） | 设备映射区起始虚拟地址，init_mem 计算赋值 |

## 对外接口（fb.h）

```c
// 初始化
void init_fb(uint32_t mbi_addr);

// 渲染
void clear();                                        // 清屏填黑 0x000000
void putc(char c, uint32_t fg = 0xFFFFFF);           // 输出单字符，默认白色
void prints(const char *s, uint32_t fg = 0xFFFFFF);  // 输出字符串，默认白色

// 光标查询
uint32_t cursor_get_x();
uint32_t cursor_get_y();
uint32_t cursor_get_width();     // 读 fb.width
uint32_t cursor_get_height();    // 读 fb.height

// 光标移动
void cursor_move(uint32_t x, uint32_t y);  // 无边界检查，调用方负责
```

## init_fb 设计

- 签名：`void init_fb(uint32_t mbi_addr)`
- 内部使用 static helper `find_fb_tag` 从 multiboot2 tag list 查找 framebuffer tag
- 读取 `device_vma_base`（mem.h 全局）确定设备映射区基址
- 依赖 `bump_alloc`（全局）分配页表，`page_directory`（全局）安装映射
- 末尾自行 reload CR3 刷新 TLB，返回后 framebuffer 立即可用
- 初始化 `fb` struct 所有字段，`cursor` 归零

## putc 行为

| 字符 | 行为 |
|---|---|
| 可打印字符 | 绘制到当前光标位置，光标右移一字符 |
| `\n` | 光标移到下一行行首 |
| `\r` | 光标移到当前行行首 |
| `\t` | 光标对齐到下一个 8 字符列倍数；超出宽度则换行 |
| 写到行尾 | 自动换行 |
| 写到屏幕底部 | 停住，暂不滚动 |

## 字体

- 内嵌 8x16 PC BIOS 位图字体
- 覆盖 ASCII 可打印字符 0x20-0x7E
- 每字符 16 字节，共 ~2KB

## 前景色

- `putc`/`prints` 通过 `fg` 参数指定，默认 `0xFFFFFF`（白色）
- 背景色由 `clear` 决定（黑色），暂不支持逐字符背景色

## init_mem 修改

移除以下逻辑（迁移到 init_fb）：
- `find_fb_tag` 调用
- `device_vma_base` 计算（改为赋值到全局变量）
- framebuffer 页表映射（第 278-307 行）
- `fb_mapped_vaddr` / `fb_size` 赋值

移除以下导出（由 fb 模块替代）：
- `mem.h` 中的 `extern void *fb_mapped_vaddr`
- `mem.h` 中的 `extern size_t fb_size`
- `mem.cc` 中的定义

新增：
- `mem.h` 中声明 `extern uintptr_t device_vma_base`
- `mem.cc` 中定义 `uintptr_t device_vma_base`
- `init_mem` 末尾调用 `init_fb(mbi_addr)`

## kernel.cc 修改

- 移除 `fill_white` 函数
- 移除对 `fb_mapped_vaddr` / `fb_size` 的引用
- 改为调用 `fb.h` 接口（如 `clear()`、`prints` 等）

## 依赖关系

```
init_mem 执行顺序：
  1. mmap 解析 → bump allocator 初始化 → 页帧管理
  2. 计算 device_vma_base 并赋值全局变量
  3. 调用 init_fb(mbi_addr)  ← 必须在 bump_alloc 和 device_vma_base 就绪之后
```
