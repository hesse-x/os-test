// boot/stub.c - 独立 EFI bootloader
// 从 FAT32 读取 myos.elf，加载到物理地址，设置 boot_info，跳转内核
#include <efi.h>
#include <efilib.h>
#include <elf.h>
#include <stdint.h>
#include "boot/boot.h"

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

// ===================== 加载整个文件到内存 =====================
// 读取 file 的全部内容到一段 AllocatePages 分配的物理内存。
// 返回分配的物理地址（*out_phys）和字节数（*out_size）。
// 失败返回 EFI 错误码；调用者负责在失败路径上释放已分配的页面。
static EFI_STATUS load_file_to_mem(EFI_FILE_PROTOCOL *file,
                                   EFI_PHYSICAL_ADDRESS *out_phys,
                                   UINT64 *out_size) {
  // 先取文件大小
  EFI_STATUS st = uefi_call_wrapper(file->SetPosition, 2, file, 0xFFFFFFFFFFFFFFFFULL);
  if (EFI_ERROR(st)) return st;
  UINT64 file_size = 0;
  st = uefi_call_wrapper(file->GetPosition, 2, file, &file_size);
  if (EFI_ERROR(st)) return st;
  // 回到文件头
  st = uefi_call_wrapper(file->SetPosition, 2, file, 0);
  if (EFI_ERROR(st)) return st;

  // 分配页面对齐的物理内存（向上取整到 4KB 页）
  // EfiLoaderData: 归 loader 拥有，ExitBootServices 后固件不回收，
  // 内核页表映射后可直接访问（Linux initrd 同款做法）。
  UINTN npages = (UINTN)((file_size + 4095) / 4096);
  EFI_PHYSICAL_ADDRESS phys = 0;
  st = uefi_call_wrapper(BS->AllocatePages, 4,
      AllocateAnyPages, EfiLoaderData, npages, &phys);
  if (EFI_ERROR(st)) {
    Print(L"stub: AllocatePages failed (%r)\n", st);
    return st;
  }

  // 一次性读完整个文件
  UINTN want = (UINTN)file_size;
  st = uefi_call_wrapper(file->Read, 3, file, &want, (void *)phys);
  if (EFI_ERROR(st) || want != (UINTN)file_size) {
    Print(L"stub: read file failed (st=%r want=%lx size=%lx)\n",
          st, (UINT64)want, file_size);
    uefi_call_wrapper(BS->FreePages, 2, phys, npages);
    return EFI_DEVICE_ERROR;
  }

  *out_phys = phys;
  *out_size = file_size;
  return EFI_SUCCESS;
}

// ===================== 打开文件 =====================
// 从 BOOTX64.EFI 所在的卷（disk.img 的 ESP 分区）打开文件。
// 用 LoadedImage 定位装载 BOOTX64.EFI 的设备 handle，保证拿到 ESP
// 而非根分区或其他磁盘的 FAT32。
static EFI_STATUS open_file(EFI_HANDLE ImageHandle,
                            EFI_FILE_PROTOCOL **file, CHAR16 *name) {
  EFI_LOADED_IMAGE *li;
  EFI_STATUS st = uefi_call_wrapper(BS->HandleProtocol, 3,
      ImageHandle, &gEfiLoadedImageProtocolGuid, (void **)&li);
  if (EFI_ERROR(st)) {
    Print(L"stub: locate LoadedImage failed\n");
    return st;
  }

  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  st = uefi_call_wrapper(BS->HandleProtocol, 3,
      li->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void **)&fs);
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

EFI_STATUS exit_bs(EFI_SYSTEM_TABLE *SystemTable, EFI_HANDLE ImageHandle) {
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

  read_rsdp(SystemTable);

  EFI_FILE_PROTOCOL *kernel_file;
  EFI_STATUS st = open_file(ImageHandle, &kernel_file, L"myos.elf");
  if (EFI_ERROR(st)) {
    Print(L"stub: cannot open myos.elf, halting\n");
    while (1) __asm__ volatile("hlt");
  }

  UINT64 entry_vaddr = 0;
  st = load_elf(kernel_file, &entry_vaddr);
  if (EFI_ERROR(st)) {
    Print(L"stub: load ELF failed, halting\n");
    while (1) __asm__ volatile("hlt");
  }
  Print(L"stub: ELF loaded, entry=0x%lx\n", entry_vaddr);

  // Load init.elf into physical memory (initrd-style). The kernel creates
  // the init process directly from this buffer, so no early disk I/O is
  // needed. Placed on the ESP alongside myos.elf.
  EFI_FILE_PROTOCOL *init_file;
  st = open_file(ImageHandle, &init_file, L"init.elf");
  if (EFI_ERROR(st)) {
    Print(L"stub: cannot open init.elf, halting\n");
    while (1) __asm__ volatile("hlt");
  }
  EFI_PHYSICAL_ADDRESS init_phys = 0;
  UINT64 init_size = 0;
  st = load_file_to_mem(init_file, &init_phys, &init_size);
  if (EFI_ERROR(st)) {
    Print(L"stub: load init.elf failed, halting\n");
    while (1) __asm__ volatile("hlt");
  }
  bi.init_elf_addr = (uint64_t)init_phys;
  bi.init_elf_size = init_size;
  Print(L"stub: init.elf loaded, phys=0x%lx size=%lu\n",
        (UINT64)init_phys, init_size);

  st = exit_bs(SystemTable, ImageHandle);
  if (EFI_ERROR(st)) {
    while (1) __asm__ volatile("hlt");
  }

  jump_to_kernel(entry_vaddr);

  return EFI_SUCCESS;
}
