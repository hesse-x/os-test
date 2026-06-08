// boot/stub.c - 独立 EFI bootloader
// 从 FAT32 读取 myos.elf，加载到物理地址，设置 boot_info，跳转内核
#include <efi.h>
#include <efilib.h>
#include <elf.h>
#include <stdint.h>

// 内核加载的物理地址
#define KERNEL_LOAD_ADDR  0x100000
// VMA_BASE，和内核链接脚本一致
#define VMA_BASE          0xFFFFFFFF80000000ULL
#define KERNEL_VMA_BASE   0xFFFFFFFF80100000ULL

// boot_info 魔数和结构，和 paging.h 一致
#define BOOT_INFO_MAGIC   0x4F53424F544F4F42ULL

struct boot_info {
  UINT64 magic;
  UINT64 kernel_phys;
  UINT64 rsdp;
  UINT64 fb_addr;
  UINT32 fb_width;
  UINT32 fb_height;
  UINT32 fb_pitch;
  UINT32 fb_bpp;
  UINT32 fb_pixel_format;
  UINT64 mmap_addr;
  UINT64 mmap_size;
  UINT64 mmap_desc_size;
  UINT64 mmap_desc_ver;
  // APIC info (from MADT)
  UINT64 lapic_base;
  UINT64 ioapic_base;
  UINT32 ncpus;
  UINT32 apic_ids[4];
};

// EFI 内存映射缓冲区
#define MMAP_BUF_SIZE (4096 * 4)
static UINT8 mmap_buf[MMAP_BUF_SIZE] __attribute__((aligned(16)));

// boot_info 实例
static struct boot_info bi;

// ===================== ELF 加载 =====================
static EFI_STATUS load_elf(EFI_FILE_PROTOCOL *file, UINT64 *entry_addr) {
  Elf64_Ehdr ehdr;
  UINTN size = sizeof(ehdr);
  EFI_STATUS st = uefi_call_wrapper(file->Read, 3, file, &size, &ehdr);
  if (EFI_ERROR(st) || size != sizeof(ehdr)) {
    Print(L"stub: read ELF header failed\n");
    return st;
  }

  if (ehdr.e_ident[EI_MAG0] != ELFMAG0 || ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
      ehdr.e_ident[EI_MAG2] != ELFMAG2 || ehdr.e_ident[EI_MAG3] != ELFMAG3) {
    Print(L"stub: not a valid ELF\n");
    return EFI_INVALID_PARAMETER;
  }

  *entry_addr = ehdr.e_entry;

  for (UINT16 i = 0; i < ehdr.e_phnum; i++) {
    Elf64_Phdr phdr;
    UINTN pos = ehdr.e_phoff + i * ehdr.e_phentsize;
    st = uefi_call_wrapper(file->SetPosition, 2, file, pos);
    if (EFI_ERROR(st)) return st;
    size = sizeof(phdr);
    st = uefi_call_wrapper(file->Read, 3, file, &size, &phdr);
    if (EFI_ERROR(st)) return st;

    if (phdr.p_type != PT_LOAD) continue;

    UINT64 load_addr = KERNEL_LOAD_ADDR + (phdr.p_vaddr - KERNEL_VMA_BASE);
    if (phdr.p_paddr != 0) {
      load_addr = phdr.p_paddr;
    }

    st = uefi_call_wrapper(file->SetPosition, 2, file, phdr.p_offset);
    if (EFI_ERROR(st)) return st;
    size = phdr.p_filesz;
    st = uefi_call_wrapper(file->Read, 3, file, &size, (void *)load_addr);
    if (EFI_ERROR(st)) return st;

    if (phdr.p_memsz > phdr.p_filesz) {
      SetMem((void *)(load_addr + phdr.p_filesz), phdr.p_memsz - phdr.p_filesz, 0);
    }

    Print(L"  seg %d: paddr=0x%lx size=0x%lx\n", i, load_addr, phdr.p_memsz);
  }

  return EFI_SUCCESS;
}

// ===================== 打开文件 =====================
static EFI_STATUS open_file(EFI_SYSTEM_TABLE *SystemTable,
                            EFI_FILE_PROTOCOL **file, CHAR16 *name) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  EFI_STATUS st = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
      &gEfiSimpleFileSystemProtocolGuid, NULL, (void **)&fs);
  if (EFI_ERROR(st)) {
    Print(L"stub: locate filesystem failed\n");
    return st;
  }

  EFI_FILE_PROTOCOL *root;
  st = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
  if (EFI_ERROR(st)) {
    Print(L"stub: open volume failed\n");
    return st;
  }

  st = uefi_call_wrapper(root->Open, 5, root, file, name,
                         EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(st)) {
    Print(L"stub: open %s failed\n", name);
  }
  return st;
}

// ===================== 获取 GOP =====================
static void read_gop(EFI_SYSTEM_TABLE *SystemTable) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
  EFI_STATUS st = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3,
      &gEfiGraphicsOutputProtocolGuid, NULL, (void **)&gop);
  if (EFI_ERROR(st) || gop == NULL || gop->Mode == NULL) {
    return;
  }
  bi.fb_addr = gop->Mode->FrameBufferBase;
  if (gop->Mode->Info) {
    bi.fb_width = gop->Mode->Info->HorizontalResolution;
    bi.fb_height = gop->Mode->Info->VerticalResolution;
    bi.fb_pitch = gop->Mode->Info->PixelsPerScanLine * 4;
    bi.fb_bpp = 32;
    bi.fb_pixel_format = gop->Mode->Info->PixelFormat;
  }
  Print(L"stub: GOP %dx%d pitch=%d bpp=%d fb=0x%lx\n",
        bi.fb_width, bi.fb_height, bi.fb_pitch, bi.fb_bpp, bi.fb_addr);
}

// ===================== 获取 RSDP =====================
static void read_rsdp(EFI_SYSTEM_TABLE *SystemTable) {
  for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
    EFI_GUID acpi_guid = ACPI_20_TABLE_GUID;
    UINT8 *a = (UINT8 *)&SystemTable->ConfigurationTable[i].VendorGuid;
    UINT8 *b = (UINT8 *)&acpi_guid;
    int match = 1;
    for (int j = 0; j < 16; j++) {
      if (a[j] != b[j]) { match = 0; break; }
    }
    if (match) {
      bi.rsdp = (UINT64)SystemTable->ConfigurationTable[i].VendorTable;
      Print(L"stub: RSDP=0x%lx\n", bi.rsdp);
      return;
    }
  }
}

// ===================== ACPI table helpers =====================
// RSDP is at 16-byte alignment, XSDT entry is 8 bytes
static UINT8 checksum(UINT8 *tbl, UINTN len) {
  UINT8 sum = 0;
  for (UINTN i = 0; i < len; i++) sum += tbl[i];
  return sum;
}

#pragma pack(push, 1)
typedef struct {
  UINT8  signature[8];
  UINT8  checksum;
  UINT8  oem_id[6];
  UINT8  revision;
  UINT32 rsdt_address;
  UINT32 length;
  UINT64 xsdt_address;
} RSDP;
typedef struct {
  UINT8  signature[4];
  UINT32 length;
  UINT8  revision;
  UINT8  checksum;
  UINT8  oem_id[6];
  UINT8  oem_table_id[8];
  UINT32 oem_revision;
  UINT32 creator_id;
  UINT32 creator_revision;
} SDT_HEADER;
typedef struct {
  SDT_HEADER header;
  UINT32 local_apic_address;
  UINT32 flags;
} MADT;
typedef struct {
  UINT8 type;
  UINT8 length;
} MADT_ENTRY;
typedef struct {
  UINT8 type;   // 0
  UINT8 length;
  UINT8 processor_id;
  UINT8 apic_id;
  UINT32 flags;
} MADT_LAPIC_ENTRY;
typedef struct {
  UINT8 type;   // 1
  UINT8 length;
  UINT8 ioapic_id;
  UINT8 reserved;
  UINT32 ioapic_address;
  UINT32 gsi_base;
} MADT_IOAPIC_ENTRY;
#pragma pack(pop)

static void read_madt() {
  RSDP *rsdp = (RSDP *)bi.rsdp;
  if (rsdp == NULL) return;
  // Validate RSDP signature "RSD PTR "
  if (rsdp->signature[0] != 'R' || rsdp->signature[1] != 'S' ||
      rsdp->signature[2] != 'D' || rsdp->signature[3] != ' ' ||
      rsdp->signature[4] != 'P' || rsdp->signature[5] != 'T' ||
      rsdp->signature[6] != 'R' || rsdp->signature[7] != ' ') {
    Print(L"stub: RSDP signature mismatch\n");
    return;
  }

  // Use XSDT if revision >= 2 and xsdt_address is valid
  UINT8 *sdt = NULL;
  UINT8 entry_size = 4; // RSDT entries are 4 bytes
  if (rsdp->revision >= 2 && rsdp->xsdt_address) {
    sdt = (UINT8 *)(uintptr_t)rsdp->xsdt_address;
    entry_size = 8; // XSDT entries are 8 bytes
  } else {
    sdt = (UINT8 *)(uintptr_t)rsdp->rsdt_address;
  }
  if (sdt == NULL) return;

  SDT_HEADER *sdt_hdr = (SDT_HEADER *)sdt;
  UINTN entry_count = (sdt_hdr->length - sizeof(SDT_HEADER)) / entry_size;

  // Scan for MADT (signature "APIC")
  for (UINTN i = 0; i < entry_count; i++) {
    UINT64 addr;
    if (entry_size == 8) {
      addr = *(UINT64 *)(sdt + sizeof(SDT_HEADER) + i * 8);
    } else {
      addr = *(UINT32 *)(sdt + sizeof(SDT_HEADER) + i * 4);
    }
    MADT *madt = (MADT *)(uintptr_t)addr;
    if (madt->header.signature[0] != 'A' || madt->header.signature[1] != 'P' ||
        madt->header.signature[2] != 'I' || madt->header.signature[3] != 'C') {
      continue;
    }
    Print(L"stub: MADT at 0x%lx, len=%d\n", addr, madt->header.length);

    bi.lapic_base = madt->local_apic_address;

    // Parse MADT entries for LAPIC and IOAPIC
    UINTN offset = sizeof(MADT);
    bi.ncpus = 0;
    while (offset < madt->header.length) {
      MADT_ENTRY *e = (MADT_ENTRY *)((UINT8 *)madt + offset);
      if (e->length == 0) break;
      if (e->type == 0 && bi.ncpus < 4) {
        // LAPIC entry
        MADT_LAPIC_ENTRY *lapic = (MADT_LAPIC_ENTRY *)e;
        if (lapic->flags & 1) { // enabled
          bi.apic_ids[bi.ncpus] = lapic->apic_id;
          bi.ncpus++;
          Print(L"  LAPIC: id=%d\n", lapic->apic_id);
        }
      } else if (e->type == 1 && bi.ioapic_base == 0) {
        // IOAPIC entry (use first one)
        MADT_IOAPIC_ENTRY *ioapic = (MADT_IOAPIC_ENTRY *)e;
        bi.ioapic_base = ioapic->ioapic_address;
        Print(L"  IOAPIC: addr=0x%x gsi_base=%d\n", ioapic->ioapic_address, ioapic->gsi_base);
      }
      offset += e->length;
    }
    break; // only need first MADT
  }
  Print(L"stub: lapic=0x%lx ioapic=0x%lx ncpus=%d\n", bi.lapic_base, bi.ioapic_base, bi.ncpus);
}

// ===================== ExitBootServices =====================
static EFI_STATUS exit_bs(EFI_SYSTEM_TABLE *SystemTable, EFI_HANDLE ImageHandle) {
  UINTN map_key, mmap_size, desc_size;
  UINT32 desc_ver;

  mmap_size = sizeof(mmap_buf);
  EFI_STATUS st = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
      &mmap_size, (void *)mmap_buf, &map_key, &desc_size, &desc_ver);
  if (EFI_ERROR(st)) {
    Print(L"stub: GetMemoryMap failed\n");
    return st;
  }

  bi.mmap_addr = (UINT64)(uintptr_t)mmap_buf;
  bi.mmap_size = mmap_size;
  bi.mmap_desc_size = desc_size;
  bi.mmap_desc_ver = desc_ver;

  st = uefi_call_wrapper(SystemTable->BootServices->ExitBootServices, 2,
                         ImageHandle, map_key);
  if (EFI_ERROR(st)) {
    Print(L"stub: ExitBootServices failed\n");
  }
  return st;
}

// ===================== 跳转内核 =====================
__attribute__((noreturn)) static void jump_to_kernel(UINT64 entry_vaddr) {
  UINT64 entry_phys = entry_vaddr - VMA_BASE;
  typedef void (*kernel_entry_t)(struct boot_info *);
  kernel_entry_t entry = (kernel_entry_t)entry_phys;
  entry(&bi);
  while (1) __asm__ volatile("hlt");
}

// ===================== efi_main =====================
EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  InitializeLib(ImageHandle, SystemTable);
  Print(L"stub: efi_main started\n");

  SetMem(&bi, sizeof(bi), 0);
  bi.magic = BOOT_INFO_MAGIC;
  bi.kernel_phys = KERNEL_LOAD_ADDR;

  read_gop(SystemTable);
  read_rsdp(SystemTable);
  read_madt();

  EFI_FILE_PROTOCOL *kernel_file;
  EFI_STATUS st = open_file(SystemTable, &kernel_file, L"myos.elf");
  if (EFI_ERROR(st)) {
    Print(L"stub: cannot open myos.elf, halting\n");
    while (1) __asm__ volatile("hlt");
  }

  UINT64 entry_vaddr;
  st = load_elf(kernel_file, &entry_vaddr);
  if (EFI_ERROR(st)) {
    Print(L"stub: load ELF failed, halting\n");
    while (1) __asm__ volatile("hlt");
  }
  Print(L"stub: ELF loaded, entry=0x%lx\n", entry_vaddr);

  st = exit_bs(SystemTable, ImageHandle);
  if (EFI_ERROR(st)) {
    while (1) __asm__ volatile("hlt");
  }

  jump_to_kernel(entry_vaddr);

  return EFI_SUCCESS;
}
