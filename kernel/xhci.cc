#include "kernel/xhci.h"
#include "kernel/pci.h"
#include "kernel/serial.h"
#include "kernel/mem/alloc.h"
#include "kernel/trap.h"
#include "arch/x64/utils.h"
#include "arch/x64/apic.h"
#include "common/errno.h"

// ===================== xHCI register offsets =====================
// Capability registers (from MMIO base)
#define XHCI_CAPLENGTH   0x00
#define XHCI_HCIVERSION  0x02
#define XHCI_HCSPARAMS1  0x04
#define XHCI_HCSPARAMS2  0x08
#define XHCI_HCSPARAMS3  0x0C
#define XHCI_HCCPARAMS1  0x10
#define XHCI_DBOFF       0x14
#define XHCI_RTSOFF      0x18

// Operational registers (base + CAPLENGTH)
#define XHCI_USBCMD      0x00
#define XHCI_USBSTS      0x04
#define XHCI_PAGESIZE    0x08
#define XHCI_DNCTRL      0x14
#define XHCI_CRCR        0x18
#define XHCI_CRCR_LO     0x18
#define XHCI_CRCR_HI     0x1C
#define XHCI_DCBAAP      0x30
#define XHCI_DCBAAP_LO   0x30
#define XHCI_DCBAAP_HI   0x34
#define XHCI_CONFIG      0x38

// Port registers (base + 0x400 + port*0x10)
#define XHCI_PORTSC      0x00

// Runtime registers (base + RTSOFF)
#define XHCI_MFINDEX     0x00
// Interrupter registers: base + RTSOFF + 0x20 + intr*0x20
#define XHCI_IMAN        0x00
#define XHCI_IMOD        0x04
#define XHCI_ERSTSZ      0x08
#define XHCI_ERSTBA_LO   0x10
#define XHCI_ERSTBA_HI   0x14
#define XHCI_ERDP_LO     0x18
#define XHCI_ERDP_HI     0x1C

// ===================== xHCI bit definitions =====================
// USBCMD
#define USBCMD_RS        (1 << 0)
#define USBCMD_HCRST     (1 << 1)
#define USBCMD_INTE      (1 << 2)

// USBSTS
#define USBSTS_HCH       (1 << 0)
#define USBSTS_HSE       (1 << 2)
#define USBSTS_EINT      (1 << 3)
#define USBSTS_PCD       (1 << 4)
#define USBSTS_CNR       (1 << 11)

// IMAN
#define IMAN_IE          (1 << 1)
#define IMAN_IP          (1 << 0)

// CRCR flags
#define CRCR_RCS         (1 << 4)  // Ring Cycle State

// ERDP flags
#define ERDP_EHB         (1 << 3)  // Event Handler Busy

// PORTSC
#define PORTSC_CCS       (1 << 0)  // Current Connect Status
#define PORTSC_PED       (1 << 1)  // Port Enabled/Disabled
#define PORTSC_PR        (1 << 4)  // Port Reset

// ===================== TRB definitions =====================
// TRB types (bits 10:15 of dword 3)
#define TRB_TYPE_SHIFT   10
#define TRB_TYPE_MASK    0x3F

#define TRB_ENABLE_SLOT  9
#define TRB_LINK         6
#define TRB_CMD_COMPLETE 33
#define TRB_PORT_STATUS  34

// TRB completion codes
#define CC_SUCCESS       1
#define CC_TRB_ERROR     2

// TRB flags
#define TRB_TC           (1 << 1)   // Toggle Cycle (Link TRB)
#define TRB_IOC          (1 << 5)   // Interrupt on Completion

// ===================== xHCI driver state =====================
static uint64_t mmio_base;        // MMIO base virtual address
static uint64_t op_base;          // Operational register base
static uint64_t rt_base;          // Runtime register base
static uint64_t db_base;          // Doorbell register base
static struct pci_device *xhci_dev;

// DMA memory pages
static Page *dcbaa_page;
static Page *input_ctx_page;
static Page *cmd_ring_page;
static Page *erst_page;
static Page *event_ring_page;
static Page *scratchpad_page;

static uint64_t dcbaa_phys;
static uint64_t input_ctx_phys;
static uint64_t cmd_ring_phys;
static uint64_t erst_phys;
static uint64_t event_ring_phys;
static uint64_t scratchpad_phys;

static uint64_t dcbaa_virt;
static uint64_t cmd_ring_virt;
static uint64_t erst_virt;
static uint64_t event_ring_virt;

// Ring state
static int cmd_ring_enqueue = 0;
static int cmd_ring_ccs = 1;       // Command Ring Cycle State
static int event_ring_dequeue = 0;
static int event_ring_ccs = 1;     // Event Ring Consumer Cycle State

// HC parameters
static int max_slots;
static int max_intrs;
static int max_ports;

// ===================== Helper functions =====================

static inline uint32_t xhci_read(uint64_t offset) {
  return readl((void *)(mmio_base + offset));
}

static inline void xhci_write(uint64_t offset, uint32_t val) {
  writel((void *)(mmio_base + offset), val);
}

static inline uint32_t op_read(uint64_t offset) {
  return readl((void *)(op_base + offset));
}

static inline void op_write(uint64_t offset, uint32_t val) {
  writel((void *)(op_base + offset), val);
}

static inline uint32_t rt_read(uint64_t offset) {
  return readl((void *)(rt_base + offset));
}

static inline void rt_write(uint64_t offset, uint32_t val) {
  writel((void *)(rt_base + offset), val);
}

static inline void db_write(uint32_t slot, uint32_t target) {
  writel((void *)(db_base + slot * 4), target);
}

static uint64_t rt_intr_base(int intr) {
  return rt_base + 0x20 + intr * 0x20;
}

static inline uint32_t intr_read(int intr, uint64_t offset) {
  return readl((void *)(rt_intr_base(intr) + offset));
}

static inline void intr_write(int intr, uint64_t offset, uint32_t val) {
  writel((void *)(rt_intr_base(intr) + offset), val);
}

// ===================== TRB helpers =====================

struct trb {
  uint32_t dword0;
  uint32_t dword1;
  uint32_t dword2;
  uint32_t dword3;
};

static void cmd_ring_push(struct trb *t) {
  volatile uint32_t *ring = (volatile uint32_t *)cmd_ring_virt;
  int idx = cmd_ring_enqueue * 4;
  // Set cycle bit in dword3 (bit 0)
  t->dword3 &= ~1;
  t->dword3 |= (cmd_ring_ccs & 1);

  ring[idx + 0] = t->dword0;
  ring[idx + 1] = t->dword1;
  ring[idx + 2] = t->dword2;
  ring[idx + 3] = t->dword3;
  cmd_ring_enqueue++;

  // If we hit the Link TRB at index 255, flip CCS and wrap
  if (cmd_ring_enqueue == 255) {
    cmd_ring_enqueue = 0;
    cmd_ring_ccs ^= 1;
  }
}

static int poll_cmd_complete(uint32_t *completion_code, uint32_t *slot_id) {
  volatile uint32_t *ring = (volatile uint32_t *)event_ring_virt;

  for (int timeout = 0; timeout < 1000000; timeout++) {
    // Check event ring for Command Completion Event
    int idx = event_ring_dequeue * 4;
    uint32_t d3 = ring[idx + 3];
    uint32_t cycle = d3 & 1;
    if (cycle != (event_ring_ccs & 1)) {
      // No new event
      __asm__ volatile("pause");
      continue;
    }

    uint32_t d0 = ring[idx + 0];
    uint32_t d1 = ring[idx + 1];
    uint32_t d2 = ring[idx + 2];

    int type = (d3 >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;

    if (type == TRB_CMD_COMPLETE) {
      *completion_code = (d2 >> 24) & 0xFF;
      *slot_id = (d3 >> 24) & 0xFF;
      event_ring_dequeue++;
      if (event_ring_dequeue >= 255) {
        event_ring_dequeue = 0;
        event_ring_ccs ^= 1;
      }
      // Update ERDP
      uint64_t erdp = event_ring_phys + event_ring_dequeue * 16;
      erdp |= ERDP_EHB;
      intr_write(0, XHCI_ERDP_LO, (uint32_t)erdp);
      intr_write(0, XHCI_ERDP_HI, (uint32_t)(erdp >> 32));
      return 0;
    }

    // Skip other event types
    event_ring_dequeue++;
    if (event_ring_dequeue >= 255) {
      event_ring_dequeue = 0;
      event_ring_ccs ^= 1;
    }
    // Update ERDP
    uint64_t erdp = event_ring_phys + event_ring_dequeue * 16;
    erdp |= ERDP_EHB;
    intr_write(0, XHCI_ERDP_LO, (uint32_t)erdp);
    intr_write(0, XHCI_ERDP_HI, (uint32_t)(erdp >> 32));
  }

  return -1; // timeout
}

// ===================== ISR =====================

static void xhci_isr(trapframe_t *tf) {
  serial_puts("[xHCI] MSI-X interrupt received on vector ");
  serial_put_hex((uint64_t)tf->trapno);
  serial_puts("\n");

  // Read IMAN to confirm IP
  uint32_t iman = intr_read(0, XHCI_IMAN);
  if (iman & IMAN_IP) {
    // Clear IP + ensure IE
    intr_write(0, XHCI_IMAN, iman | IMAN_IP | IMAN_IE);
  }

  // Walk event ring
  volatile uint32_t *ring = (volatile uint32_t *)event_ring_virt;
  while (1) {
    int idx = event_ring_dequeue * 4;
    uint32_t d3 = ring[idx + 3];
    uint32_t cycle = d3 & 1;
    if (cycle != (event_ring_ccs & 1)) break;

    int type = (d3 >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;
    uint32_t d2 = ring[idx + 2];
    if (type == TRB_CMD_COMPLETE) {
      uint32_t cc = (d2 >> 24) & 0xFF;
      uint32_t sid = (d3 >> 24) & 0xFF;
      serial_puts("[xHCI] CMD_COMPLETE: cc=");
      serial_put_hex(cc);
      serial_puts(" slot=");
      serial_put_hex(sid);
      serial_puts("\n");
    }

    event_ring_dequeue++;
    if (event_ring_dequeue >= 255) {
      event_ring_dequeue = 0;
      event_ring_ccs ^= 1;
    }
  }

  // Update ERDP
  uint64_t erdp = event_ring_phys + event_ring_dequeue * 16;
  erdp |= ERDP_EHB;
  intr_write(0, XHCI_ERDP_LO, (uint32_t)erdp);
  intr_write(0, XHCI_ERDP_HI, (uint32_t)(erdp >> 32));

  lapic_eoi();
}

// ===================== xHCI init =====================

void xhci_init() {
  serial_puts("[xHCI] init start\n");

  // 1. Find xHCI PCI device (class=0x0C03, prog_if=0x30)
  xhci_dev = nullptr;
  for (int i = 0; i < pci_device_count; i++) {
    if (pci_devices[i].class_code == PCI_CLASS_SERIAL_USB) {
      // Check prog_if = 0x30 (xHCI)
      uint32_t rev_class = pci_read_config(pci_devices[i].bus, pci_devices[i].dev,
                                            pci_devices[i].func, 0x08);
      uint8_t prog_if = (rev_class >> 8) & 0xFF;
      if (prog_if == 0x30) {
        xhci_dev = &pci_devices[i];
        break;
      }
    }
  }

  if (!xhci_dev) {
    serial_puts("[xHCI] No xHCI controller found, skip\n");
    return;
  }

  serial_puts("[xHCI] Found PCI device: bus=");
  serial_put_hex(xhci_dev->bus);
  serial_puts(" dev=");
  serial_put_hex(xhci_dev->dev);
  serial_puts(" func=");
  serial_put_hex(xhci_dev->func);
  serial_puts("\n");

  // 2. Enable PCI device (map BAR0 MMIO + Bus Master + Memory Space)
  if (pci_enable_device(xhci_dev) != 0) {
    serial_puts("[xHCI] pci_enable_device failed\n");
    return;
  }

  mmio_base = xhci_dev->bar[0].vaddr;
  if (mmio_base == 0) {
    serial_puts("[xHCI] BAR0 not mapped\n");
    return;
  }

  // 3. Check MSI-X capability
  if (xhci_dev->msix_cap_offset == 0) {
    serial_puts("[xHCI] No MSI-X capability, skip\n");
    return;
  }

  serial_puts("[xHCI] MSI-X: cap_offset=0x");
  serial_put_hex(xhci_dev->msix_cap_offset);
  serial_puts(" table_bar=");
  serial_put_hex(xhci_dev->msix_table_bar);
  serial_puts(" table_offset=0x");
  serial_put_hex(xhci_dev->msix_table_offset);
  serial_puts("\n");

  // Read capability registers
  uint8_t cap_length = xhci_read(XHCI_CAPLENGTH) & 0xFF;
  op_base = mmio_base + cap_length;
  uint32_t rtsoff = xhci_read(XHCI_RTSOFF);
  rt_base = mmio_base + (rtsoff & ~0x1F);  // align to 32 bytes
  uint32_t dboff = xhci_read(XHCI_DBOFF);
  db_base = mmio_base + (dboff & ~0x3);     // align to 4 bytes

  serial_puts("[xHCI] cap_len=");
  serial_put_hex(cap_length);
  serial_puts(" rtsoff=0x");
  serial_put_hex(rtsoff);
  serial_puts(" dboff=0x");
  serial_put_hex(dboff);
  serial_puts("\n");

  // 4. Allocate DMA memory (6 pages)
  dcbaa_page = bfc_alloc.alloc_page_low(1);
  input_ctx_page = bfc_alloc.alloc_page_low(1);
  cmd_ring_page = bfc_alloc.alloc_page_low(1);
  erst_page = bfc_alloc.alloc_page_low(1);
  event_ring_page = bfc_alloc.alloc_page_low(1);
  scratchpad_page = bfc_alloc.alloc_page_low(1);

  if (!dcbaa_page || !input_ctx_page || !cmd_ring_page ||
      !erst_page || !event_ring_page || !scratchpad_page) {
    serial_puts("[xHCI] DMA alloc failed\n");
    return;
  }

  dcbaa_phys = page_to_phys(dcbaa_page);
  input_ctx_phys = page_to_phys(input_ctx_page);
  cmd_ring_phys = page_to_phys(cmd_ring_page);
  erst_phys = page_to_phys(erst_page);
  event_ring_phys = page_to_phys(event_ring_page);
  scratchpad_phys = page_to_phys(scratchpad_page);

  dcbaa_virt = phys_to_virt(dcbaa_phys);
  cmd_ring_virt = phys_to_virt(cmd_ring_phys);
  erst_virt = phys_to_virt(erst_phys);
  event_ring_virt = phys_to_virt(event_ring_phys);

  // Zero all DMA buffers
  __memset((void *)dcbaa_virt, 0, 4096);
  __memset((void *)phys_to_virt(input_ctx_phys), 0, 4096);
  __memset((void *)cmd_ring_virt, 0, 4096);
  __memset((void *)erst_virt, 0, 4096);
  __memset((void *)event_ring_virt, 0, 4096);
  __memset((void *)phys_to_virt(scratchpad_phys), 0, 4096);

  // 5. Read HCSPARAMS1
  uint32_t hcsparams1 = xhci_read(XHCI_HCSPARAMS1);
  max_slots = hcsparams1 & 0xFF;
  max_intrs = (hcsparams1 >> 8) & 0x7FF;
  max_ports = (hcsparams1 >> 24) & 0xFF;

  serial_puts("[xHCI] MaxSlots=");
  serial_put_hex(max_slots);
  serial_puts(" MaxIntrs=");
  serial_put_hex(max_intrs);
  serial_puts(" MaxPorts=");
  serial_put_hex(max_ports);
  serial_puts("\n");

  // 6. Configure MSI-X
  int nvectors = pci_enable_msix(xhci_dev, max_intrs);
  if (nvectors <= 0) {
    serial_puts("[xHCI] MSI-X enable failed\n");
    return;
  }

  serial_puts("[xHCI] MSI-X enabled, vectors ");
  serial_put_hex(xhci_dev->msix_vector_base);
  serial_puts("-");
  serial_put_hex(xhci_dev->msix_vector_base + nvectors - 1);
  serial_puts("\n");

  // Register ISR for Entry 0 vector
  int vector0 = xhci_dev->msix_vector_base;
  register_irq(vector0, xhci_isr);

  // 7. xHC reset: USBCMD.HCRST -> poll HCRST=0 (1s timeout)
  // First ensure controller is halted
  uint32_t usbsts = op_read(XHCI_USBSTS);
  if (!(usbsts & USBSTS_HCH)) {
    // Stop controller
    op_write(XHCI_USBCMD, op_read(XHCI_USBCMD) & ~USBCMD_RS);
    // Poll for HCH=1
    for (int i = 0; i < 1000000; i++) {
      if (op_read(XHCI_USBSTS) & USBSTS_HCH) break;
      __asm__ volatile("pause");
    }
  }

  // Reset
  op_write(XHCI_USBCMD, USBCMD_HCRST);
  for (int i = 0; i < 1000000; i++) {
    if (!(op_read(XHCI_USBCMD) & USBCMD_HCRST) && !(op_read(XHCI_USBSTS) & USBSTS_CNR))
      break;
    __asm__ volatile("pause");
  }

  if (op_read(XHCI_USBCMD) & USBCMD_HCRST) {
    serial_puts("[xHCI] Controller reset timeout\n");
    serial_puts("  USBCMD=0x");
    serial_put_hex(op_read(XHCI_USBCMD));
    serial_puts(" USBSTS=0x");
    serial_put_hex(op_read(XHCI_USBSTS));
    serial_puts("\n");
    return;
  }
  serial_puts("[xHCI] Controller reset done\n");

  // 8. Program DCBAA base address
  op_write(XHCI_DCBAAP_LO, (uint32_t)dcbaa_phys);
  op_write(XHCI_DCBAAP_HI, (uint32_t)(dcbaa_phys >> 32));

  // 11. Scratchpad Buffer: DCBAA[0] = scratchpad page phys
  uint64_t *dcbaa = (uint64_t *)dcbaa_virt;
  dcbaa[0] = scratchpad_phys;

  // 9. Command Ring: index 255 = Link TRB (TC=1), write CRCR
  volatile uint32_t *cmd_ring = (volatile uint32_t *)cmd_ring_virt;
  int link_idx = 255 * 4;
  cmd_ring[link_idx + 0] = (uint32_t)cmd_ring_phys;
  cmd_ring[link_idx + 1] = (uint32_t)(cmd_ring_phys >> 32);
  cmd_ring[link_idx + 2] = 0;
  cmd_ring[link_idx + 3] = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_TC;

  // Write CRCR (PCS=1)
  uint64_t crcr = cmd_ring_phys | CRCR_RCS;
  op_write(XHCI_CRCR_LO, (uint32_t)crcr);
  op_write(XHCI_CRCR_HI, (uint32_t)(crcr >> 32));

  // 10. Event Ring: ERST[0] = {phys, 256}, write ERSTSZ/ERSTBA/ERDP
  struct erst_entry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t size;
    uint32_t reserved;
  };

  volatile struct erst_entry *erst = (volatile struct erst_entry *)erst_virt;
  erst[0].addr_lo = (uint32_t)event_ring_phys;
  erst[0].addr_hi = (uint32_t)(event_ring_phys >> 32);
  erst[0].size = 256;  // TRBs in this segment (index 255 is Link TRB)

  // Link TRB at end of event ring segment
  volatile uint32_t *evt_ring = (volatile uint32_t *)event_ring_virt;
  int evt_link_idx = 255 * 4;
  evt_ring[evt_link_idx + 0] = (uint32_t)event_ring_phys;
  evt_ring[evt_link_idx + 1] = (uint32_t)(event_ring_phys >> 32);
  evt_ring[evt_link_idx + 2] = 0;
  evt_ring[evt_link_idx + 3] = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_TC | 1; // TC=1, cycle=1

  // Write ERSTSZ, ERSTBA, ERDP for interrupter 0
  intr_write(0, XHCI_ERSTSZ, 1);   // 1 segment
  uint64_t erstba = erst_phys;
  intr_write(0, XHCI_ERSTBA_LO, (uint32_t)erstba);
  intr_write(0, XHCI_ERSTBA_HI, (uint32_t)(erstba >> 32));
  uint64_t erdp = event_ring_phys | ERDP_EHB;
  intr_write(0, XHCI_ERDP_LO, (uint32_t)erdp);
  intr_write(0, XHCI_ERDP_HI, (uint32_t)(erdp >> 32));

  // 12. CONFIG = MaxSlots
  op_write(XHCI_CONFIG, max_slots);

  // 13. USBCMD.RS=1 -> poll HCH=0 (1s timeout)
  op_write(XHCI_USBCMD, op_read(XHCI_USBCMD) | USBCMD_RS);
  for (int i = 0; i < 1000000; i++) {
    if (!(op_read(XHCI_USBSTS) & USBSTS_HCH)) break;
    __asm__ volatile("pause");
  }

  if (op_read(XHCI_USBSTS) & USBSTS_HCH) {
    serial_puts("[xHCI] Controller start timeout\n");
    serial_puts("  USBCMD=0x");
    serial_put_hex(op_read(XHCI_USBCMD));
    serial_puts(" USBSTS=0x");
    serial_put_hex(op_read(XHCI_USBSTS));
    serial_puts("\n");
    return;
  }
  serial_puts("[xHCI] Controller running (HCH=0)\n");

  // 14. Enable Slot command (polling mode, MSI-X Entry mask)
  volatile uint32_t *ring = (volatile uint32_t *)cmd_ring_virt;
  int idx = cmd_ring_enqueue * 4;
  ring[idx + 0] = 0;
  ring[idx + 1] = 0;
  ring[idx + 2] = 0;
  ring[idx + 3] = (TRB_ENABLE_SLOT << TRB_TYPE_SHIFT) | (cmd_ring_ccs & 1);
  cmd_ring_enqueue++;
  if (cmd_ring_enqueue >= 255) cmd_ring_enqueue = 0;

  db_write(0, 0);  // Ring doorbell slot 0, target 0

  uint32_t completion_code = 0, slot_id = 0;
  int result = poll_cmd_complete(&completion_code, &slot_id);
  if (result < 0) {
    serial_puts("[xHCI] Enable Slot timeout\n");
    serial_puts("  CRCR_LO=0x");
    serial_put_hex(op_read(XHCI_CRCR_LO));
    serial_puts(" CRCR_HI=0x");
    serial_put_hex(op_read(XHCI_CRCR_HI));
    serial_puts("\n");
    return;
  }

  serial_puts("[xHCI] Enable Slot: completion_code=");
  serial_put_hex(completion_code);
  serial_puts(", slot_id=");
  serial_put_hex(slot_id);
  serial_puts("\n");

  // 15. pci_msix_unmask_entry(dev, 0) — unmask Entry 0
  pci_msix_unmask_entry(xhci_dev, 0);

  // 16. USBCMD.INTE=1, IMAN.IE=1
  op_write(XHCI_USBCMD, op_read(XHCI_USBCMD) | USBCMD_INTE);
  intr_write(0, XHCI_IMAN, intr_read(0, XHCI_IMAN) | IMAN_IE);

  serial_puts("[xHCI] init complete (INTE+IE enabled)\n");
}
