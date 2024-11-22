#include "os-test/utils/disk_io.h"
#include "os-test/utils/elf.h"
#include "os-test/utils/os_utils.h"
#include "os-test/kernel/mem/memlayout.h"
#include "os-test/utils/x86.h"
#include <stdint.h>

static uint32_t elf_addr = KERNEL_ELF_ADDR;

static const uint32_t elf_offset = 4096 + 512;
static const uint32_t elf_sect = elf_offset / 512;

static bool isElf(Elf32_Ehdr *hdr) {
  return hdr->e_ident[EI_MAG0] == ELFMAG0 && hdr->e_ident[EI_MAG1] == ELFMAG1 &&
         hdr->e_ident[EI_MAG2] == ELFMAG2 && hdr->e_ident[EI_MAG3] == ELFMAG3;
}

void load_kernel() {
  void *addr = (void *)elf_addr;
  read_sect(addr, elf_sect);
  Elf32_Ehdr *hdr = (Elf32_Ehdr *)addr;
  if (!isElf(hdr)) {
    outw(0x8A00, 0x8A00);
    outw(0x8A00, 0x8E00);
    return;
  }

  // load each program segment (ignores ph flags)
  uint32_t phoff = hdr->e_phoff;
  uint32_t phnum = hdr->e_phnum;
  uint32_t phsize = hdr->e_phentsize * phnum;

  // Now we load elf header at 0x10000 first, but this is kernel_entry addr.
  // So we need to copy necessary info to stack. Copy entry addr this.
  uint32_t entry = hdr->e_entry;
  // Load all program headers.
  read_seg((Elf32_Phdr *)(elf_addr + phoff), phsize, phoff + elf_offset);
  {
    // Copy all program headers to stack.
    Elf32_Phdr ph[phnum];
    for (int i = 0; i < (int)phnum; i++) {
      ph[i] = *(Elf32_Phdr *)(elf_addr + phoff + i * hdr->e_phentsize);
    }
    // Load program sections.
    for (int i = 0; i < (int)phnum; ++i) {
      if (ph[i].p_memsz == 0)
        continue;
      read_seg((void *)(ph[i].p_vaddr & 0xFFFFFF), ph[i].p_memsz,
               ph[i].p_offset + elf_offset);
    }
  }

  void (*fn)(void) = (void(*)(void))(entry & 0xFFFFFF);
  fn();
}
