#include "kernel/ahci.h"
#include "kernel/pci.h"
#include "kernel/serial.h"
#include "kernel/mem/alloc.h"
#include "arch/x64/utils.h"
#include "common/errno.h"

// ===================== AHCI register offsets (from ABAR) =====================
// Global HBA registers
#define AHCI_CAP   0x00
#define AHCI_GHC   0x04   // HR=bit0, IE=bit1, AE=bit31
#define AHCI_IS    0x08
#define AHCI_PI    0x0C   // Ports Implemented bitmap
#define AHCI_VS    0x10

// Port register offsets (from port base = ABAR + 0x100 + port*0x80)
#define PxCLB   0x00
#define PxCLBU  0x04
#define PxFB    0x08
#define PxFBU   0x0C
#define PxIS    0x10   // TFES=bit30
#define PxIE    0x14
#define PxCMD   0x18   // ST=bit0, SUD=bit1, POD=bit2, CLO=bit3, FRE=bit4, FR=bit14, CR=bit15
#define PxTFD   0x20
#define PxSIG   0x24
#define PxSSTS  0x28   // DET=bits0:3 (3=device present+PHY)
#define PxSCTL  0x2C
#define PxSERR  0x30
#define PxSACT  0x34
#define PxCI    0x38

// Command constants
#define CMD_READ_DMA_EXT 0x25
#define CMD_WRITE_DMA_EXT 0x34
#define CMD_IDENTIFY_DEVICE 0xEC
#define FIS_H2D          0x27
#define FIS_H2D_CMD      0x80

// Bounce buffer: 16 pages = 64KB
#define AHCI_BOUNCE_PAGES 16

// ===================== Module state =====================
static uint64_t abar;
static int active_port = -1;

spinlock_t ahci_lock = SPINLOCK_INIT;

static Page *cmd_list_page;
static Page *fis_recv_page;
static Page *cmd_table_page;
static Page *bounce_page;

static uint64_t cmd_list_phys, cmd_list_virt;
static uint64_t fis_recv_phys, fis_recv_virt;
static uint64_t cmd_table_phys, cmd_table_virt;
static uint64_t bounce_phys, bounce_virt;

// ===================== Helpers =====================
static inline void *port_reg(int port, uint32_t offset) {
  return (void *)(abar + 0x100 + port * 0x80 + offset);
}

static void ahci_puts(const char *s) { serial_puts(s); }

// ===================== DMA allocation =====================
static void ahci_alloc_dma() {
  cmd_list_page = bfc_alloc.alloc_page_low(1);
  fis_recv_page = bfc_alloc.alloc_page_low(1);
  cmd_table_page = bfc_alloc.alloc_page_low(1);
  bounce_page = bfc_alloc.alloc_page_low(AHCI_BOUNCE_PAGES);

  if (!cmd_list_page || !fis_recv_page || !cmd_table_page || !bounce_page) {
    ahci_puts("ahci: DMA alloc failed\n");
    halt();
  }

  cmd_list_phys = page_to_phys(cmd_list_page);
  fis_recv_phys = page_to_phys(fis_recv_page);
  cmd_table_phys = page_to_phys(cmd_table_page);
  bounce_phys = page_to_phys(bounce_page);

  cmd_list_virt = phys_to_virt(cmd_list_phys);
  fis_recv_virt = phys_to_virt(fis_recv_phys);
  cmd_table_virt = phys_to_virt(cmd_table_phys);
  bounce_virt = phys_to_virt(bounce_phys);

  __memset((void *)cmd_list_virt, 0, 4096);
  __memset((void *)fis_recv_virt, 0, 4096);
  __memset((void *)cmd_table_virt, 0, 4096);
  __memset((void *)bounce_virt, 0, AHCI_BOUNCE_PAGES * 4096);
}

// ===================== Port stop =====================
static void port_stop(int port) {
  void *cmd = port_reg(port, PxCMD);

  // Clear ST, wait for CR=0
  writel(cmd, readl(cmd) & ~(uint32_t)1);
  for (int i = 0; i < 500000; i++) {
    if (!(readl(cmd) & (1 << 15))) break;
  }

  // Clear FRE, wait for FR=0
  writel(cmd, readl(cmd) & ~(uint32_t)(1 << 4));
  for (int i = 0; i < 500000; i++) {
    if (!(readl(cmd) & (1 << 14))) break;
  }

  // Clear PxSERR and PxIS
  writel(port_reg(port, PxSERR), readl(port_reg(port, PxSERR)));
  writel(port_reg(port, PxIS), 0xFFFFFFFF);
}

// ===================== Port init (idempotent) =====================
static void port_init(int port) {
  port_stop(port);

  // Program command list and FIS base addresses
  writel(port_reg(port, PxCLB), (uint32_t)cmd_list_phys);
  writel(port_reg(port, PxCLBU), (uint32_t)(cmd_list_phys >> 32));
  writel(port_reg(port, PxFB), (uint32_t)fis_recv_phys);
  writel(port_reg(port, PxFBU), (uint32_t)(fis_recv_phys >> 32));

  // Clear error and interrupt status
  writel(port_reg(port, PxSERR), readl(port_reg(port, PxSERR)));
  writel(port_reg(port, PxIS), 0xFFFFFFFF);

  // Enable FIS receive: set FRE, wait for FR=1
  void *cmd = port_reg(port, PxCMD);
  writel(cmd, readl(cmd) | (1 << 4));
  for (int i = 0; i < 500000; i++) {
    if (readl(cmd) & (1 << 14)) break;
  }

  // Start port: set ST, wait for CR=1
  writel(cmd, readl(cmd) | 1);
  for (int i = 0; i < 500000; i++) {
    if (readl(cmd) & (1 << 15)) break;
  }
}

// ===================== IDENTIFY DEVICE =====================
static int ahci_identify_device(int port, void *buf) {
  // Build Command Table: zero FIS area
  __memset((void *)cmd_table_virt, 0, 0x80);

  // H2D Register FIS: IDENTIFY DEVICE (0xEC)
  uint8_t *fis = (uint8_t *)cmd_table_virt;
  fis[0]  = FIS_H2D;
  fis[1]  = FIS_H2D_CMD;
  fis[2]  = CMD_IDENTIFY_DEVICE;
  fis[3]  = 0x00;   // Features (low)
  fis[4]  = 0x00;   // LBA[0:7]
  fis[5]  = 0x00;   // LBA[8:15]
  fis[6]  = 0x00;   // LBA[16:23]
  fis[7]  = 0x00;   // Device: no LBA bit per spec
  fis[8]  = 0x00;   // LBA[24:31]
  fis[9]  = 0x00;   // LBA[32:39]
  fis[10] = 0x00;   // LBA[40:47]
  fis[11] = 0x00;   // Features (high)
  fis[12] = 0x00;   // Sector count low
  fis[13] = 0x00;   // Sector Count high

  // PRD: point to bounce buffer, dbc=511 (512 bytes - 1)
  uint32_t *prd = (uint32_t *)(cmd_table_virt + 0x80);
  prd[0] = (uint32_t)(bounce_phys & 0xFFFFFFFF);
  prd[1] = (uint32_t)((bounce_phys >> 32) & 0xFFFFFFFF);
  prd[2] = 0;
  prd[3] = (511 & 0x3FFFFF) | (1U << 31);  // DBC=511 + IOC

  // Command Header (slot 0)
  uint32_t *hdr = (uint32_t *)cmd_list_virt;
  hdr[0] = (5 << 0) | (1 << 16);  // CFL=5 DW, PRDTL=1
  hdr[1] = 0;
  hdr[2] = (uint32_t)(cmd_table_phys & 0xFFFFFFFF);
  hdr[3] = (uint32_t)((cmd_table_phys >> 32) & 0xFFFFFFFF);
  hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;

  // Clear port interrupt status and issue command
  writel(port_reg(port, PxIS), 0xFFFFFFFF);
  writel(port_reg(port, PxCI), 1);

  // Poll until PxCI bit 0 clears
  for (int i = 0; i < 10000000; i++) {
    if (!(readl(port_reg(port, PxCI)) & 1)) goto done;
  }
  return -EIO;

done:
  // Check for task file error (ATAPI devices fail IDENTIFY DEVICE)
  if (readl(port_reg(port, PxIS)) & (1 << 30))
    return -EIO;

  // Copy 512 bytes from bounce buffer to caller
  __memcpy(buf, (const void *)bounce_virt, 512);
  return 0;
}

// ===================== ahci_init =====================
void ahci_init() {
  // Find AHCI controller (class 0x0106 = SATA/AHCI)
  struct pci_device *dev = pci_find_device(PCI_CLASS_STORAGE_AHCI);
  if (!dev) {
    ahci_puts("ahci: no AHCI controller found\n");
    halt();
  }

  ahci_puts("ahci: found at bus ");
  serial_put_hex(dev->bus);
  ahci_puts(" dev ");
  serial_put_hex(dev->dev);
  ahci_puts("\n");

  // Enable device: map BAR MMIO + Bus Master
  pci_enable_device(dev);
  abar = dev->bar[5].vaddr;

  ahci_puts("ahci: ABAR vaddr=");
  serial_put_hex(abar);
  ahci_puts("\n");

  // HBA reset: set GHC.HR (bit 0)
  uint32_t ghc = readl((void *)(abar + AHCI_GHC));
  writel((void *)(abar + AHCI_GHC), ghc | 1);

  // Poll until HR clears (some QEMU versions may have AE already set)
  for (int i = 0; i < 1000000; i++) {
    if (!(readl((void *)(abar + AHCI_GHC)) & 1)) break;
  }

  // Enable AHCI mode: GHC.AE (bit 31)
  writel((void *)(abar + AHCI_GHC), readl((void *)(abar + AHCI_GHC)) | (1U << 31));

  // One-time DMA allocation (shared by all candidate ports)
  ahci_alloc_dma();

  // Scan PI bitmap: probe each port with DET==3 via IDENTIFY DEVICE
  uint8_t idbuf[512];
  uint32_t pi = readl((void *)(abar + AHCI_PI));
  for (int i = 0; i < 32; i++) {
    if (!(pi & (1 << i))) continue;
    uint32_t ssts = readl(port_reg(i, PxSSTS));
    if ((ssts & 0xF) != 3) continue;

    port_init(i);
    if (ahci_identify_device(i, idbuf) == 0) {
      ahci_puts("ahci: port ");
      serial_putc('0' + i);
      ahci_puts(": SATA disk\n");
      active_port = i;
      break;
    } else {
      ahci_puts("ahci: port ");
      serial_putc('0' + i);
      ahci_puts(": ATAPI, skipping\n");
      port_stop(i);
    }
  }

  if (active_port < 0) {
    ahci_puts("ahci: no SATA disk found\n");
    halt();
  }

  // Idempotent re-initialize the active port
  port_init(active_port);
  ahci_puts("ahci: init done\n");
}

// ===================== ahci_read_lba =====================
int ahci_read_lba(uint32_t lba, uint32_t count, void *buf) {
  uint8_t *dst = (uint8_t *)buf;

  while (count > 0) {
    uint32_t chunk = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;

    // Build Command Table: zero FIS area
    __memset((void *)cmd_table_virt, 0, 0x80);

    // H2D Register FIS (20 bytes at offset 0)
    uint8_t *fis = (uint8_t *)cmd_table_virt;
    fis[0]  = FIS_H2D;          // FIS type
    fis[1]  = FIS_H2D_CMD;      // D2H register FIS with interrupt
    fis[2]  = CMD_READ_DMA_EXT; // Command
    fis[3]  = 0x00;             // Features (low)
    fis[4]  = (uint8_t)(lba & 0xFF);
    fis[5]  = (uint8_t)((lba >> 8) & 0xFF);
    fis[6]  = (uint8_t)((lba >> 16) & 0xFF);
    fis[7]  = 0x40;             // Device: LBA mode
    fis[8]  = (uint8_t)((lba >> 24) & 0xFF);
    fis[9]  = 0x00;             // LBA[32:39]
    fis[10] = 0x00;             // LBA[40:47]
    fis[11] = 0x00;             // Features (high)
    fis[12] = (uint8_t)(chunk & 0xFF);         // Sector count low
    fis[13] = (uint8_t)((chunk >> 8) & 0xFF);  // Sector count high

    // PRD entry at offset 0x80 in command table
    uint32_t *prd = (uint32_t *)(cmd_table_virt + 0x80);
    prd[0] = (uint32_t)(bounce_phys & 0xFFFFFFFF);           // DBA low
    prd[1] = (uint32_t)((bounce_phys >> 32) & 0xFFFFFFFF);  // DBA high
    prd[2] = 0;                                               // Reserved
    prd[3] = ((chunk * 512 - 1) & 0x3FFFFF) | (1U << 31);  // DBC + IOC

    // Command Header (slot 0 in command list)
    uint32_t *hdr = (uint32_t *)cmd_list_virt;
    hdr[0] = (5 << 0) | (1 << 16);  // CFL=5 DW, PRDTL=1
    hdr[1] = 0;                       // PRDBC
    hdr[2] = (uint32_t)(cmd_table_phys & 0xFFFFFFFF);         // CTBA low
    hdr[3] = (uint32_t)((cmd_table_phys >> 32) & 0xFFFFFFFF); // CTBA high
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;         // Reserved

    // Clear port interrupt status
    writel(port_reg(active_port, PxIS), 0xFFFFFFFF);

    // Issue command: set PxCI bit 0
    writel(port_reg(active_port, PxCI), 1);

    // Poll until PxCI bit 0 clears
    int timed_out = 1;
    for (int i = 0; i < 10000000; i++) {
      if (!(readl(port_reg(active_port, PxCI)) & 1)) { timed_out = 0; break; }
    }
    if (timed_out) return -EIO;

    // Check for task file error
    uint32_t pxis = readl(port_reg(active_port, PxIS));
    if (pxis & (1 << 30)) {
      ahci_puts("ahci: task file error on port ");
      serial_putc('0' + active_port);
      ahci_puts("\n");
      return -EIO;
    }

    // Copy from bounce buffer to caller
    __memcpy(dst, (const void *)bounce_virt, chunk * 512);

    dst += chunk * 512;
    lba += chunk;
    count -= chunk;
  }
  return 0;
}

// ===================== ahci_write_lba =====================
int ahci_write_lba(uint32_t lba, uint32_t count, const void *buf) {
  const uint8_t *src = (const uint8_t *)buf;

  while (count > 0) {
    uint32_t chunk = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;

    // Copy caller data into bounce buffer BEFORE issuing command
    __memcpy((void *)bounce_virt, src, chunk * 512);

    // Build Command Table: zero FIS area
    __memset((void *)cmd_table_virt, 0, 0x80);

    // H2D Register FIS (20 bytes at offset 0)
    uint8_t *fis = (uint8_t *)cmd_table_virt;
    fis[0]  = FIS_H2D;           // FIS type
    fis[1]  = FIS_H2D_CMD;       // D2H register FIS with interrupt
    fis[2]  = CMD_WRITE_DMA_EXT; // Command
    fis[3]  = 0x00;              // Features (low)
    fis[4]  = (uint8_t)(lba & 0xFF);
    fis[5]  = (uint8_t)((lba >> 8) & 0xFF);
    fis[6]  = (uint8_t)((lba >> 16) & 0xFF);
    fis[7]  = 0x40;              // Device: LBA mode
    fis[8]  = (uint8_t)((lba >> 24) & 0xFF);
    fis[9]  = 0x00;              // LBA[32:39]
    fis[10] = 0x00;              // LBA[40:47]
    fis[11] = 0x00;              // Features (high)
    fis[12] = (uint8_t)(chunk & 0xFF);         // Sector count low
    fis[13] = (uint8_t)((chunk >> 8) & 0xFF);  // Sector count high

    // PRD entry at offset 0x80 in command table
    uint32_t *prd = (uint32_t *)(cmd_table_virt + 0x80);
    prd[0] = (uint32_t)(bounce_phys & 0xFFFFFFFF);           // DBA low
    prd[1] = (uint32_t)((bounce_phys >> 32) & 0xFFFFFFFF);  // DBA high
    prd[2] = 0;                                               // Reserved
    prd[3] = ((chunk * 512 - 1) & 0x3FFFFF) | (1U << 31);  // DBC + IOC

    // Command Header (slot 0 in command list)
    uint32_t *hdr = (uint32_t *)cmd_list_virt;
    hdr[0] = (5 << 0) | (1 << 16);  // CFL=5 DW, PRDTL=1
    hdr[1] = 0;                       // PRDBC
    hdr[2] = (uint32_t)(cmd_table_phys & 0xFFFFFFFF);         // CTBA low
    hdr[3] = (uint32_t)((cmd_table_phys >> 32) & 0xFFFFFFFF); // CTBA high
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;         // Reserved

    // Clear port interrupt status
    writel(port_reg(active_port, PxIS), 0xFFFFFFFF);

    // Issue command: set PxCI bit 0
    writel(port_reg(active_port, PxCI), 1);

    // Poll until PxCI bit 0 clears
    int timed_out = 1;
    for (int i = 0; i < 10000000; i++) {
      if (!(readl(port_reg(active_port, PxCI)) & 1)) { timed_out = 0; break; }
    }
    if (timed_out) return -EIO;

    // Check for task file error
    uint32_t pxis = readl(port_reg(active_port, PxIS));
    if (pxis & (1 << 30)) {
      ahci_puts("ahci: write task file error on port ");
      serial_putc('0' + active_port);
      ahci_puts("\n");
      return -EIO;
    }

    src += chunk * 512;
    lba += chunk;
    count -= chunk;
  }
  return 0;
}
