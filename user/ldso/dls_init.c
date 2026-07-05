/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// ld.so 主流程（post-bootstrap）
// ld.md §3.3 / §8.2.2 递归加载多依赖 / plan_ld2b3 T14

#include <stddef.h>
#include <stdint.h>
#include <sys/link_map.h>
#include <xos/elf.h>
#include <xos/fcntl.h>
#include <xos/syscall_nums.h>

// bootstrap 阶段辅助（hidden：跨文件可见但不走 PLT）
__attribute__((visibility("hidden"))) void dl_puts(const char *s);
__attribute__((visibility("hidden"))) void dl_put_hex(uint64_t val);

// 外部函数声明（load_so.c / relocate.c / symtab.c / link_map.c）
__attribute__((visibility("hidden"))) void *load_so(const char *path,
                                                    Elf64_Dyn **out_dyn);
__attribute__((visibility("hidden"))) void
apply_relocation(Elf64_Rela *r, void *base, struct link_map *lmap);
__attribute__((visibility("hidden"))) void *
lookup_symbol_in_link_map(const char *name, struct link_map *lmap);
__attribute__((visibility("hidden"))) void eager_bind(struct link_map *l);

// minilibc.c
__attribute__((visibility("hidden"))) void *malloc(unsigned long size);
__attribute__((visibility("hidden"))) int strcmp(const char *a, const char *b);

// 全局 link_map 链表（link_map.c 定义）
extern struct link_map *_dl_link_map;

// 链接器提供的 .dynamic 起始符号（ld.so 自身）
extern Elf64_Dyn _DYNAMIC[];

static uintptr_t find_auxv_val(uintptr_t *sp, uint64_t type) {
  int argc = (int)sp[0];
  uintptr_t *argv = sp + 1;
  uintptr_t *envp = argv + argc + 1;
  uintptr_t *p = envp;
  while (*p)
    p++;
  p++;
  while (*p != AT_NULL) {
    if (p[0] == type)
      return p[1];
    p += 2;
  }
  return 0;
}

// 重定位单个对象：遍历 .rela.dyn 应用所有重定位 + eager bind .rela.plt
static void relocate_object(struct link_map *l) {
  Elf64_Rela *rela = (Elf64_Rela *)l->rela_dyn;
  size_t rela_sz = l->rela_dyn_sz;
  if (rela && rela_sz) {
    for (size_t i = 0; i < rela_sz / sizeof(Elf64_Rela); i++) {
      apply_relocation(&rela[i], (void *)l->base, l);
    }
  }
  eager_bind(l);
}

// ===== §8.2.1 加载队列与去重 =====
// 已加载库表：soname → link_map 节点（去重 + 闭环检测）
struct loaded_lib {
  char soname[64];
  struct link_map *map;
};
static struct loaded_lib g_loaded[16];
static int g_loaded_cnt = 0;

// soname → link_map，未加载返回 NULL
static struct link_map *find_loaded(const char *soname) {
  for (int i = 0; i < g_loaded_cnt; i++) {
    if (strcmp(g_loaded[i].soname, soname) == 0)
      return g_loaded[i].map;
  }
  return NULL;
}

// 登记已加载库
static void register_loaded(const char *soname, struct link_map *m) {
  if (g_loaded_cnt >= 16) {
    dl_puts("dl: FATAL: too many loaded libs");
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(SYS_EXIT), "D"(1)
                     : "rcx", "r11");
    while (1)
      ;
  }
  // 拷贝 soname（截断到 63 字节）
  int i = 0;
  while (soname[i] && i < 63) {
    g_loaded[g_loaded_cnt].soname[i] = soname[i];
    i++;
  }
  g_loaded[g_loaded_cnt].soname[i] = '\0';
  g_loaded[g_loaded_cnt].map = m;
  g_loaded_cnt++;
}

// 加载单个 .so（不去重），建 link_map 节点但不重定位
static struct link_map *load_one(const char *soname) {
  // 路径搜索：先 /lib/<soname>（生产库），失败回退 /test/lib/<soname>
  // （ld.so 测试 stub）。对齐 Linux DT_RPATH 思路；当前不解析
  // DT_RPATH/DT_RUNPATH （ld.md §8.4 已知边界），硬编码前缀已满足所有库放 /lib/
  // 或 /test/lib/。
  static char lib_path[256];
  const char *prefix = "/lib/";
  char *dp = lib_path;
  while (*prefix)
    *dp++ = *prefix++;
  const char *np = soname;
  while (*np)
    *dp++ = *np++;
  *dp = '\0';

  // 探测 /lib/<soname> 是否存在（load_so open 失败会 SYS_EXIT，不能靠返回值）
  long fd;
  __asm__ volatile("syscall"
                   : "=a"(fd)
                   : "a"(SYS_OPEN), "D"((int64_t)(uintptr_t)lib_path),
                     "S"((int64_t)O_RDONLY)
                   : "rcx", "r11");
  if (fd < 0) {
    // 回退 /test/lib/<soname>
    const char *tp = "/test/lib/";
    dp = lib_path;
    while (*tp)
      *dp++ = *tp++;
    np = soname;
    while (*np)
      *dp++ = *np++;
    *dp = '\0';
  } else {
    long rc;
    __asm__ volatile("syscall"
                     : "=a"(rc)
                     : "a"(SYS_CLOSE), "D"((int64_t)fd)
                     : "rcx", "r11");
  }

  dl_puts("dl: loading ");
  dl_puts(lib_path);
  Elf64_Dyn *dyn = NULL;
  void *base = load_so(lib_path, &dyn);
  dl_puts("dl: loaded ");
  dl_puts(soname);
  dl_puts(" @ ");
  dl_put_hex((uint64_t)base);

  struct link_map *m = (struct link_map *)malloc(sizeof(*m));
  fill_link_map(m, (uintptr_t)base, dyn);
  // 填 soname
  int i = 0;
  while (soname[i] && i < 63) {
    m->soname[i] = soname[i];
    i++;
  }
  m->soname[i] = '\0';
  m->l_next = NULL;
  m->l_prev = NULL;
  return m;
}

// 递归加载 soname 及其全部 NEEDED（BFS），已加载则去重
// 链入链表尾部 tail，返回新尾
static struct link_map *load_recursive(const char *soname,
                                       struct link_map *tail) {
  if (find_loaded(soname))
    return tail; // 去重（兼闭环收敛）
  struct link_map *m = load_one(soname);
  register_loaded(soname, m);
  tail->l_next = m;
  m->l_prev = tail;
  tail = m;

  // 递归：读 m 自身的 .dynamic，加载它的全部 DT_NEEDED
  // dynstr 直接用 fill_link_map 已填好的 m->strtab（运行时绝对地址）
  const char *dynstr = m->strtab;
  for (Elf64_Dyn *d = (Elf64_Dyn *)m->dynamic; d->d_tag != DT_NULL; d++) {
    if (d->d_tag == DT_NEEDED) {
      const char *dep = dynstr + d->d_un.d_val;
      tail = load_recursive(dep, tail);
    }
  }
  return tail;
}

void __dls_init(uintptr_t *sp, uintptr_t ld_base) {
  // 1. 从 auxv 取主 ELF 信息
  uintptr_t phdr = find_auxv_val(sp, AT_PHDR);
  size_t phent = find_auxv_val(sp, AT_PHENT);
  size_t phnum = find_auxv_val(sp, AT_PHNUM);
  uintptr_t entry = find_auxv_val(sp, AT_ENTRY);

  // 2. 遍历 PHDR 找 PT_DYNAMIC
  Elf64_Dyn *main_dyn = NULL;
  for (size_t i = 0; i < phnum; i++) {
    Elf64_Phdr *p = (Elf64_Phdr *)(phdr + i * phent);
    if (p->p_type == PT_DYNAMIC) {
      main_dyn = (Elf64_Dyn *)p->p_vaddr;
      break;
    }
  }
  if (!main_dyn) {
    dl_puts("dl: FATAL: main ELF has no PT_DYNAMIC");
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(SYS_EXIT), "D"(1)
                     : "rcx", "r11");
    while (1)
      ;
  }

  // 3. 找 DT_STRTAB
  const char *main_dynstr = NULL;
  for (Elf64_Dyn *d = main_dyn; d->d_tag != DT_NULL; d++) {
    if (d->d_tag == DT_STRTAB) {
      main_dynstr = (const char *)d->d_un.d_ptr;
      break;
    }
  }

  // 4. ld.so 自身节点：静态（无外部依赖，零成本）
  struct link_map *ld_map = &g_ld_map_static;
  fill_link_map(ld_map, ld_base, _DYNAMIC);
  ld_map->soname[0] = '\0';

  // 5. 递归加载主 ELF 的全部 DT_NEEDED（BFS，去重）
  //    链表按加载完成顺序尾插，天然满足「依赖先于依赖者」
  //    head 固定为链首（ld_map），tail 随尾插前进；load_recursive 返回新尾
  struct link_map *head = ld_map;
  struct link_map *tail = ld_map;
  for (Elf64_Dyn *d = main_dyn; d->d_tag != DT_NULL; d++) {
    if (d->d_tag == DT_NEEDED) {
      const char *soname = main_dynstr + d->d_un.d_val;
      tail = load_recursive(soname, tail);
    }
  }

  // 6. 主 ELF 节点（非 PIE，base=0），链到链表头
  struct link_map *main_map = (struct link_map *)malloc(sizeof(*main_map));
  fill_link_map(main_map, 0, main_dyn);
  main_map->soname[0] = '\0';
  main_map->l_next = head;
  main_map->l_prev = NULL;
  if (head)
    head->l_prev = main_map;
  _dl_link_map = main_map;

  // 7. 填主 ELF 的 PT_TLS 信息（libc.so/ld.so 通常无 TLS，留 0）
  fill_tls_from_phdr(main_map, 0, phdr, phent, phnum);

  // 8. 重定位：按「依赖先于依赖者」顺序遍历链表（跳过 ld.so 自身和主 ELF）
  //    ld.so 已在 bootstrap 自重定位；主 ELF 最后重定位
  //    链表顺序 main → ld → libs...：ld 夹在中间，需显式跳过 ld_map
  for (struct link_map *l = main_map->l_next; l; l = l->l_next) {
    if (l == ld_map)
      continue;
    dl_puts("dl: relocating ");
    dl_puts(l->soname);
    relocate_object(l);
  }
  dl_puts("dl: relocating main ELF");
  relocate_object(main_map);
  dl_puts("dl: main ELF relocated");

  // 9. 跳主 ELF entry
  dl_puts("dl: jump to entry");
  dl_put_hex(entry);

  __asm__ volatile("mov %0, %%rsp\n"
                   "jmp *%1\n"
                   :
                   : "r"(sp), "r"(entry));
  while (1)
    ;
}
