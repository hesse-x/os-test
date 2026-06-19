#include "kernel/ahci.h"
#include "kernel/pci.h"
#include "kernel/serial.h"
#include "kernel/mem/alloc.h"
#include "kernel/proc.h"
#include "kernel/trap.h"
#include "arch/x64/utils.h"
#include "arch/x64/apic.h"
#include "arch/x64/smp.h"
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
#define PxIS    0x10   // DHRS=bit0, TFES=bit30
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

// ===================== Block request queue (async I/O) =====================
#define BLOCK_QUEUE_SIZE 32

struct block_req {
    pid_t       caller_pid;
    uint32_t    lba;
    uint32_t    count;          // sector count (1..AHCI_MAX_SECTORS)
    uint8_t     dir;            // 0=read, 1=write
    void       *user_buf;       // user-space virtual address
    uint32_t    cookie;         // monotonic ID for completion matching
    int         result;         // 0=ok, EIO=error
};

static block_req block_pool[BLOCK_QUEUE_SIZE];
static int bq_head = 0;     // next slot to dequeue
static int bq_tail = 0;     // next slot to enqueue
static int bq_count = 0;    // number of queued requests
block_req *ahci_current_req = nullptr;  // in-flight request
static uint32_t ahci_cookie_counter = 0;

// ===================== Helpers =====================
static inline void *port_reg(int port, uint32_t offset) {
  return (void *)(abar + 0x100 + port * 0x80 + offset);
}

static void ahci_puts(const char *s) { serial_puts(s); }

// ===================== Page-table walk: bounce → user pages =====================
// Walk target process's page tables from proc->cr3 (physical address)
// to find physical pages backing the user buffer, then copy bounce buffer
// data via kernel higher-half mapping. Safe in IRQ context (no CR3 switch).

static uint64_t walk_user_pt(uint64_t cr3_phys, uint64_t vaddr) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(cr3_phys);
    int pml4_idx = (vaddr >> 39) & 0x1FF;
    if (!(pml4[pml4_idx] & PTE_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL);
    int pdpt_idx = (vaddr >> 30) & 0x1FF;
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return 0;
    if (pdpt[pdpt_idx] & PTE_PS)  // 1GB huge page
        return (pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL) + (vaddr & 0x3FFFFFFF);

    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL);
    int pd_idx = (vaddr >> 21) & 0x1FF;
    if (!(pd[pd_idx] & PTE_PRESENT)) return 0;
    if (pd[pd_idx] & PTE_PS)  // 2MB huge page
        return (pd[pd_idx] & 0x000FFFFFFFFFF000ULL) + (vaddr & 0x1FFFFF);

    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pd_idx] & 0x000FFFFFFFFFF000ULL);
    int pt_idx = (vaddr >> 12) & 0x1FF;
    if (!(pt[pt_idx] & PTE_PRESENT)) return 0;
    return (pt[pt_idx] & 0x000FFFFFFFFFF000ULL) + (vaddr & 0xFFF);  // 4KB page
}

// Copy bounce buffer data to a user process's buffer via page-table walk.
// Returns true on success, false if page walk failed.
static bool bounce_to_user_pages(pid_t pid, void *user_buf, uint32_t byte_len) {
    if (pid < 0 || pid >= MAX_PROC) return false;
    proc_t *proc = &procs[pid];
    if (proc->pid != pid || proc->state == ZOMBIE || proc->state == REAPING) return false;

    uint64_t cr3 = proc->cr3;  // physical address of PML4
    uint64_t dst_va = (uint64_t)user_buf;
    uint32_t offset = 0;

    while (offset < byte_len) {
        uint64_t page_va = (dst_va + offset) & ~0xFFFULL;  // ALIGN_DOWN to page
        uint64_t page_offset = (dst_va + offset) - page_va;
        uint32_t remaining = byte_len - offset;
        uint32_t chunk = remaining < (4096 - page_offset) ? remaining : (4096 - (uint32_t)page_offset);

        uint64_t phys = walk_user_pt(cr3, page_va);
        if (phys == 0) return false;

        __memcpy((void *)(phys_to_virt(phys) + page_offset),
                 (const void *)(bounce_virt + offset), chunk);
        offset += chunk;
    }
    return true;
}

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

  // Enable port interrupts: DHRE (bit0) for command completion notification
  // + error bits: TFEE (bit30), HBFE (bit31), HBDIE (bit29)
  writel(port_reg(port, PxIE), (1U << 0) | (1U << 30) | (1U << 31) | (1U << 29));

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

// ===================== AHCI IRQ handler =====================
// Called when AHCI port interrupt fires (command completion).
// Completes the current async request, notifies caller, issues next queued request.

void ahci_issue_cmd(block_req *req);  // forward declaration

static void ahci_irq_handler(trapframe_t *tf) {
    // Check if our port generated the interrupt
    uint32_t pxis = readl(port_reg(active_port, PxIS));
    if (pxis == 0) {
        // Not our interrupt — still acknowledge and EOI
        writel((void *)(abar + AHCI_IS), readl((void *)(abar + AHCI_IS)));
        lapic_eoi();
        return;
    }

    ahci_puts("ahci: IRQ on port ");
    serial_putc('0' + active_port);
    ahci_puts(" pxis=0x");
    serial_put_hex(pxis);
    ahci_puts("\n");

    // Acknowledge port interrupt status
    writel(port_reg(active_port, PxIS), pxis);
    // Acknowledge global IS
    writel((void *)(abar + AHCI_IS), readl((void *)(abar + AHCI_IS)));

    // Check if a command was in flight
    if (!ahci_current_req) {
        lapic_eoi();
        return;
    }

    // Check for error (TFES = bit30)
    bool error = (pxis & (1U << 30));

    // For reads: copy bounce buffer data to user buffer via page-table walk
    if (ahci_current_req->dir == 0 && !error) {
        uint32_t byte_len = ahci_current_req->count * 512;
        bool ok = bounce_to_user_pages(ahci_current_req->caller_pid,
                                       ahci_current_req->user_buf, byte_len);
        if (!ok) error = true;  // page walk failed = I/O error
    }

    ahci_current_req->result = error ? EIO : 0;

    // Build RECV_NOTIFY completion message
    recv_msg msg;
    msg.type = RECV_NOTIFY;
    msg.src = 0;  // kernel disk completion
    __memset(msg.data, 0, 56);
    // Pack: cookie(4) + result(4) + lba(4) + count(4)
    __memcpy(msg.data, &ahci_current_req->cookie, 4);
    __memcpy(msg.data + 4, &ahci_current_req->result, 4);
    __memcpy(msg.data + 8, &ahci_current_req->lba, 4);
    __memcpy(msg.data + 12, &ahci_current_req->count, 4);

    pid_t caller = ahci_current_req->caller_pid;
    ahci_current_req = nullptr;
    bq_count--;
    bq_head = (bq_head + 1) % BLOCK_QUEUE_SIZE;

    ahci_puts("ahci: complete cookie=");
    serial_put_hex(*(uint32_t*)msg.data);
    ahci_puts(" to pid=");
    serial_put_hex(caller);
    ahci_puts("\n");

    // Notify caller process
    notify_and_wake(caller, &msg);

    // Issue next queued request if available
    if (bq_count > 0) {
        ahci_current_req = &block_pool[bq_head];
        ahci_issue_cmd(ahci_current_req);
    }

    lapic_eoi();
}

// ===================== Command issue with memory barrier =====================
// Issue command slot 0 on the active port. Must be called after command table
// and header are fully written. Uses MFENCE to ensure memory writes are visible
// to the HBA before the PxCI doorbell write.
static inline void ahci_issue_command() {
    // Ensure all command table and header writes are visible before PxCI
    __sync_synchronize();  // full memory barrier (MFENCE on x86-64)
    writel(port_reg(active_port, PxCI), 1);
}

// ===================== ahci_issue_cmd =====================
// Build FIS + PRD + command header and issue PxCI=1 (no polling).
// For writes: bounce buffer must already contain the data (done in syscall context).

void ahci_issue_cmd(block_req *req) {
    uint32_t lba = req->lba;
    uint32_t chunk = req->count;
    uint8_t cmd_byte = (req->dir == 0) ? CMD_READ_DMA_EXT : CMD_WRITE_DMA_EXT;

    ahci_puts("ahci: issue port=");
    serial_putc('0' + active_port);
    ahci_puts(" lba=0x");
    serial_put_hex(lba);
    ahci_puts(" cnt=");
    serial_put_hex(chunk);
    ahci_puts("\n");

    // Build Command Table: zero FIS area
    __memset((void *)cmd_table_virt, 0, 0x80);

    // H2D Register FIS
    uint8_t *fis = (uint8_t *)cmd_table_virt;
    fis[0]  = FIS_H2D;          // FIS type
    fis[1]  = FIS_H2D_CMD;      // D2H register FIS with interrupt
    fis[2]  = cmd_byte;
    fis[3]  = 0x00;             // Features (low)
    fis[4]  = (uint8_t)(lba & 0xFF);
    fis[5]  = (uint8_t)((lba >> 8) & 0xFF);
    fis[6]  = (uint8_t)((lba >> 16) & 0xFF);
    fis[7]  = 0x40;             // Device: LBA mode
    fis[8]  = (uint8_t)((lba >> 24) & 0xFF);
    fis[9]  = 0x00;             // LBA[32:39]
    fis[10] = 0x00;             // LBA[40:47]
    fis[11] = 0x00;             // Features (high)
    fis[12] = (uint8_t)(chunk & 0xFF);
    fis[13] = (uint8_t)((chunk >> 8) & 0xFF);

    // PRD entry at offset 0x80 in command table
    uint32_t *prd = (uint32_t *)(cmd_table_virt + 0x80);
    prd[0] = (uint32_t)(bounce_phys & 0xFFFFFFFF);
    prd[1] = (uint32_t)((bounce_phys >> 32) & 0xFFFFFFFF);
    prd[2] = 0;
    prd[3] = ((chunk * 512 - 1) & 0x3FFFFF) | (1U << 31);

    // Command Header (slot 0)
    uint32_t *hdr = (uint32_t *)cmd_list_virt;
    hdr[0] = (5 << 0) | (1 << 16);  // CFL=5 DW, PRDTL=1
    hdr[1] = 0;
    hdr[2] = (uint32_t)(cmd_table_phys & 0xFFFFFFFF);
    hdr[3] = (uint32_t)((cmd_table_phys >> 32) & 0xFFFFFFFF);
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;

    // Clear port interrupt status before issuing
    writel(port_reg(active_port, PxIS), 0xFFFFFFFF);

    ahci_puts("ahci: issue PxCI before=");
    serial_put_hex(readl(port_reg(active_port, PxCI)));
    ahci_puts("\n");

    // Issue command with memory barrier
    ahci_issue_command();

    ahci_puts("ahci: issue PxCI after=");
    serial_put_hex(readl(port_reg(active_port, PxCI)));
    ahci_puts(" PxCMD=");
    serial_put_hex(readl(port_reg(active_port, PxCMD)));
    ahci_puts(" PxTFD=");
    serial_put_hex(readl(port_reg(active_port, PxTFD)));
    ahci_puts("\n");
}

// ===================== Port COMRESET =====================
// Force device detection via COMRESET on a port, then wait for DET=3.
// Returns 0 on success, -EIO if device not detected after reset.
static int ahci_comreset_port(int port) {
  // Stop port before reset
  void *cmd = port_reg(port, PxCMD);
  writel(cmd, readl(cmd) & ~(uint32_t)1);  // clear ST
  for (int i = 0; i < 500000; i++) {
    if (!(readl(cmd) & (1 << 15))) break;  // wait CR=0
  }
  writel(cmd, readl(cmd) & ~(uint32_t)(1 << 4));  // clear FRE
  for (int i = 0; i < 500000; i++) {
    if (!(readl(cmd) & (1 << 14))) break;  // wait FR=0
  }

  // Issue COMRESET via PxSCTL.DET=1
  writel(port_reg(port, PxSCTL), 1);
  // Wait at least 1ms (approx loop)
  for (int i = 0; i < 100000; i++) __asm__ volatile("pause");
  // Clear COMRESET
  writel(port_reg(port, PxSCTL), 0);

  // Wait for device detection (DET=3), up to 100ms
  int det = 0;
  for (int i = 0; i < 5000000; i++) {
    uint32_t ssts = readl(port_reg(port, PxSSTS));
    det = ssts & 0xF;
    if (det == 3) break;
    __asm__ volatile("pause");
  }
  ahci_puts("ahci: comreset port "); serial_put_hex(port);
  ahci_puts(" DET="); serial_put_hex(det); ahci_puts("\n");

  writel(port_reg(port, PxSERR), 0xFFFFFFFF);
  writel(port_reg(port, PxIS), 0xFFFFFFFF);
  return (det == 3) ? 0 : -EIO;
}

// ===================== ahci_set_active_port =====================
// Switch the active port for polling I/O. Returns 0 on success, -EIO if port has no device.
// Tries COMRESET if the port doesn't show DET=3 initially.
int ahci_set_active_port(int port) {
  // Check if port has a device; try COMRESET if not detected
  uint32_t ssts = readl(port_reg(port, PxSSTS));
  uint32_t det = ssts & 0xF;
  if (det != 3) {
    ahci_puts("ahci: port "); serial_put_hex(port); ahci_puts(" DET="); serial_put_hex(det); ahci_puts(" trying COMRESET...\n");
    if (ahci_comreset_port(port) != 0) {
      ahci_puts("ahci: port "); serial_put_hex(port); ahci_puts(" no device after COMRESET\n");
      return -EIO;
    }
  }
  ahci_puts("ahci: switching to port "); serial_put_hex(port); ahci_puts("\n");
  port_init(port);
  active_port = port;
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

  // Poll until HR clears
  for (int i = 0; i < 1000000; i++) {
    if (!(readl((void *)(abar + AHCI_GHC)) & 1)) break;
  }

  // Enable AHCI mode: GHC.AE (bit 31)
  writel((void *)(abar + AHCI_GHC), readl((void *)(abar + AHCI_GHC)) | (1U << 31));

  // One-time DMA allocation (shared by all candidate ports)
  ahci_alloc_dma();

  // Scan PI bitmap: probe each port with DET==3 via IDENTIFY DEVICE
  // Scan ALL ports — with boot.img on port 0 and disk.img on port 2 (ide.1 master),
  // active_port ends up as port 2 (disk.img) where the ELF files live.
  uint8_t idbuf[512];
  int disk_count = 0;
  active_port = -1;
  uint32_t pi = readl((void *)(abar + AHCI_PI));
  ahci_puts("ahci: PI=0x");
  serial_put_hex(pi);
  ahci_puts("\n");
  for (int i = 0; i < 32; i++) {
    if (!(pi & (1 << i))) continue;
    uint32_t ssts = readl(port_reg(i, PxSSTS));
    uint32_t det = ssts & 0xF;
    ahci_puts("ahci: port "); serial_put_hex(i);
    ahci_puts(" PI=1 SSTS=0x"); serial_put_hex(ssts);
    ahci_puts(" DET="); serial_put_hex(det);
    if (det != 3) { ahci_puts(" (no device)\n"); continue; }

    port_init(i);
    if (ahci_identify_device(i, idbuf) == 0) {
      ahci_puts(" SATA disk\n");
      // Keep the first detected port as active; try all ports for ELF later
      if (active_port < 0) active_port = i;
      disk_count++;
    } else {
      ahci_puts(" ATAPI, skipping\n");
      port_stop(i);
    }
  }

  // If only 1 disk found, also check all PI ports by trying COMRESET + detect
  if (disk_count <= 1) {
    ahci_puts("ahci: fallback: COMRESET all PI ports\n");
    for (int i = 0; i < 32; i++) {
      if (!(pi & (1 << i))) continue;
      if (i == active_port) continue;  // already scanned
      uint32_t ssts = readl(port_reg(i, PxSSTS));
      uint32_t det = ssts & 0xF;
      ahci_puts("ahci: re-scan port "); serial_put_hex(i);
      ahci_puts(" DET="); serial_put_hex(det);
      if (det != 3) {
        // Try COMRESET to force device detection
        if (ahci_comreset_port(i) != 0) {
          ahci_puts(" (no device after COMRESET)\n");
          continue;
        }
      } else {
        ahci_puts(" (already detected)\n");
      }
      port_init(i);
      if (ahci_identify_device(i, idbuf) == 0) {
        ahci_puts("ahci: port "); serial_put_hex(i); ahci_puts(": SATA disk (fallback)\n");
        if (active_port < 0) active_port = i;
        disk_count++;
      } else {
        ahci_puts("ahci: port "); serial_put_hex(i); ahci_puts(": IDENTIFY failed\n");
        port_stop(i);
      }
    }
  }

  ahci_puts("ahci: disks found=");
  serial_put_hex(disk_count);
  ahci_puts(" active=");
  serial_put_hex(active_port);
  ahci_puts("\n");

  if (active_port < 0) {
    ahci_puts("ahci: no SATA disk found\n");
    halt();
  }

  // Idempotent re-initialize the active port
  port_init(active_port);

  // Use the interrupt line programmed by firmware (OVMF sets this correctly for ICH9)
  uint8_t ahci_gsi = dev->irq_line;
  uint8_t ahci_irq_vec = 32 + ahci_gsi;

  // Enable AHCI interrupts:
  // 1. Register IRQ handler for AHCI vector
  register_irq(ahci_irq_vec, ahci_irq_handler);

  // 2. Enable GHC.IE (global HBA interrupt enable, bit 1)
  writel((void *)(abar + AHCI_GHC), readl((void *)(abar + AHCI_GHC)) | (1U << 1));

  // 3. Route I/O APIC GSI → AHCI vector, unmasked, to BSP
  uint32_t bsp_apic_id = lapic_read(LAPIC_ID) >> 24;
  ioapic_set_irq(ahci_gsi, ahci_irq_vec, bsp_apic_id, false);

  ahci_puts("ahci: interrupts enabled (GSI=");
  serial_put_hex(ahci_gsi);
  ahci_puts(" vec=");
  serial_put_hex(ahci_irq_vec);
  ahci_puts(")\n");

  ahci_puts("ahci: init done\n");
}

// ===================== ahci_read_lba (polling, for init-time ELF loading) =====================
int ahci_read_lba(uint32_t lba, uint32_t count, void *buf) {
  uint8_t *dst = (uint8_t *)buf;

  ahci_puts("ahci_read_lba: port=");
  serial_put_hex(active_port);
  ahci_puts(" lba=0x");
  serial_put_hex(lba);
  ahci_puts(" count=0x");
  serial_put_hex(count);
  ahci_puts("\n");

  while (count > 0) {
    uint32_t chunk = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;

    __memset((void *)cmd_table_virt, 0, 0x80);

    uint8_t *fis = (uint8_t *)cmd_table_virt;
    fis[0]  = FIS_H2D;
    fis[1]  = FIS_H2D_CMD;
    fis[2]  = CMD_READ_DMA_EXT;
    fis[3]  = 0x00;
    fis[4]  = (uint8_t)(lba & 0xFF);
    fis[5]  = (uint8_t)((lba >> 8) & 0xFF);
    fis[6]  = (uint8_t)((lba >> 16) & 0xFF);
    fis[7]  = 0x40;
    fis[8]  = (uint8_t)((lba >> 24) & 0xFF);
    fis[9]  = 0x00;
    fis[10] = 0x00;
    fis[11] = 0x00;
    fis[12] = (uint8_t)(chunk & 0xFF);
    fis[13] = (uint8_t)((chunk >> 8) & 0xFF);

    uint32_t *prd = (uint32_t *)(cmd_table_virt + 0x80);
    prd[0] = (uint32_t)(bounce_phys & 0xFFFFFFFF);
    prd[1] = (uint32_t)((bounce_phys >> 32) & 0xFFFFFFFF);
    prd[2] = 0;
    prd[3] = ((chunk * 512 - 1) & 0x3FFFFF) | (1U << 31);

    uint32_t *hdr = (uint32_t *)cmd_list_virt;
    hdr[0] = (5 << 0) | (1 << 16);
    hdr[1] = 0;
    hdr[2] = (uint32_t)(cmd_table_phys & 0xFFFFFFFF);
    hdr[3] = (uint32_t)((cmd_table_phys >> 32) & 0xFFFFFFFF);
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;

    writel(port_reg(active_port, PxIS), 0xFFFFFFFF);
    ahci_issue_command();

    // Poll until PxCI bit 0 clears
    int timed_out = 1;
    for (int i = 0; i < 10000000; i++) {
      if (!(readl(port_reg(active_port, PxCI)) & 1)) { timed_out = 0; break; }
    }
    if (timed_out) {
      ahci_puts("ahci_read_lba: TIMEOUT (PxCI stuck)\n");
      uint32_t tfd = readl(port_reg(active_port, PxTFD));
      ahci_puts("  PxTFD=0x"); serial_put_hex(tfd); ahci_puts(" PxCMD=0x"); serial_put_hex(readl(port_reg(active_port, PxCMD))); ahci_puts(" PxIS=0x"); serial_put_hex(readl(port_reg(active_port, PxIS))); ahci_puts("\n");
      return -EIO;
    }

    uint32_t pxis = readl(port_reg(active_port, PxIS));
    if (pxis & (1 << 30)) {
      ahci_puts("ahci: task file error on port ");
      serial_put_hex(active_port);
      ahci_puts(" PxIS=0x"); serial_put_hex(pxis); ahci_puts(" PxTFD=0x"); serial_put_hex(readl(port_reg(active_port, PxTFD))); ahci_puts("\n");
      return -EIO;
    }

    __memcpy(dst, (const void *)bounce_virt, chunk * 512);

    dst += chunk * 512;
    lba += chunk;
    count -= chunk;
  }
  return 0;
}

// ===================== ahci_write_lba (polling, for init-time) =====================
int ahci_write_lba(uint32_t lba, uint32_t count, const void *buf) {
  const uint8_t *src = (const uint8_t *)buf;

  while (count > 0) {
    uint32_t chunk = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;

    __memcpy((void *)bounce_virt, src, chunk * 512);

    __memset((void *)cmd_table_virt, 0, 0x80);

    uint8_t *fis = (uint8_t *)cmd_table_virt;
    fis[0]  = FIS_H2D;
    fis[1]  = FIS_H2D_CMD;
    fis[2]  = CMD_WRITE_DMA_EXT;
    fis[3]  = 0x00;
    fis[4]  = (uint8_t)(lba & 0xFF);
    fis[5]  = (uint8_t)((lba >> 8) & 0xFF);
    fis[6]  = (uint8_t)((lba >> 16) & 0xFF);
    fis[7]  = 0x40;
    fis[8]  = (uint8_t)((lba >> 24) & 0xFF);
    fis[9]  = 0x00;
    fis[10] = 0x00;
    fis[11] = 0x00;
    fis[12] = (uint8_t)(chunk & 0xFF);
    fis[13] = (uint8_t)((chunk >> 8) & 0xFF);

    uint32_t *prd = (uint32_t *)(cmd_table_virt + 0x80);
    prd[0] = (uint32_t)(bounce_phys & 0xFFFFFFFF);
    prd[1] = (uint32_t)((bounce_phys >> 32) & 0xFFFFFFFF);
    prd[2] = 0;
    prd[3] = ((chunk * 512 - 1) & 0x3FFFFF) | (1U << 31);

    uint32_t *hdr = (uint32_t *)cmd_list_virt;
    hdr[0] = (5 << 0) | (1 << 16);
    hdr[1] = 0;
    hdr[2] = (uint32_t)(cmd_table_phys & 0xFFFFFFFF);
    hdr[3] = (uint32_t)((cmd_table_phys >> 32) & 0xFFFFFFFF);
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;

    writel(port_reg(active_port, PxIS), 0xFFFFFFFF);
    ahci_issue_command();

    int timed_out = 1;
    for (int i = 0; i < 10000000; i++) {
      if (!(readl(port_reg(active_port, PxCI)) & 1)) { timed_out = 0; break; }
    }
    if (timed_out) {
      return -EIO;
    }

    uint32_t pxis = readl(port_reg(active_port, PxIS));
    if (pxis & (1 << 30)) {
      ahci_puts("ahci: write task file error on port ");
      serial_put_hex(active_port);
      ahci_puts(" PxIS=0x"); serial_put_hex(pxis); ahci_puts("\n");
      return -EIO;
    }

    src += chunk * 512;
    lba += chunk;
    count -= chunk;
  }
  return 0;
}

// ===================== Async block I/O interface =====================

bool ahci_is_busy() {
    return ahci_current_req != nullptr;
}

// Submit async block request. Returns cookie (>0) on success, -errno on error.
// Completion delivered via RECV_NOTIFY to caller.
int ahci_submit_async(uint32_t lba, void *buf, uint32_t count, uint8_t dir) {
    // Validate user buffer
    uint64_t ptr = (uint64_t)buf;
    if (!ptr || ptr >= 0xFFFFFFFF80000000ULL)
        return EFAULT;
    uint64_t end = ptr + (uint64_t)count * 512;
    if (end < ptr || end > 0xFFFFFFFF80000000ULL)
        return EFAULT;
    if (count == 0 || count > AHCI_MAX_SECTORS)
        return EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&ahci_lock, &flags);

    if (bq_count >= BLOCK_QUEUE_SIZE) {
        spin_unlock_irqrestore(&ahci_lock, flags);
        return EBUSY;
    }

    // For writes: copy user data to bounce buffer (in syscall context, CR3 loaded)
    if (dir == 1) {
        __memcpy((void *)bounce_virt, buf, (size_t)count * 512);
    }

    // Allocate request from pool
    block_req *req = &block_pool[bq_tail];
    req->caller_pid = current_proc->pid;
    req->lba = lba;
    req->count = count;
    req->dir = dir;
    req->user_buf = buf;
    req->cookie = ++ahci_cookie_counter;
    req->result = 0;

    bq_tail = (bq_tail + 1) % BLOCK_QUEUE_SIZE;
    bq_count++;

    // Issue immediately if AHCI idle, else queue
    if (!ahci_current_req) {
        ahci_current_req = req;
        ahci_puts("ahci: submit cookie=");
        serial_put_hex(req->cookie);
        ahci_puts(" caller=");
        serial_put_hex(req->caller_pid);
        ahci_puts("\n");
        ahci_issue_cmd(req);

        // Short polling fallback: spin briefly to check if command completes
        // without an IRQ. This handles emulators/configs where the port
        // interrupt doesn't fire reliably (e.g., after PxIE toggle).
        bool completed = false;
        for (int i = 0; i < 500000; i++) {
            if (!(readl(port_reg(active_port, PxCI)) & 1)) {
                completed = true;
                break;
            }
            __asm__ volatile("pause");
        }

        if (completed) {
            // Command completed via polling — process completion directly
            uint32_t pxis = readl(port_reg(active_port, PxIS));
            writel(port_reg(active_port, PxIS), 0xFFFFFFFF);  // acknowledge
            bool error = (pxis & (1U << 30));

            // For reads: copy bounce buffer to user buffer
            if (dir == 0 && !error) {
                __memcpy(buf, (const void *)bounce_virt, (size_t)count * 512);
            }

            req->result = error ? EIO : 0;

            // Build notification message
            recv_msg msg;
            msg.type = RECV_NOTIFY;
            msg.src = 0;
            __memset(msg.data, 0, 56);
            __memcpy(msg.data, &req->cookie, 4);
            __memcpy(msg.data + 4, &req->result, 4);
            __memcpy(msg.data + 8, &req->lba, 4);
            __memcpy(msg.data + 12, &req->count, 4);

            ahci_current_req = nullptr;
            bq_count--;
            bq_head = (bq_head + 1) % BLOCK_QUEUE_SIZE;

            serial_printf("ahci: poll-complete cookie=%ld to pid=%d, bq_count=%ld\n",
                req->cookie, req->caller_pid, bq_count);

            // Notify caller (safe while holding ahci_lock)
            notify_and_wake(req->caller_pid, &msg);
        }
    }

    spin_unlock_irqrestore(&ahci_lock, flags);
    return (int)req->cookie;  // positive cookie = success
}

