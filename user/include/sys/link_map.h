#ifndef USER_SYS_LINK_MAP_H
#define USER_SYS_LINK_MAP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <xos/elf.h>

#ifdef __cplusplus
extern "C" {
#endif

// link_map：ld.so 构造的已加载对象描述符
// libc.so 通过 _dl_link_map 读取此结构（collect_tls_from_link_map 用）
// ld.so 的 link_map.c 定义同一结构，两边必须一致
// plan_ld2b3 T12 / ld.md §3.3.7

struct link_map {
  uintptr_t base; // 加载基址
  char soname[64]; // DT_NEEDED soname（如 "libc.so"）；主 ELF/ld.so 可为空
  void *dynamic;           // .dynamic 段指针
  struct link_map *l_next; // 链表下一项
  struct link_map *l_prev; // 链表上一项
  void *symtab;            // .symtab
  const char *strtab;      // .strtab
  void *gnu_hash;          // .gnu.hash
  void *rela_dyn;          // .rela.dyn
  size_t rela_dyn_sz;      // .rela.dyn 大小
  void *rela_plt;          // .rela.plt
  size_t rela_plt_sz;      // .rela.plt 大小
  void *tls_template;      // TLS 模板（.tdata 起始）
  size_t tls_tdata_size;   // .tdata 大小
  size_t tls_tbss_size;    // .tbss 大小
  size_t tls_align;        // TLS 对齐
};

// ld.so 导出的全局 link_map 链表头
// visibility("default") 匹配 ld.so 的定义（ld.so 全局 -fvisibility=hidden，
// 仅此符号导出）。libc.so 引用时走 GLOB_DAT 重定位（ld.so bootstrap 已处理）
__attribute__((visibility("default"))) extern struct link_map *_dl_link_map;

// libc.so 用：遍历 _dl_link_map 合并 PT_TLS 填 tls_info
struct tls_info;
LIBC_EXPORT extern struct tls_info
collect_tls_from_link_map(struct link_map *lmap);

// ld.so 内部：从 PHDR 找 PT_TLS 填 link_map 的 tls_* 字段
// hidden visibility：避免 ld.so 跨模块调用走 PLT（bootstrap 前 GOT 未填，
// PLT 跳转落到 lazy stub 0x1016 → #PF）
__attribute__((visibility("hidden"))) void
fill_tls_from_phdr(struct link_map *l, uintptr_t base, uintptr_t phdr,
                   size_t phent, size_t phnum);

// ld.so 内部：解析 .dynamic 填 link_map 符号查找字段（dls_init.c
// 动态构造链表时调用）
__attribute__((visibility("hidden"))) void
fill_link_map(struct link_map *l, uintptr_t base, Elf64_Dyn *dyn);

// ld.so 自身静态节点（link_map.c 定义，dls_init.c 取地址链入链表尾）
__attribute__((visibility("hidden"))) extern struct link_map g_ld_map_static;

#ifdef __cplusplus
}
#endif

#endif // USER_SYS_LINK_MAP_H
