/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// ld.so main flow (post-bootstrap)
// ld.md §3.3 / §8.2.2 recursive loading of multiple dependencies / plan_ld2b3 T14

#include <stddef.h>
#include <stdint.h>
#include <sys/link_map.h>
#include <xos/elf.h>
#include <xos/fcntl.h>
#include <xos/syscall_nums.h>

// bootstrap stage helpers (hidden: visible across files but not via PLT)
__attribute__((visibility("hidden"))) void dl_puts(const char *s);
__attribute__((visibility("hidden"))) void dl_put_hex(uint64_t val);

// external function declarations (load_so.c / relocate.c / symtab.c / link_map.c)
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

// global link_map list (defined in link_map.c)
extern struct link_map *_dl_link_map;

// linker-provided .dynamic start symbol (ld.so itself)
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

// relocate a single object: walk .rela.dyn applying all relocations + eager bind .rela.plt
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

// ===== §8.2.1 load queue and dedup =====
// loaded library table: soname -> link_map node (dedup + cycle detection)
struct loaded_lib {
  char soname[64];
  struct link_map *map;
};
static struct loaded_lib g_loaded[16];
static int g_loaded_cnt = 0;

// soname -> link_map, returns NULL if not loaded
static struct link_map *find_loaded(const char *soname) {
  for (int i = 0; i < g_loaded_cnt; i++) {
    if (strcmp(g_loaded[i].soname, soname) == 0)
      return g_loaded[i].map;
  }
  return NULL;
}

// register loaded library
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
  // copy soname (truncate to 63 bytes)
  int i = 0;
  while (soname[i] && i < 63) {
    g_loaded[g_loaded_cnt].soname[i] = soname[i];
    i++;
  }
  g_loaded[g_loaded_cnt].soname[i] = '\0';
  g_loaded[g_loaded_cnt].map = m;
  g_loaded_cnt++;
}

// load a single .so (no dedup), build link_map node but do not relocate
static struct link_map *load_one(const char *soname) {
  // path search: try /lib/<soname> first (production libs), fall back to /test/lib/<soname>
  // (ld.so test stub). Aligns with Linux DT_RPATH idea; currently does not parse
  // DT_RPATH/DT_RUNPATH (ld.md §8.4 known boundary), hardcoded prefixes suffice since all
  // libs live in /lib/ or /test/lib/.
  static char lib_path[256];
  const char *prefix = "/lib/";
  char *dp = lib_path;
  while (*prefix)
    *dp++ = *prefix++;
  const char *np = soname;
  while (*np)
    *dp++ = *np++;
  *dp = '\0';

  // probe whether /lib/<soname> exists (load_so open failure triggers SYS_EXIT, cannot rely on return value)
  long fd;
  __asm__ volatile("syscall"
                   : "=a"(fd)
                   : "a"(SYS_OPEN), "D"((int64_t)(uintptr_t)lib_path),
                     "S"((int64_t)O_RDONLY)
                   : "rcx", "r11");
  if (fd < 0) {
    // fall back to /test/lib/<soname>
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
  // fill soname
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

// recursively load soname and all its NEEDED (BFS), dedup if already loaded
// append to tail of list, return new tail
static struct link_map *load_recursive(const char *soname,
                                       struct link_map *tail) {
  if (find_loaded(soname))
    return tail; // dedup (also closes cycles)
  struct link_map *m = load_one(soname);
  register_loaded(soname, m);
  tail->l_next = m;
  m->l_prev = tail;
  tail = m;

  // recurse: read m's own .dynamic, load all its DT_NEEDED
  // dynstr uses m->strtab filled by fill_link_map (runtime absolute address)
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
  // 1. get main ELF info from auxv
  uintptr_t phdr = find_auxv_val(sp, AT_PHDR);
  size_t phent = find_auxv_val(sp, AT_PHENT);
  size_t phnum = find_auxv_val(sp, AT_PHNUM);
  uintptr_t entry = find_auxv_val(sp, AT_ENTRY);

  // 2. walk PHDR to find PT_DYNAMIC
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

  // 3. find DT_STRTAB
  const char *main_dynstr = NULL;
  for (Elf64_Dyn *d = main_dyn; d->d_tag != DT_NULL; d++) {
    if (d->d_tag == DT_STRTAB) {
      main_dynstr = (const char *)d->d_un.d_ptr;
      break;
    }
  }

  // 4. ld.so's own node: static (no external dependencies, zero cost)
  struct link_map *ld_map = &g_ld_map_static;
  fill_link_map(ld_map, ld_base, _DYNAMIC);
  ld_map->soname[0] = '\0';

  // 5. recursively load all DT_NEEDED of the main ELF (BFS, dedup)
  //    list is appended at tail in load-completion order, naturally satisfying
  //    "dependencies before dependents"
  //    head is fixed as list head (ld_map), tail advances with each append;
  //    load_recursive returns the new tail
  struct link_map *head = ld_map;
  struct link_map *tail = ld_map;
  for (Elf64_Dyn *d = main_dyn; d->d_tag != DT_NULL; d++) {
    if (d->d_tag == DT_NEEDED) {
      const char *soname = main_dynstr + d->d_un.d_val;
      tail = load_recursive(soname, tail);
    }
  }

  // 6. main ELF node (non-PIE, base=0), link as list head
  struct link_map *main_map = (struct link_map *)malloc(sizeof(*main_map));
  fill_link_map(main_map, 0, main_dyn);
  main_map->soname[0] = '\0';
  main_map->l_next = head;
  main_map->l_prev = NULL;
  if (head)
    head->l_prev = main_map;
  _dl_link_map = main_map;

  // 7. fill main ELF's PT_TLS info (libc.so/ld.so usually have no TLS, leave 0)
  fill_tls_from_phdr(main_map, 0, phdr, phent, phnum);

  // 8. relocation: walk the list in "dependencies before dependents" order
  //    (skip ld.so itself and the main ELF)
  //    ld.so self-relocated during bootstrap; main ELF relocated last
  //    list order main -> ld -> libs...: ld is in the middle, must explicitly skip ld_map
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

  // 9. jump to main ELF entry
  dl_puts("dl: jump to entry");
  dl_put_hex(entry);

  __asm__ volatile("mov %0, %%rsp\n"
                   "jmp *%1\n"
                   :
                   : "r"(sp), "r"(entry));
  while (1)
    ;
}
