/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/xhci.h"
#include "arch/x64/apic.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/driver/pci.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include <stdbool.h>
#include <stdint.h>
#include <xos/shm.h>

// ===================== xHCI register offsets =====================
// Capability registers (from MMIO base)
#define XHCI_CAPLENGTH 0x00
#define XHCI_HCIVERSION 0x02
#define XHCI_HCSPARAMS1 0x04
#define XHCI_HCSPARAMS2 0x08
#define XHCI_HCSPARAMS3 0x0C
#define XHCI_HCCPARAMS1 0x10
#define XHCI_DBOFF 0x14
#define XHCI_RTSOFF 0x18

// Operational registers (base + CAPLENGTH)
#define XHCI_USBCMD 0x00
#define XHCI_USBSTS 0x04
#define XHCI_PAGESIZE 0x08
#define XHCI_DNCTRL 0x14
#define XHCI_CRCR 0x18
#define XHCI_CRCR_LO 0x18
#define XHCI_CRCR_HI 0x1C
#define XHCI_DCBAAP 0x30
#define XHCI_DCBAAP_LO 0x30
#define XHCI_DCBAAP_HI 0x34
#define XHCI_CONFIG 0x38

// Port registers (op_base + 0x400 + port*0x10)
#define XHCI_PORTSC_BASE 0x400
#define XHCI_PORTSC 0x00

// Runtime registers (base + RTSOFF)
#define XHCI_MFINDEX 0x00
// Interrupter registers: base + RTSOFF + 0x20 + intr*0x20
#define XHCI_IMAN 0x00
#define XHCI_IMOD 0x04
#define XHCI_ERSTSZ 0x08
#define XHCI_ERSTBA_LO 0x10
#define XHCI_ERSTBA_HI 0x14
#define XHCI_ERDP_LO 0x18
#define XHCI_ERDP_HI 0x1C

// ===================== xHCI bit definitions =====================
// USBCMD
#define USBCMD_RS (1 << 0)
#define USBCMD_HCRST (1 << 1)
#define USBCMD_INTE (1 << 2)

// USBSTS
#define USBSTS_HCH (1 << 0)
#define USBSTS_HSE (1 << 2)
#define USBSTS_EINT (1 << 3)
#define USBSTS_PCD (1 << 4)
#define USBSTS_CNR (1 << 11)

// IMAN
#define IMAN_IE (1 << 1)
#define IMAN_IP (1 << 0)

// CRCR flags
#define CRCR_RCS (1 << 4)

// ERDP flags
#define ERDP_EHB (1 << 3)

// PORTSC
#define PORTSC_CCS (1 << 0)           // Current Connect Status
#define PORTSC_PED (1 << 1)           // Port Enabled/Disabled
#define PORTSC_PR (1 << 4)            // Port Reset
#define PORTSC_PLS_MASK 0xF           // Port Link State (bits 5-8)
#define PORTSC_SPEED_MASK (0xF << 10) // Port Speed (bits 10-13)
#define PORTSC_SPEED_FS (1 << 10)     // Full Speed
#define PORTSC_SPEED_LS (2 << 10)     // Low Speed
#define PORTSC_SPEED_HS (3 << 10)     // High Speed

// ===================== TRB definitions =====================
// TRB types (bits 10:15 of dword 3)
#define TRB_TYPE_SHIFT 10
#define TRB_TYPE_MASK 0x3F

#define TRB_NORMAL 1
#define TRB_SETUP_STAGE 2
#define TRB_DATA_STAGE 3
#define TRB_STATUS_STAGE 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEV 11
#define TRB_CONFIG_EP 12
#define TRB_EVAL_CTX 13
#define TRB_CMD_COMPLETE 33
#define TRB_PORT_STATUS 34
#define TRB_TRANSFER 32

// TRB completion codes
#define CC_SUCCESS 1
#define CC_TRB_ERROR 2

// TRB flags
#define TRB_TC (1 << 1)      // Toggle Cycle (Link TRB)
#define TRB_IOC (1 << 5)     // Interrupt on Completion
#define TRB_IDT (1 << 6)     // Immediate Data (Setup Stage)
#define TRB_CH (1 << 9)      // Chain bit
#define TRB_DIR_IN (1 << 16) // Direction IN (Data/Status stage)

// Slot/EP state values
#define SLOT_STATE_DISABLED 0
#define SLOT_STATE_ENABLED 1
#define SLOT_STATE_DEFAULT 2
#define SLOT_STATE_CONFIGURED 3

#define EP_STATE_DISABLED 0
#define EP_STATE_RUNNING 1

// EP type values (3-bit field in EP Context dword1 bits [3:5])
// Matches xHCI spec Table 6-7 and QEMU's EPType enum
#define EP_TYPE_ISOCH_OUT 1
#define EP_TYPE_BULK_OUT 2
#define EP_TYPE_INTERRUPT_OUT 3
#define EP_TYPE_CONTROL 4 // Control (bidirectional, uses OUT DCI)
#define EP_TYPE_ISOCH_IN 5
#define EP_TYPE_BULK_IN 6
#define EP_TYPE_INTERRUPT_IN 7

// ===================== xHCI driver state =====================
static void __iomem *mmio_base;
static void __iomem *op_base;
static void __iomem *rt_base;
static void __iomem *db_base;
static pci_device *xhci_dev;

// DMA memory pages (base controller)
static struct page *dcbaa_page;
static struct page *input_ctx_page;
static struct page *cmd_ring_page;
static struct page *erst_page;
static struct page *event_ring_page;
static struct page *scratchpad_page;

static uint64_t dcbaa_phys;
static uint64_t input_ctx_phys;
static uint64_t cmd_ring_phys;
static uint64_t erst_phys;
static uint64_t event_ring_phys;
static uint64_t scratchpad_phys;

static void *dcbaa_virt;
static void *cmd_ring_virt;
static void *erst_virt;
static void *event_ring_virt;

// Ring state
static int cmd_ring_enqueue = 0;
static int cmd_ring_ccs = 1;
static int event_ring_dequeue = 0;
static int event_ring_ccs = 1;

// HC parameters
static int max_slots;
static int max_intrs;
static int max_ports;

// ===================== USB HID / keyboard state =====================
typedef struct xhci_intr {
  struct page *ring_page;
  uint64_t ring_phys;
  void *ring_virt;
  int enqueue;
  int ccs;
  int slot_id;
  int ep_dci; // Doorbell target (DCI): EP1-IN = 3
  int ep_num; // Endpoint number for event matching: EP1-IN = 1
} xhci_intr;

static xhci_intr xhci_intrs[2]; // intr 0=keyboard, 1=spare

// USB HID SHM page (kernel-allocated, shared with kbd_driver)
static struct page *usb_hid_shm_page;
static uint64_t usb_hid_shm_phys;
static void *usb_hid_shm_virt;

// kbd opener tracking: xhci module self-managed, replaces the old
// global type-keyed driver lookup used by the ISR to wake the kbd driver
static pid_t kbd_openers[8];

// HID DMA buffer (xHCI writes HID report here, <4GB)
static struct page *hid_dma_page;
static uint64_t hid_dma_phys;
static void *hid_dma_virt;

// EP0 Transfer Ring (for control transfers: Get Descriptor, Set Protocol)
static struct page *ep0_ring_page;
static uint64_t ep0_ring_phys;
static void *ep0_ring_virt;
static int ep0_ring_enqueue = 0;
static int ep0_ring_ccs = 1;

// Output Device Context (xHCI writes device state here)
static struct page *dev_ctx_page;
static uint64_t dev_ctx_phys;

// Control transfer DMA buffer (for Get Descriptor data stage)
static struct page *ctrl_dma_page;
static uint64_t ctrl_dma_phys;
static void *ctrl_dma_virt;

// ===================== Helper functions =====================

static inline uint32_t xhci_read(uint64_t offset) {
  return readl((void __iomem *)((uint8_t __iomem *)mmio_base + offset));
}

static inline void xhci_write(uint64_t offset, uint32_t val) {
  writel((void __iomem *)((uint8_t __iomem *)mmio_base + offset), val);
}

static inline uint32_t op_read(uint64_t offset) {
  return readl((void __iomem *)((uint8_t __iomem *)op_base + offset));
}

static inline void op_write(uint64_t offset, uint32_t val) {
  writel((void __iomem *)((uint8_t __iomem *)op_base + offset), val);
}

static inline uint32_t rt_read(uint64_t offset) {
  return readl((void __iomem *)((uint8_t __iomem *)rt_base + offset));
}

static inline void rt_write(uint64_t offset, uint32_t val) {
  writel((void __iomem *)((uint8_t __iomem *)rt_base + offset), val);
}

static inline void db_write(uint32_t slot, uint32_t target) {
  writel((void __iomem *)((uint8_t __iomem *)db_base + slot * 4), target);
}

static void __iomem *rt_intr_base(int intr) {
  return (void __iomem *)((uint8_t __iomem *)rt_base + 0x20 + intr * 0x20);
}

static inline uint32_t intr_read(int intr, uint64_t offset) {
  return readl(
      (void __iomem *)((uint8_t __iomem *)rt_intr_base(intr) + offset));
}

static inline void intr_write(int intr, uint64_t offset, uint32_t val) {
  writel((void __iomem *)((uint8_t __iomem *)rt_intr_base(intr) + offset), val);
}

static inline uint32_t portsc_read(int port) {
  return op_read(XHCI_PORTSC_BASE + port * 0x10 + XHCI_PORTSC);
}

static inline void portsc_write(int port, uint32_t val) {
  op_write(XHCI_PORTSC_BASE + port * 0x10 + XHCI_PORTSC, val);
}

// ===================== TRB helpers =====================

typedef struct trb {
  uint32_t dword0;
  uint32_t dword1;
  uint32_t dword2;
  uint32_t dword3;
} trb;

static void cmd_ring_push(trb *t) {
  volatile uint32_t *ring = (volatile uint32_t *)cmd_ring_virt;
  int idx = cmd_ring_enqueue * 4;
  t->dword3 &= ~1;
  t->dword3 |= (cmd_ring_ccs & 1);

  ring[idx + 0] = t->dword0;
  ring[idx + 1] = t->dword1;
  ring[idx + 2] = t->dword2;
  ring[idx + 3] = t->dword3;
  cmd_ring_enqueue++;

  if (cmd_ring_enqueue == 255) {
    cmd_ring_enqueue = 0;
    cmd_ring_ccs ^= 1;
  }
}

// Enqueue a TRB on a transfer ring (EP0 or EP1-IN)
static void xfer_ring_enqueue(volatile uint32_t *ring_virt, int *enqueue,
                              int *ccs, trb *t) {
  int idx = (*enqueue) * 4;
  t->dword3 &= ~1;
  t->dword3 |= (*ccs & 1);

  ring_virt[idx + 0] = t->dword0;
  ring_virt[idx + 1] = t->dword1;
  ring_virt[idx + 2] = t->dword2;
  ring_virt[idx + 3] = t->dword3;
  (*enqueue)++;

  if (*enqueue == 255) {
    *enqueue = 0;
    *ccs ^= 1;
  }
}

// Poll event ring for any completion/event (not just CMD_COMPLETE)
// Returns 0 on success, -1 on timeout. Fills in event TRB fields.
static int poll_event(uint32_t *completion_code, uint32_t *slot_id,
                      uint32_t *trb_type) {
  volatile uint32_t *ring = (volatile uint32_t *)event_ring_virt;

  for (int timeout = 0; timeout < 2000000; timeout++) {
    int idx = event_ring_dequeue * 4;
    uint32_t d3 = ring[idx + 3];
    uint32_t cycle = d3 & 1;
    if (cycle != (event_ring_ccs & 1)) {
      __asm__ volatile("pause");
      continue;
    }

    uint32_t d2 = ring[idx + 2];
    *trb_type = (d3 >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;
    *completion_code = (d2 >> 24) & 0xFF;
    *slot_id = (d3 >> 24) & 0xFF;

    event_ring_dequeue++;
    if (event_ring_dequeue >= 255) {
      event_ring_dequeue = 0;
      event_ring_ccs ^= 1;
    }

    uint64_t erdp = event_ring_phys + event_ring_dequeue * 16;
    erdp |= ERDP_EHB;
    intr_write(0, XHCI_ERDP_LO, (uint32_t)erdp);
    intr_write(0, XHCI_ERDP_HI, (uint32_t)(erdp >> 32));
    return 0;
  }

  return -1;
}

// ===================== ISR =====================

static void xhci_isr(trapframe *tf) {
  // Read IMAN to confirm IP
  uint32_t iman = intr_read(0, XHCI_IMAN);
  if (iman & IMAN_IP) {
    intr_write(0, XHCI_IMAN, iman | IMAN_IP | IMAN_IE);
  }

  // Walk event ring
  volatile uint32_t *ring = (volatile uint32_t *)event_ring_virt;
  while (1) {
    int idx = event_ring_dequeue * 4;
    uint32_t d3 = ring[idx + 3];
    uint32_t cycle = d3 & 1;
    if (cycle != (event_ring_ccs & 1))
      break;

    int type = (d3 >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;
    uint32_t d2 = ring[idx + 2];

    if (type == TRB_TRANSFER) {
      uint32_t cc = (d2 >> 24) & 0xFF;
      uint32_t sid = (d3 >> 24) & 0xFF;
      uint32_t epid = (d3 >> 16) & 0xFF;

      if (sid == (uint32_t)xhci_intrs[0].slot_id &&
          epid == (uint32_t)xhci_intrs[0].ep_num) {
        // Replenish TRB regardless of completion code to keep ring alive
        volatile uint32_t *xfer_ring =
            (volatile uint32_t *)xhci_intrs[0].ring_virt;
        trb norm;
        norm.dword0 = (uint32_t)hid_dma_phys;
        norm.dword1 = (uint32_t)(hid_dma_phys >> 32);
        norm.dword2 = 8;
        norm.dword3 = (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC;
        xfer_ring_enqueue(xfer_ring, &xhci_intrs[0].enqueue, &xhci_intrs[0].ccs,
                          &norm);
        db_write(xhci_intrs[0].slot_id, xhci_intrs[0].ep_dci);

        if (cc == CC_SUCCESS) {
          // Read 8-byte HID report from DMA buffer
          volatile uint8_t *report = (volatile uint8_t *)hid_dma_virt;

          // Write to USB HID SHM keyboard sub-ring
          volatile struct usb_hid_shm_header *hdr =
              (volatile struct usb_hid_shm_header *)usb_hid_shm_virt;
          uint32_t head =
              __atomic_load_n(&hdr->rings[0].head, __ATOMIC_ACQUIRE);
          uint32_t tail =
              __atomic_load_n(&hdr->rings[0].tail, __ATOMIC_ACQUIRE);
          uint32_t next = (head + 1) % HID_SUBRING_CAPACITY;

          if (next != tail) { // ring not full
            volatile struct usb_hid_slot *slot =
                (volatile struct usb_hid_slot *)((uint8_t *)usb_hid_shm_virt +
                                                 HID_SUBRING_KBD_OFFSET +
                                                 head * HID_SLOT_SIZE);
            slot->type = HID_TYPE_KEYBOARD;
            slot->len = 8;
            for (int i = 0; i < 8; i++)
              slot->data[i] = report[i];
            __atomic_store_n(&hdr->rings[0].head, next, __ATOMIC_RELEASE);
          }

          // Notify kbd openers: wake so sys_recv returns -EINTR
          for (int i = 0; i < 8; i++) {
            pid_t p = kbd_openers[i];
            if (p > 0)
              wake_process(p);
          }
        }
      }
    } else if (type == TRB_CMD_COMPLETE) {
      // No action needed at runtime
    } else if (type == TRB_PORT_STATUS) {
      // No action needed at runtime
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

// Called periodically from timer_handler to ring EP1-IN doorbell.
// This ensures QEMU's xHCI emulation retries NAK'ed interrupt transfers
// (QEMU only retries on doorbell write, not on timer expiry after NAK).
static int xhci_poll_initialized = 0;

void xhci_poll() {
  if (!xhci_poll_initialized)
    return;
  db_write(xhci_intrs[0].slot_id, xhci_intrs[0].ep_dci);
}

// ===================== xHCI init =====================

static void xhci_init_keyboard(); // forward declaration

void xhci_init() {
  // 1. Find xHCI PCI device
  xhci_dev = NULL;
  for (int i = 0; i < pci_device_count; i++) {
    if (pci_devices[i].class_code == PCI_CLASS_SERIAL_USB) {
      uint32_t rev_class = pci_read_config(
          pci_devices[i].bus, pci_devices[i].dev, pci_devices[i].func, 0x08);
      uint8_t prog_if = (rev_class >> 8) & 0xFF;
      if (prog_if == 0x30) {
        xhci_dev = &pci_devices[i];
        break;
      }
    }
  }

  if (!xhci_dev)
    return;

  // 2. Enable PCI device
  if (pci_enable_device(xhci_dev) != 0)
    return;

  mmio_base = xhci_dev->bar[0].vaddr;
  if (!mmio_base)
    return;

  // 3. Check MSI-X
  if (xhci_dev->msix_cap_offset == 0)
    return;

  // Read capability registers
  uint8_t cap_length = xhci_read(XHCI_CAPLENGTH) & 0xFF;
  op_base = (void __iomem *)((uint8_t __iomem *)mmio_base + cap_length);
  uint32_t rtsoff = xhci_read(XHCI_RTSOFF);
  rt_base = (void __iomem *)((uint8_t __iomem *)mmio_base + (rtsoff & ~0x1F));
  uint32_t dboff = xhci_read(XHCI_DBOFF);
  db_base = (void __iomem *)((uint8_t __iomem *)mmio_base + (dboff & ~0x3));

  // 4. Allocate DMA memory (6 base pages)
  dcbaa_page = bfc_alloc_page_low(1);
  input_ctx_page = bfc_alloc_page_low(1);
  cmd_ring_page = bfc_alloc_page_low(1);
  erst_page = bfc_alloc_page_low(1);
  event_ring_page = bfc_alloc_page_low(1);
  scratchpad_page = bfc_alloc_page_low(1);

  if (!dcbaa_page || !input_ctx_page || !cmd_ring_page || !erst_page ||
      !event_ring_page || !scratchpad_page)
    return;

  dcbaa_phys = (__force uint64_t)page_to_phys(dcbaa_page);
  input_ctx_phys = (__force uint64_t)page_to_phys(input_ctx_page);
  cmd_ring_phys = (__force uint64_t)page_to_phys(cmd_ring_page);
  erst_phys = (__force uint64_t)page_to_phys(erst_page);
  event_ring_phys = (__force uint64_t)page_to_phys(event_ring_page);
  scratchpad_phys = (__force uint64_t)page_to_phys(scratchpad_page);

  dcbaa_virt = (__force void *)phys_to_virt((__force phys_addr_t)dcbaa_phys);
  cmd_ring_virt =
      (__force void *)phys_to_virt((__force phys_addr_t)cmd_ring_phys);
  erst_virt = (__force void *)phys_to_virt((__force phys_addr_t)erst_phys);
  event_ring_virt =
      (__force void *)phys_to_virt((__force phys_addr_t)event_ring_phys);

  // Zero all DMA buffers
  __memset((void *)dcbaa_virt, 0, 4096);
  __memset((__force void *)phys_to_virt((__force phys_addr_t)input_ctx_phys), 0,
           4096);
  __memset((void *)cmd_ring_virt, 0, 4096);
  __memset((void *)erst_virt, 0, 4096);
  __memset((void *)event_ring_virt, 0, 4096);
  __memset((__force void *)phys_to_virt((__force phys_addr_t)scratchpad_phys),
           0, 4096);

  // 5. Read HCSPARAMS1
  uint32_t hcsparams1 = xhci_read(XHCI_HCSPARAMS1);
  max_slots = hcsparams1 & 0xFF;
  max_intrs = (hcsparams1 >> 8) & 0x7FF;
  max_ports = (hcsparams1 >> 24) & 0xFF;

  // 6. Configure MSI-X
  int nvectors = pci_enable_msix(xhci_dev, max_intrs);
  if (nvectors <= 0)
    return;

  irq_register(xhci_dev->msix_vector_base, xhci_isr);

  // 7. Controller reset
  uint32_t usbsts = op_read(XHCI_USBSTS);
  if (!(usbsts & USBSTS_HCH)) {
    op_write(XHCI_USBCMD, op_read(XHCI_USBCMD) & ~USBCMD_RS);
    for (int i = 0; i < 1000000; i++) {
      if (op_read(XHCI_USBSTS) & USBSTS_HCH)
        break;
      __asm__ volatile("pause");
    }
  }

  op_write(XHCI_USBCMD, USBCMD_HCRST);
  for (int i = 0; i < 1000000; i++) {
    if (!(op_read(XHCI_USBCMD) & USBCMD_HCRST) &&
        !(op_read(XHCI_USBSTS) & USBSTS_CNR))
      break;
    __asm__ volatile("pause");
  }

  if (op_read(XHCI_USBCMD) & USBCMD_HCRST)
    return;

  // 8. Program DCBAAP
  op_write(XHCI_DCBAAP_LO, (uint32_t)dcbaa_phys);
  op_write(XHCI_DCBAAP_HI, (uint32_t)(dcbaa_phys >> 32));

  uint64_t *dcbaa = (uint64_t *)dcbaa_virt;
  dcbaa[0] = scratchpad_phys;

  // 9. Command Ring setup
  volatile uint32_t *cmd_ring = (volatile uint32_t *)cmd_ring_virt;
  cmd_ring[255 * 4 + 0] = (uint32_t)cmd_ring_phys;
  cmd_ring[255 * 4 + 1] = (uint32_t)(cmd_ring_phys >> 32);
  cmd_ring[255 * 4 + 2] = 0;
  cmd_ring[255 * 4 + 3] = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_TC;

  uint64_t crcr = cmd_ring_phys | CRCR_RCS;
  op_write(XHCI_CRCR_LO, (uint32_t)crcr);
  op_write(XHCI_CRCR_HI, (uint32_t)(crcr >> 32));

  // 10. Event Ring setup
  struct erst_entry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t size;
    uint32_t reserved;
  };
  volatile struct erst_entry *erst = (volatile struct erst_entry *)erst_virt;
  erst[0].addr_lo = (uint32_t)event_ring_phys;
  erst[0].addr_hi = (uint32_t)(event_ring_phys >> 32);
  erst[0].size = 256;

  volatile uint32_t *evt_ring = (volatile uint32_t *)event_ring_virt;
  evt_ring[255 * 4 + 0] = (uint32_t)event_ring_phys;
  evt_ring[255 * 4 + 1] = (uint32_t)(event_ring_phys >> 32);
  evt_ring[255 * 4 + 2] = 0;
  evt_ring[255 * 4 + 3] = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_TC | 1;

  intr_write(0, XHCI_ERSTSZ, 1);
  intr_write(0, XHCI_ERSTBA_LO, (uint32_t)erst_phys);
  intr_write(0, XHCI_ERSTBA_HI, (uint32_t)(erst_phys >> 32));
  uint64_t erdp = event_ring_phys | ERDP_EHB;
  intr_write(0, XHCI_ERDP_LO, (uint32_t)erdp);
  intr_write(0, XHCI_ERDP_HI, (uint32_t)(erdp >> 32));

  // 12. CONFIG = MaxSlots
  op_write(XHCI_CONFIG, max_slots);

  // 13. Start controller
  op_write(XHCI_USBCMD, op_read(XHCI_USBCMD) | USBCMD_RS);
  for (int i = 0; i < 1000000; i++) {
    if (!(op_read(XHCI_USBSTS) & USBSTS_HCH))
      break;
    __asm__ volatile("pause");
  }

  if (op_read(XHCI_USBSTS) & USBSTS_HCH)
    return;

  // USB HID device enumeration
  xhci_init_keyboard();

  // Enable interrupts (done regardless of keyboard presence)
  pci_msix_unmask_entry(xhci_dev, 0);
  op_write(XHCI_USBCMD, op_read(XHCI_USBCMD) | USBCMD_INTE);
  intr_write(0, XHCI_IMAN, intr_read(0, XHCI_IMAN) | IMAN_IE);
}

// ===================== USB HID keyboard enumeration =====================

static int usb_hid_kbd_open(xtask *proc, int fd) {
  for (int i = 0; i < 8; i++)
    if (kbd_openers[i] == 0) {
      kbd_openers[i] = proc->pid;
      break;
    }
  return 0;
}

static int usb_hid_kbd_close(xtask *proc, int fd) {
  for (int i = 0; i < 8; i++)
    if (kbd_openers[i] == proc->pid) {
      kbd_openers[i] = 0;
      break;
    }
  return 0;
}

static void xhci_init_keyboard() {
  uint32_t bsp_apic_id = lapic_read(LAPIC_ID) >> 24;

  // Allocate additional pages for USB keyboard
  usb_hid_shm_page = bfc_alloc_page(1);
  hid_dma_page = bfc_alloc_page_low(1);
  ep0_ring_page = bfc_alloc_page_low(1);
  xhci_intrs[0].ring_page = bfc_alloc_page_low(1);
  dev_ctx_page = bfc_alloc_page_low(1);
  ctrl_dma_page = bfc_alloc_page_low(1);

  if (!usb_hid_shm_page || !hid_dma_page || !ep0_ring_page ||
      !xhci_intrs[0].ring_page || !dev_ctx_page || !ctrl_dma_page)
    return;

  usb_hid_shm_phys = (__force uint64_t)page_to_phys(usb_hid_shm_page);
  usb_hid_shm_virt =
      (__force void *)phys_to_virt((__force phys_addr_t)usb_hid_shm_phys);
  hid_dma_phys = (__force uint64_t)page_to_phys(hid_dma_page);
  hid_dma_virt =
      (__force void *)phys_to_virt((__force phys_addr_t)hid_dma_phys);
  ep0_ring_phys = (__force uint64_t)page_to_phys(ep0_ring_page);
  ep0_ring_virt =
      (__force void *)phys_to_virt((__force phys_addr_t)ep0_ring_phys);
  xhci_intrs[0].ring_phys =
      (__force uint64_t)page_to_phys(xhci_intrs[0].ring_page);
  xhci_intrs[0].ring_virt = (__force void *)phys_to_virt(
      (__force phys_addr_t)xhci_intrs[0].ring_phys);
  dev_ctx_phys = (__force uint64_t)page_to_phys(dev_ctx_page);
  ctrl_dma_phys = (__force uint64_t)page_to_phys(ctrl_dma_page);
  ctrl_dma_virt =
      (__force void *)phys_to_virt((__force phys_addr_t)ctrl_dma_phys);

  // Zero all new pages
  __memset((void *)usb_hid_shm_virt, 0, 4096);
  __memset((void *)hid_dma_virt, 0, 4096);
  __memset((void *)ep0_ring_virt, 0, 4096);
  __memset((void *)xhci_intrs[0].ring_virt, 0, 4096);
  __memset((__force void *)phys_to_virt((__force phys_addr_t)dev_ctx_phys), 0,
           4096);
  __memset((void *)ctrl_dma_virt, 0, 4096);

  // Initialize USB HID SHM header
  volatile struct usb_hid_shm_header *hid_hdr =
      (volatile struct usb_hid_shm_header *)usb_hid_shm_virt;
  hid_hdr->magic = USB_HID_SHM_MAGIC;
  hid_hdr->version = USB_HID_SHM_VERSION;
  for (int i = 0; i < 4; i++) {
    hid_hdr->rings[i].head = 0;
    hid_hdr->rings[i].tail = 0;
    hid_hdr->rings[i].capacity = HID_SUBRING_CAPACITY;
    hid_hdr->rings[i].reserved = 0;
  }

  // Register /dev/usb_hid device with SHM — kbd_driver opens it via open + mmap
  {
    struct shm *hid_shm = shm_create_internal(1);
    if (hid_shm) {
      // Initialize HID SHM header in the new page (same as original init)
      void *new_virt = (__force void *)phys_to_virt(
          (__force phys_addr_t)hid_shm->page_list[0]);
      usb_hid_shm_header *hdr = (usb_hid_shm_header *)new_virt;
      hdr->magic = USB_HID_SHM_MAGIC;
      hdr->version = USB_HID_SHM_VERSION;
      // Sub-ring descriptors (same offsets as original init)
      for (int i = 0; i < 4; i++) {
        hdr->rings[i].head = 0;
        hdr->rings[i].tail = 0;
        hdr->rings[i].capacity = HID_SUBRING_CAPACITY;
        hdr->rings[i].reserved = 0;
      }
      // Update usb_hid_shm_virt to point to the new page
      // (xHCI DMA continues using the original hid_dma_page, not this SHM;
      //  xHCI ISR copies from hid_dma_virt into usb_hid_shm_virt)
      usb_hid_shm_phys = hid_shm->page_list[0];
      usb_hid_shm_virt = new_virt;
      // Free the original bfc page (no longer used)
      bfc_free_page(usb_hid_shm_page, 1);
      usb_hid_shm_page = NULL;

      static struct dev_ops usb_hid_ops;
      __memset(&usb_hid_ops, 0, sizeof(usb_hid_ops));
      usb_hid_ops.driver_pid = 0; // kernel device
      usb_hid_ops.is_block = false;
      usb_hid_ops.open = usb_hid_kbd_open;
      usb_hid_ops.close = usb_hid_kbd_close;
      devtmpfs_create("usb_hid_kbd", &usb_hid_ops, hid_shm);
      shm_put(hid_shm); // devtmpfs_create took a reference via shm_get
    }
  }

  // Initialize Transfer Rings with Link TRBs
  volatile uint32_t *ep0_ring = (volatile uint32_t *)ep0_ring_virt;
  ep0_ring[255 * 4 + 0] = (uint32_t)ep0_ring_phys;
  ep0_ring[255 * 4 + 1] = (uint32_t)(ep0_ring_phys >> 32);
  ep0_ring[255 * 4 + 2] = 0;
  ep0_ring[255 * 4 + 3] = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_TC;

  volatile uint32_t *ep1_ring = (volatile uint32_t *)xhci_intrs[0].ring_virt;
  ep1_ring[255 * 4 + 0] = (uint32_t)xhci_intrs[0].ring_phys;
  ep1_ring[255 * 4 + 1] = (uint32_t)(xhci_intrs[0].ring_phys >> 32);
  ep1_ring[255 * 4 + 2] = 0;
  ep1_ring[255 * 4 + 3] = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_TC;

  xhci_intrs[0].enqueue = 0;
  xhci_intrs[0].ccs = 1;
  xhci_intrs[0].ep_dci = 3; // EP1-IN doorbell target (DCI)
  xhci_intrs[0].ep_num =
      3; // EPID in transfer events (QEMU reports DCI, not EP number)

  // ---- Step A: Port Discovery ----
  int usb_port = -1;
  for (int p = 0; p < max_ports; p++) {
    uint32_t portsc = portsc_read(p);
    if (portsc & PORTSC_CCS) {
      usb_port = p;
      break;
    }
  }

  if (usb_port < 0)
    return;

  // ---- Step B: Port Reset ----
  uint32_t portsc = portsc_read(usb_port);
  portsc &= ~PORTSC_PR;
  portsc |= PORTSC_PR;
  portsc_write(usb_port, portsc);

  for (int i = 0; i < 1000000; i++) {
    portsc = portsc_read(usb_port);
    if (!(portsc & PORTSC_PR) && (portsc & PORTSC_PED))
      break;
    __asm__ volatile("pause");
  }

  portsc = portsc_read(usb_port);
  if (portsc & PORTSC_PR)
    return;

  int port_speed = (portsc & PORTSC_SPEED_MASK) >> 10;

  // ---- Step C: Enable Slot ----
  trb es_trb;
  es_trb.dword0 = 0;
  es_trb.dword1 = 0;
  es_trb.dword2 = 0;
  es_trb.dword3 = (TRB_ENABLE_SLOT << TRB_TYPE_SHIFT);
  cmd_ring_push(&es_trb);
  db_write(0, 0);

  uint32_t cc = 0, sid = 0, etype = 0;
  for (int tries = 0; tries < 10; tries++) {
    if (poll_event(&cc, &sid, &etype) < 0)
      return;
    if (etype == TRB_CMD_COMPLETE)
      break;
  }

  if (cc != CC_SUCCESS || etype != TRB_CMD_COMPLETE)
    return;

  // ---- Step D: Address Device ----
  uint64_t *dcbaa = (uint64_t *)dcbaa_virt;
  dcbaa[sid] = dev_ctx_phys;

  volatile uint32_t *ictx = (__force volatile uint32_t *)phys_to_virt(
      (__force phys_addr_t)input_ctx_phys);
  __memset((void *)ictx, 0, 4096);

  ictx[1] = (1 << 0) | (1 << 1); // Add flags: Slot + EP0
  ictx[8] = (port_speed << 20);
  ictx[9] = ((usb_port + 1) & 0xFF) << 16;
  ictx[15] = 1; // ctx_entries = 1

  int max_pkt = (port_speed == 2) ? 8 : 64;
  ictx[16] = 0;
  ictx[17] = (EP_TYPE_CONTROL << 3) | (max_pkt << 16);
  ictx[18] = (uint32_t)(ep0_ring_phys | 1);
  ictx[19] = (uint32_t)(ep0_ring_phys >> 32);
  ictx[22] = 8;

  trb addr_trb;
  addr_trb.dword0 = (uint32_t)input_ctx_phys;
  addr_trb.dword1 = (uint32_t)(input_ctx_phys >> 32);
  addr_trb.dword2 = 0;
  addr_trb.dword3 = (TRB_ADDRESS_DEV << TRB_TYPE_SHIFT) | (sid << 24);
  cmd_ring_push(&addr_trb);
  db_write(0, 0);

  for (int tries = 0; tries < 10; tries++) {
    if (poll_event(&cc, &sid, &etype) < 0)
      return;
    if (etype == TRB_CMD_COMPLETE)
      break;
  }

  if (cc != CC_SUCCESS || etype != TRB_CMD_COMPLETE)
    return;

  xhci_intrs[0].slot_id = sid;

  // ---- Step E: Get Descriptor (Device Descriptor) ----
  volatile uint32_t *ep0 = (volatile uint32_t *)ep0_ring_virt;

  trb setup_trb;
  setup_trb.dword0 = 0x01000680;
  setup_trb.dword1 = 0x00120000;
  setup_trb.dword2 = 8;
  setup_trb.dword3 = (TRB_SETUP_STAGE << TRB_TYPE_SHIFT) | TRB_IDT | TRB_IOC;
  xfer_ring_enqueue(ep0, &ep0_ring_enqueue, &ep0_ring_ccs, &setup_trb);

  trb data_trb;
  data_trb.dword0 = (uint32_t)ctrl_dma_phys;
  data_trb.dword1 = (uint32_t)(ctrl_dma_phys >> 32);
  data_trb.dword2 = 18;
  data_trb.dword3 = (TRB_DATA_STAGE << TRB_TYPE_SHIFT) | TRB_DIR_IN | TRB_IOC;
  xfer_ring_enqueue(ep0, &ep0_ring_enqueue, &ep0_ring_ccs, &data_trb);

  trb status_trb;
  status_trb.dword0 = 0;
  status_trb.dword1 = 0;
  status_trb.dword2 = 0;
  status_trb.dword3 = (TRB_STATUS_STAGE << TRB_TYPE_SHIFT) | TRB_IOC;
  xfer_ring_enqueue(ep0, &ep0_ring_enqueue, &ep0_ring_ccs, &status_trb);

  db_write(sid, 1);
  for (int tries = 0; tries < 30; tries++) {
    if (poll_event(&cc, &sid, &etype) < 0)
      return;
    if (etype == TRB_TRANSFER && cc == CC_SUCCESS)
      break;
    if (etype == TRB_CMD_COMPLETE)
      continue;
  }

  if (cc != CC_SUCCESS)
    return;

  // ---- Step E2: Get Descriptor (Configuration Descriptor) ----
  ep0 = (volatile uint32_t *)ep0_ring_virt;
  trb cfg_setup;
  cfg_setup.dword0 = 0x02000680;
  cfg_setup.dword1 = 0x00400000;
  cfg_setup.dword2 = 8;
  cfg_setup.dword3 = (TRB_SETUP_STAGE << TRB_TYPE_SHIFT) | TRB_IDT | TRB_IOC;
  xfer_ring_enqueue(ep0, &ep0_ring_enqueue, &ep0_ring_ccs, &cfg_setup);

  trb cfg_data;
  cfg_data.dword0 = (uint32_t)ctrl_dma_phys;
  cfg_data.dword1 = (uint32_t)(ctrl_dma_phys >> 32);
  cfg_data.dword2 = 64;
  cfg_data.dword3 = (TRB_DATA_STAGE << TRB_TYPE_SHIFT) | TRB_DIR_IN | TRB_IOC;
  xfer_ring_enqueue(ep0, &ep0_ring_enqueue, &ep0_ring_ccs, &cfg_data);

  trb cfg_status;
  cfg_status.dword0 = 0;
  cfg_status.dword1 = 0;
  cfg_status.dword2 = 0;
  cfg_status.dword3 = (TRB_STATUS_STAGE << TRB_TYPE_SHIFT) | TRB_IOC;
  xfer_ring_enqueue(ep0, &ep0_ring_enqueue, &ep0_ring_ccs, &cfg_status);

  db_write(sid, 1);
  for (int tries = 0; tries < 30; tries++) {
    if (poll_event(&cc, &sid, &etype) < 0)
      return;
    if (etype == TRB_TRANSFER && cc == CC_SUCCESS)
      break;
    if (etype == TRB_CMD_COMPLETE)
      continue;
  }
  if (cc != CC_SUCCESS)
    return;

  // Parse Configuration Descriptor to find EP1-IN max packet size
  volatile uint8_t *cfg_buf = (volatile uint8_t *)ctrl_dma_virt;
  int cfg_total = cfg_buf[2] | (cfg_buf[3] << 8);
  int off = 0;
  uint16_t ep_max_pkt = 8;
  uint8_t ep_interval = 4;
  while (off + 1 < 64 && off < cfg_total) {
    uint8_t dlen = cfg_buf[off];
    uint8_t dtype = cfg_buf[off + 1];
    if (dlen == 0)
      break;
    if (dtype == 5 && dlen >= 7 && off + 6 < 64) {
      ep_max_pkt = cfg_buf[off + 4] | (cfg_buf[off + 5] << 8);
      ep_interval = cfg_buf[off + 6];
    }
    off += dlen;
  }

  // ---- Step F: Set Protocol (Boot Protocol) ----
  trb sp_setup;
  sp_setup.dword0 = 0x00000B21;
  sp_setup.dword1 = 0x00000000;
  sp_setup.dword2 = 8;
  sp_setup.dword3 = (TRB_SETUP_STAGE << TRB_TYPE_SHIFT) | TRB_IDT | TRB_IOC;
  xfer_ring_enqueue(ep0, &ep0_ring_enqueue, &ep0_ring_ccs, &sp_setup);

  trb sp_status;
  sp_status.dword0 = 0;
  sp_status.dword1 = 0;
  sp_status.dword2 = 0;
  sp_status.dword3 =
      (TRB_STATUS_STAGE << TRB_TYPE_SHIFT) | TRB_DIR_IN | TRB_IOC;
  xfer_ring_enqueue(ep0, &ep0_ring_enqueue, &ep0_ring_ccs, &sp_status);

  db_write(sid, 1);
  for (int tries = 0; tries < 10; tries++) {
    if (poll_event(&cc, &sid, &etype) < 0)
      return;
    if (etype == TRB_TRANSFER && cc == CC_SUCCESS)
      break;
    if (etype == TRB_CMD_COMPLETE)
      continue;
  }

  if (cc != CC_SUCCESS)
    return;

  // ---- Step F2: Set Configuration ----
  trb sc_setup;
  sc_setup.dword0 = 0x00010900;
  sc_setup.dword1 = 0x00000000;
  sc_setup.dword2 = 8;
  sc_setup.dword3 = (TRB_SETUP_STAGE << TRB_TYPE_SHIFT) | TRB_IDT | TRB_IOC;
  xfer_ring_enqueue(ep0, &ep0_ring_enqueue, &ep0_ring_ccs, &sc_setup);

  trb sc_status;
  sc_status.dword0 = 0;
  sc_status.dword1 = 0;
  sc_status.dword2 = 0;
  sc_status.dword3 =
      (TRB_STATUS_STAGE << TRB_TYPE_SHIFT) | TRB_DIR_IN | TRB_IOC;
  xfer_ring_enqueue(ep0, &ep0_ring_enqueue, &ep0_ring_ccs, &sc_status);

  db_write(sid, 1);
  for (int tries = 0; tries < 10; tries++) {
    if (poll_event(&cc, &sid, &etype) < 0)
      return;
    if (etype == TRB_TRANSFER && cc == CC_SUCCESS)
      break;
    if (etype == TRB_CMD_COMPLETE)
      continue;
  }
  if (cc != CC_SUCCESS)
    return;

  // ---- Step G: Configure Endpoint (EP1-IN Interrupt) ----
  __memset((void *)ictx, 0, 4096);

  ictx[1] = (1 << 0) | (1 << 3); // A0=Slot, A3=EP1-IN (DCI=3)

  ictx[8] = (port_speed << 20);
  ictx[9] = ((usb_port + 1) & 0xFF) << 16;
  ictx[15] = 2; // ctx_entries = 2

  ictx[32] = 0;
  ictx[33] = (EP_TYPE_INTERRUPT_IN << 3) | (ep_max_pkt << 16);
  ictx[34] = (uint32_t)(xhci_intrs[0].ring_phys | 1);
  ictx[35] = (uint32_t)(xhci_intrs[0].ring_phys >> 32);
  ictx[39] = ep_interval;

  trb cfg_trb;
  cfg_trb.dword0 = (uint32_t)input_ctx_phys;
  cfg_trb.dword1 = (uint32_t)(input_ctx_phys >> 32);
  cfg_trb.dword2 = 0;
  cfg_trb.dword3 = (TRB_CONFIG_EP << TRB_TYPE_SHIFT) | (sid << 24);
  cmd_ring_push(&cfg_trb);
  db_write(0, 0);

  for (int tries = 0; tries < 10; tries++) {
    if (poll_event(&cc, &sid, &etype) < 0)
      return;
    if (etype == TRB_CMD_COMPLETE)
      break;
  }

  if (cc != CC_SUCCESS || etype != TRB_CMD_COMPLETE)
    return;

  // ---- Step H: Start keyboard data transfer ----
  trb norm_trb;
  norm_trb.dword0 = (uint32_t)hid_dma_phys;
  norm_trb.dword1 = (uint32_t)(hid_dma_phys >> 32);
  norm_trb.dword2 = 8;
  norm_trb.dword3 = (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC;
  xfer_ring_enqueue(ep1_ring, &xhci_intrs[0].enqueue, &xhci_intrs[0].ccs,
                    &norm_trb);

  db_write(sid, 3);

  // Mask PS/2 keyboard IRQ (GSI 1) — no longer needed
  ioapic_set_irq(1, 33, bsp_apic_id, true, false, false);

  printk(LOG_INFO, "xhci: USB keyboard ready\n");
  xhci_poll_initialized = 1;
}

// ===================== Driver registry =====================
#include "kernel/driver/driver.h"

dev_driver xhci_driver = {
    .name = "xhci",
    .pci_class = 0x0C0330, // USB xHCI (class=0x0C, subclass=0x03, prog_if=0x30)
    .pci_vendor = 0,
    .pci_device = 0,
    .init = xhci_init,
    .ops = NULL, // usb_hid_ops is created dynamically in xhci_init_keyboard
};
