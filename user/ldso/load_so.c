// ld.so 加载 .so 到用户态内存（read + 匿名映射）
// ld.md §3.3.3 / plan_ld2b3 T15
//
// ld.so 不链 libc.a，syscall 用内联汇编（参考 dl_puts.c 模式）

#include <stddef.h>
#include <stdint.h>
#include <xos/elf.h>
#include <xos/syscall_nums.h>
#include <xos/mman.h>
#include <xos/fcntl.h>
#include <sys/link_map.h>

// bootstrap 阶段辅助（hidden）
__attribute__((visibility("hidden"))) void dl_puts(const char *s);
__attribute__((visibility("hidden"))) void dl_put_hex(uint64_t val);

// minilibc.c
__attribute__((visibility("hidden"))) void *memcpy(void *dst, const void *src, unsigned long n);
__attribute__((visibility("hidden"))) void *memset(void *dst, int c, unsigned long n);

// 内联 syscall 封装（ld.so 不链 libc.a，自带）
// 6 参数 syscall 寄存器映射：rdi, rsi, rdx, r10, r8, r9（x86-64 SysV）
static long dl_sys_open(const char *path, int flags) {
    long ret;
    __asm__ volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_OPEN), "D"((int64_t)(uintptr_t)path), "S"((int64_t)flags)
                 : "rcx", "r11", "memory");
    return ret;
}

static long dl_sys_read(int fd, void *buf, size_t len) {
    long ret;
    __asm__ volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_READ), "D"((int64_t)fd), "S"((int64_t)(uintptr_t)buf), "d"((int64_t)len)
                 : "rcx", "r11", "memory");
    return ret;
}

static long dl_sys_lseek(int fd, int64_t offset, int whence) {
    long ret;
    __asm__ volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_LSEEK), "D"((int64_t)fd), "S"(offset), "d"((int64_t)whence)
                 : "rcx", "r11", "memory");
    return ret;
}

static long dl_sys_close(int fd) {
    long ret;
    __asm__ volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CLOSE), "D"((int64_t)fd)
                 : "rcx", "r11", "memory");
    return ret;
}

// 6 参数 mmap：rdi=addr, rsi=len, rdx=prot, r10=flags, r8=fd, r9=offset
// hidden 导出：minilibc.c 的 malloc 复用同一封装，避免重复内联汇编
__attribute__((visibility("hidden")))
void *dl_sys_mmap(void *addr, size_t size, int prot, int flags, int fd, uint64_t offset) {
    long ret;
    register int64_t r10 __asm__("r10") = (int64_t)flags;
    register int64_t r8 __asm__("r8") = (int64_t)fd;
    register int64_t r9 __asm__("r9") = (int64_t)offset;
    __asm__ volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_MMAP),
                   "D"((int64_t)(uintptr_t)addr), "S"((int64_t)size),
                   "d"((int64_t)prot), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return (void *)(uintptr_t)ret;
}

// 本 OS 暂无 mprotect syscall，匿名映射 RW 隐式可执行，段权限暂不强制
static long dl_sys_mprotect(void *addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    return 0;
}

// 页对齐辅助
#define PAGE_SIZE 4096
#define PAGE_ALIGN(x) (((x) + PAGE_SIZE - 1) & ~((size_t)(PAGE_SIZE - 1)))
#define PAGE_DOWN(x)  ((x) & ~((size_t)(PAGE_SIZE - 1)))

// 静态 PHDR 缓冲区（ld.so 不用 malloc，PHDR 通常 < 4KB，最多 64 项足够）
static Elf64_Phdr dl_phdrs[64];

// 加载 .so 到用户态内存
// 返回加载基址，通过 out_dyn 输出 .dynamic 段指针
void *load_so(const char *path, Elf64_Dyn **out_dyn) {
    long fd = dl_sys_open(path, O_RDONLY);
    if (fd < 0) {
        dl_puts("dl: FATAL: cannot open ");
        dl_puts(path);
        long ret;
        __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_EXIT), "D"(1) : "rcx", "r11");
        while (1) ;
    }

    // 1. 读 ELF header
    Elf64_Ehdr ehdr;
    if (dl_sys_read((int)fd, &ehdr, sizeof(ehdr)) != (long)sizeof(ehdr)) {
        dl_puts("dl: FATAL: read ehdr failed");
        long ret;
        __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_EXIT), "D"(1) : "rcx", "r11");
        while (1) ;
    }
    // ELF magic 校验：0x7f 'E' 'L' 'F'
    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
        dl_puts("dl: FATAL: bad ELF magic");
        long ret;
        __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_EXIT), "D"(1) : "rcx", "r11");
        while (1) ;
    }

    // 2. 读所有 PHDR 到静态缓冲区
    size_t phnum = ehdr.e_phnum;
    if (phnum > sizeof(dl_phdrs) / sizeof(dl_phdrs[0])) {
        dl_puts("dl: FATAL: too many PHDRs");
        long ret;
        __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_EXIT), "D"(1) : "rcx", "r11");
        while (1) ;
    }
    dl_sys_lseek((int)fd, (int64_t)ehdr.e_phoff, SEEK_SET);
    if (dl_sys_read((int)fd, dl_phdrs, phnum * sizeof(Elf64_Phdr)) != (long)(phnum * sizeof(Elf64_Phdr))) {
        dl_puts("dl: FATAL: read PHDR failed");
        long ret;
        __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_EXIT), "D"(1) : "rcx", "r11");
        while (1) ;
    }

    // 3. 计算总加载大小（min vaddr ~ max vaddr，页对齐）
    uintptr_t min_vaddr = (uintptr_t)-1, max_vaddr = 0;
    int has_load = 0;
    for (size_t i = 0; i < phnum; i++) {
        if (dl_phdrs[i].p_type == PT_LOAD) {
            has_load = 1;
            if (dl_phdrs[i].p_vaddr < min_vaddr) min_vaddr = dl_phdrs[i].p_vaddr;
            uintptr_t end = dl_phdrs[i].p_vaddr + dl_phdrs[i].p_memsz;
            if (end > max_vaddr) max_vaddr = end;
        }
    }
    if (!has_load) {
        dl_puts("dl: FATAL: no PT_LOAD");
        long ret;
        __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_EXIT), "D"(1) : "rcx", "r11");
        while (1) ;
    }
    size_t load_sz = PAGE_ALIGN(max_vaddr) - PAGE_DOWN(min_vaddr);

    // 4. 匿名映射（MAP_PRIVATE | MAP_ANONYMOUS）
    //    本 OS 暂无 mprotect syscall，无法按 PT_LOAD p_flags 细分段权限，
    //    统一 RWX（内核 sys_mmap 对无 PROT_EXEC 的映射设 NX 位，会导致
    //    .text 段取指 #PF。load_so 注释曾写"RW 隐式可执行"是错误的）
    void *base = dl_sys_mmap(NULL, load_sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        dl_puts("dl: FATAL: mmap failed");
        long ret;
        __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_EXIT), "D"(1) : "rcx", "r11");
        while (1) ;
    }

    // 5. 逐 PT_LOAD 段：lseek + read 到映射区
    for (size_t i = 0; i < phnum; i++) {
        if (dl_phdrs[i].p_type != PT_LOAD) continue;
        void *seg_dst = (char *)base + (dl_phdrs[i].p_vaddr - min_vaddr);
        dl_sys_lseek((int)fd, (int64_t)dl_phdrs[i].p_offset, SEEK_SET);
        if (dl_phdrs[i].p_filesz > 0) {
            dl_sys_read((int)fd, seg_dst, dl_phdrs[i].p_filesz);
        }
        // BSS 清零（memsz > filesz 部分）— mmap 已返零页，无需再清零
        // 但若 filesz 段尾与 memsz 段尾跨页，mmap 已清零；同页内也清零
        if (dl_phdrs[i].p_memsz > dl_phdrs[i].p_filesz) {
            memset((char *)seg_dst + dl_phdrs[i].p_filesz, 0,
                   dl_phdrs[i].p_memsz - dl_phdrs[i].p_filesz);
        }
    }

    // 6. 段权限设置（本 OS 暂无 mprotect，匿名映射 RW+X 隐式可执行）
    //    未来加 mprotect syscall 后再启用
    (void)dl_sys_mprotect;

    // 7. 找 PT_DYNAMIC 输出 .dynamic 指针
    if (out_dyn) {
        *out_dyn = NULL;
        for (size_t i = 0; i < phnum; i++) {
            if (dl_phdrs[i].p_type == PT_DYNAMIC) {
                *out_dyn = (Elf64_Dyn *)((char *)base + (dl_phdrs[i].p_vaddr - min_vaddr));
                break;
            }
        }
    }

    dl_sys_close((int)fd);
    return base;
}
