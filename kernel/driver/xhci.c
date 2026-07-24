/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/xhci.h"

#include <stdbool.h>
#include <stdint.h>

#include "arch/x64/apic.h"
#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "kernel/bsd/devtmpfs.h"
#include "kernel/driver/bsd_types.h" // struct file, file_get/file_put, fd_lookup, FD_EVENTFD
#include "kernel/driver/pci.h"
#include "kernel/xcore/kpi.h"
#include "kernel/xcore/list.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mm_types.h"
#include "kernel/xcore/sched.h" // schedule, wake_with_event
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/wait_queue.h" // wait_queue_head, __wake_up, add/remove
#include "kernel/xcore/xtask.h"

#include <xos/errno.h>
#include <xos/fcntl.h>
#include <xos/hidraw.h>
#include <xos/input.h>
#include <xos/ioctl.h> // HID_BIND_IRQFD / HID_UNBIND_IRQFD
#include <xos/shm.h>
#include <xos/signal.h> // SIGKILL/SIGSTOP
#include <xos/socket.h> // POLLIN

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
// Total software CCS flips (each = one full lap consumed). Compared against the
// HC's actual lap count (inferred at dump time from the cycle-bit pattern over
// the whole ring) to detect a software/HC cycle desync: if they differ, the
// consumer flipped CCS at the wrong point.
static int event_ring_ccs_flips = 0;

// Last-ISR snapshot: captured at the end of every xhci_isr invocation so the
// hang dump can show what the most recent successful ISR saw (CCS flip state,
// dequeue pointer, how many events it consumed, the IMAN it cleared). This is
// the key to telling whether the ISR mis-handled the event ring cycle flip
// right before the hang.
static struct {
  int count;         // total ISR invocations
  int dequeue;       // event_ring_dequeue at ISR exit
  int ccs;           // event_ring_ccs at ISR exit
  int consumed;      // events drained this invocation
  uint32_t iman_in;  // IMAN value read at ISR entry (before clearing IP)
  uint32_t usbsts;   // USBSTS at ISR entry
  uint32_t last_cc;  // completion code of the last EP1-IN transfer event
  uint32_t last_len; // transfer length of the last EP1-IN transfer event
} last_isr;

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

// USB HID SHM page (kernel-allocated, shared with the evdev process via mmap)
static struct page *usb_hid_shm_page;
static uint64_t usb_hid_shm_phys;
static void *usb_hid_shm_virt;

// HID irqfd registry: the xHCI ISR signals these eventfd files (instead of
// the old pid-array kbd_openers[] + wake_process) when a HID report lands.
// Single consumer (evdev) binds one irqfd; capacity 8 is harmless headroom.
// Protected by hid_irqfd_lock (taken irqsave in the ISR).  Each entry holds a
// file reference (file_get on bind, file_put on unbind/close) so the ISR never
// touches a freed file.  (evdev_refact.md §4.2)
#define HID_IRQFD_MAX 8
static struct file *hid_irqfds[HID_IRQFD_MAX];
static spinlock hid_irqfd_lock = SPINLOCK_INIT;

// hidraw read() wait queue: blocking /dev/hidraw0 readers sleep here, woken by
// the xHCI ISR after it enqueues a new HID report into the SHM sub-ring.
static wait_queue_head *hidraw_wq;

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
    // Refresh the Link TRB cycle bit with this lap's ccs (pre-toggle) so the HC
    // follows the link on every lap. See xfer_ring_enqueue for the full
    // rationale.
    uint32_t link_d3 = ring[255 * 4 + 3];
    link_d3 = (link_d3 & ~1u) | (cmd_ring_ccs & 1);
    ring[255 * 4 + 3] = link_d3;
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
    // The Link TRB at slot 255 must carry the cycle bit the HC expects when it
    // reaches this slot, i.e. THIS lap's ccs (before the toggle): the HC's
    // cycle state still equals the current lap's ccs at slot 255, and it only
    // follows the link when the Link TRB's cycle bit matches. TC=1 then makes
    // the HC toggle its own cycle state for the next lap. So we refresh the
    // Link TRB's cycle bit with the pre-toggle ccs on every wrap. A static Link
    // TRB cycle bit stalls the HC at slot 255 after the first lap whose ccs
    // differs from the Link TRB's — observed as the keyboard freezing after
    // exactly 255*N hid reports.
    uint32_t link_d3 = ring_virt[255 * 4 + 3];
    link_d3 = (link_d3 & ~1u) | (*ccs & 1);
    ring_virt[255 * 4 + 3] = link_d3;
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
    // ERST size is 256: flip CCS on wrap to 256 (after consuming idx 255), not
    // 255. Wrapping at 255 skips TRB 255 and drifts the consumer one slot per
    // lap, so the HC sees the ring as full (EHB stays set) and the ISR
    // deadlocks consuming nothing.
    if (event_ring_dequeue >= 256) {
      event_ring_dequeue = 0;
      event_ring_ccs ^= 1;
      event_ring_ccs_flips++;
    }

    // Advance HC dequeue pointer; preserve read-back EHB, never set it.
    uint32_t erdp_lo = intr_read(0, XHCI_ERDP_LO);
    uint64_t erdp = event_ring_phys + (uint64_t)event_ring_dequeue * 16;
    erdp |= (uint64_t)(erdp_lo & ERDP_EHB);
    intr_write(0, XHCI_ERDP_LO, (uint32_t)erdp);
    intr_write(0, XHCI_ERDP_HI, (uint32_t)(erdp >> 32));
    return 0;
  }

  return -1;
}

// ===================== ISR =====================

static void xhci_isr(trapframe *tf) {
  static int isr_count = 0;
  isr_count++;
  // Acknowledge the interrupt first (write-1-to-clear IP, keep IE). This is
  // safe because we drain the event ring below before returning; any event
  // that lands during/after this write sets IP again and re-triggers us.
  uint32_t iman = intr_read(0, XHCI_IMAN);
  uint32_t sts_in = op_read(XHCI_USBSTS);
  last_isr.iman_in = iman;
  last_isr.usbsts = sts_in;
  last_isr.count = isr_count;
  if (iman & IMAN_IP) {
    intr_write(0, XHCI_IMAN, iman | IMAN_IP | IMAN_IE);
  }

  // Walk event ring, draining every valid TRB the HC has produced.
  volatile uint32_t *ring = (volatile uint32_t *)event_ring_virt;
  int consumed = 0;
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
        // Record the completion code + transfer length of this EP1-IN transfer
        // event so the hang dump can show what the HC last reported (e.g. a
        // NAK/short/error completion that the ISR mis-handles).
        last_isr.last_cc = cc;
        last_isr.last_len = d2 & 0xFFFFFF;
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

          // Notify registered irqfds: signal each bound eventfd so evdev's
          // epoll_wait wakes on the irqfd (evdev_refact.md §4.2/§3.2).
          uint64_t irqflags;
          spin_lock_irqsave(&hid_irqfd_lock, &irqflags);
          for (int i = 0; i < HID_IRQFD_MAX; i++)
            if (hid_irqfds[i])
              eventfd_signal_isr(hid_irqfds[i]);
          spin_unlock_irqrestore(&hid_irqfd_lock, irqflags);
          // Wake any blocking /dev/hidraw0 read() waiters (report now
          // enqueued in SHM sub-ring).
          if (hidraw_wq)
            __wake_up(hidraw_wq, POLLIN);
        }
      }
    } else if (type == TRB_CMD_COMPLETE) {
      // No action needed at runtime
    } else if (type == TRB_PORT_STATUS) {
      // No action needed at runtime
    }

    event_ring_dequeue++;
    // ERST size is 256: flip CCS on wrap to 256 (after consuming idx 255), not
    // 255. Wrapping at 255 skips TRB 255 and drifts the consumer one slot per
    // lap, so the HC sees the ring as full (EHB stays set) and the ISR
    // deadlocks consuming nothing.
    if (event_ring_dequeue >= 256) {
      event_ring_dequeue = 0;
      event_ring_ccs ^= 1;
      event_ring_ccs_flips++;
    }
    consumed++;
  }

  // Only touch ERDP if we advanced the dequeue pointer. Writing ERDP with an
  // unchanged pointer (and especially setting EHB) when nothing was consumed
  // never clears the HC's pending state and re-triggers the ISR indefinitely
  // (interrupt storm). Preserve the read-back EHB; software must never set it
  // (it is HC-set/SW-clear via the dequeue-pointer write).
  if (consumed) {
    uint32_t erdp_lo = intr_read(0, XHCI_ERDP_LO);
    uint64_t erdp = event_ring_phys + (uint64_t)event_ring_dequeue * 16;
    erdp |= (uint64_t)(erdp_lo & ERDP_EHB);
    intr_write(0, XHCI_ERDP_LO, (uint32_t)erdp);
    intr_write(0, XHCI_ERDP_HI, (uint32_t)(erdp >> 32));
  }

  // Snapshot exit state so the hang dump can inspect the last successful ISR.
  last_isr.dequeue = event_ring_dequeue;
  last_isr.ccs = event_ring_ccs;
  last_isr.consumed = consumed;

  // Clear the latched event-interrupt status (USBSTS.EINT, write-1-to-clear).
  // EINT latches "an event interrupt occurred" and stays set until software
  // clears it; leaving it set can keep the HC's interrupt-raise logic in a
  // permanently-satisfied state so it stops re-asserting IMAN.IP for new
  // events.
  op_write(XHCI_USBSTS, USBSTS_EINT);

  lapic_eoi();
}

// Called periodically from timer_handler to ring EP1-IN doorbell.
// This ensures QEMU's xHCI emulation retries NAK'ed interrupt transfers
// (QEMU only retries on doorbell write, not on timer expiry after NAK).
static int xhci_poll_initialized = 0;

void xhci_poll() {
  if (!xhci_poll_initialized)

    // Ring the EP1-IN doorbell every poll. QEMU's xHCI only retries a NAK'ed
    // interrupt-IN transfer on a doorbell write (not on timer expiry), so
    // without this a key press that arrives while the previous transfer is
    // NAK'ing would never be delivered.  Event processing is handled by
    // xhci_isr (MSI-X).
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
  // Link TRB cycle bit = initial producer cycle state (cmd_ring_ccs=1). The HC
  // only follows the link when this bit matches its cycle state; TC=1 toggles
  // the HC's state for the next lap. cmd_ring_push refreshes this bit on wrap.
  cmd_ring[255 * 4 + 3] = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_TC | 1;

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

  // Event ring has NO Link TRB. Unlike command/transfer rings (which wrap via a
  // Link TRB at the last slot), the event ring wraps purely by ERST.size — the
  // HC manages the cycle-bit flip itself when it wraps past the last entry. All
  // 256 slots (0..255) are real event TRBs. Putting a Link TRB here (as this
  // code previously did, with cycle=1) corrupts the cycle tracking: the first
  // 255 events consume slots 0..254, then the HC/software wrap points diverge
  // by one slot and the CCS drifts, deadlocking the ISR (consumed=0 forever).

  intr_write(0, XHCI_ERSTSZ, 1);
  intr_write(0, XHCI_ERSTBA_LO, (uint32_t)erst_phys);
  intr_write(0, XHCI_ERSTBA_HI, (uint32_t)(erst_phys >> 32));
  // Initial dequeue pointer at ring start; do NOT set EHB — it is HC-set only
  // and there are no pending events before the controller starts running.
  intr_write(0, XHCI_ERDP_LO, (uint32_t)event_ring_phys);
  intr_write(0, XHCI_ERDP_HI, (uint32_t)(event_ring_phys >> 32));

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

// usb_hid_kbd_open: no-op now.  HID interrupt delivery is registered
// explicitly via the HID_BIND_IRQFD ioctl (hid_ops_ioctl below); the old
// pid-array kbd_openers[] registration is gone.  (evdev_refact.md §4.2)
static int usb_hid_kbd_open(xtask *proc, int fd) {
  (void)proc;
  (void)fd;
  return 0;
}

// usb_hid_kbd_close: drop any irqfd slots still bound by this process
// (e.g. evdev exited without HID_UNBIND_IRQFD).  The normal path is
// HID_UNBIND_IRQFD (hid_ops_ioctl); this is the safety net for fd-table
// teardown.  (evdev_refact.md §4.2 生命周期)
static int usb_hid_kbd_close(xtask *proc, int fd) {
  (void)fd;
  (void)proc;
  uint64_t flags;
  spin_lock_irqsave(&hid_irqfd_lock, &flags);
  for (int i = 0; i < HID_IRQFD_MAX; i++) {
    struct file *irqfd = hid_irqfds[i];
    if (irqfd) {
      // Drop the slot regardless of who bound it (single-consumer evdev);
      // unbind the file and release our registry reference.
      hid_irqfds[i] = NULL;
      spin_unlock_irqrestore(&hid_irqfd_lock, flags);
      file_put(irqfd);
      spin_lock_irqsave(&hid_irqfd_lock, &flags);
    }
  }
  spin_unlock_irqrestore(&hid_irqfd_lock, flags);
  return 0;
}

// hidraw read(): dequeue one 8B raw HID Boot report from the keyboard sub-ring
// and copy_to_user. Shares the same SHM sub-ring as the evdev-mmap path:
// whoever reads first advances tail. Empty -> block on hidraw_wq (woken by
// ISR), or -EAGAIN if O_NONBLOCK. refact_evdev.md §14.2.
static void hidraw_wake_cb(wait_queue_t *wq, unsigned long flags) {
  xtask *target = (xtask *)wq->data;
  (void)flags;
  wake_with_event(target, WAIT_POLL);
}

static ssize_t usb_hidraw_read(xtask *proc, int fd, void *buf, size_t count) {
  if (count < 8)
    return -EINVAL;
  if (!usb_hid_shm_virt)
    return -ENODEV;

  struct file *f = fd_lookup(proc->proc->files, fd);
  bool nonblock = f && (f->flags & O_NONBLOCK);

  volatile struct usb_hid_shm_header *hdr =
      (volatile struct usb_hid_shm_header *)usb_hid_shm_virt;

  wait_queue_t wait;
  bool queued = false;
  if (!nonblock && hidraw_wq) {
    wait.func = hidraw_wake_cb;
    wait.data = current_task;
    wait.exclusive = 0;
    list_init(&wait.node);
    add_wait_queue(hidraw_wq, &wait);
    queued = true;
  }

  ssize_t ret;
  for (;;) {
    uint32_t head = __atomic_load_n(&hdr->rings[0].head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&hdr->rings[0].tail, __ATOMIC_ACQUIRE);
    if (head != tail) {
      volatile struct usb_hid_slot *slot =
          (volatile struct usb_hid_slot *)((uint8_t *)usb_hid_shm_virt +
                                           HID_SUBRING_KBD_OFFSET +
                                           tail * HID_SLOT_SIZE);
      if (copy_to_user(buf, (void *)slot->data, 8)) {
        ret = -EFAULT;
        goto out;
      }
      __atomic_store_n(&hdr->rings[0].tail, (tail + 1) % HID_SUBRING_CAPACITY,
                       __ATOMIC_RELEASE);
      ret = 8;
      goto out;
    }
    // Empty.
    if (nonblock) {
      ret = -EAGAIN;
      goto out;
    }
    // No wait queue (init kmalloc failed): cant block safely, fall back.
    if (!hidraw_wq) {
      ret = -EAGAIN;
      goto out;
    }
    // EINTR on pending signal (mirror eventfd read).
    {
      if (signal_pending(current_task)) {
        ret = -ERESTART;
        goto out;
      }
    }
    current_task->state = BLOCKED;
    current_task->wait_event = WAIT_POLL;
    schedule();
  }

out:
  if (queued)
    remove_wait_queue(hidraw_wq, &wait);
  return ret;
}

// hidraw + irqfd combined ioctl handler.
// HIDIOCGRAWINFO: return raw device info (§14.3).
// HID_BIND_IRQFD / HID_UNBIND_IRQFD: register/unregister an eventfd as the HID
// interrupt-delivery fd (evdev_refact.md §4.2).
static long usb_hid_ioctl(uint32_t cmd, void *arg) {
  // hidraw HIDIOCGRAWINFO
  if (cmd == HIDIOCGRAWINFO) {
    struct hidraw_devinfo info = {
        .bustype = BUS_USB, .vendor = 1, .product = 1};
    __memcpy(arg, &info, sizeof(info));
    return 0;
  }

  // HID irqfd bind/unbind
  if (cmd == HID_BIND_IRQFD) {
    int irqfd_fd = *(int *)arg;
    xtask *proc = current_task;
    struct file *irqfd = fd_lookup(proc->proc->files, irqfd_fd);
    if (!irqfd || irqfd->type != FD_EVENTFD)
      return -EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&hid_irqfd_lock, &flags);
    int slot = -1;
    for (int i = 0; i < HID_IRQFD_MAX; i++) {
      if (!hid_irqfds[i]) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      spin_unlock_irqrestore(&hid_irqfd_lock, flags);
      return -EBUSY;
    }
    file_get(irqfd); // registry holds a reference until unbind/close
    hid_irqfds[slot] = irqfd;
    spin_unlock_irqrestore(&hid_irqfd_lock, flags);
    return 0;
  }

  // HID_UNBIND_IRQFD: clear the first slot matching the callers irqfd.
  {
    uint64_t flags;
    spin_lock_irqsave(&hid_irqfd_lock, &flags);
    struct file *dropped = NULL;
    for (int i = 0; i < HID_IRQFD_MAX; i++) {
      if (hid_irqfds[i]) {
        dropped = hid_irqfds[i];
        hid_irqfds[i] = NULL;
        break;
      }
    }
    spin_unlock_irqrestore(&hid_irqfd_lock, flags);
    if (dropped)
      file_put(dropped);
    return 0;
  }
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

  // Register /dev/hidraw0 device with SHM — evdev opens it via open + mmap
  // (and third-party hidraw tools via read()/HIDIOCG*); refact_evdev.md §14.
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
      usb_hid_ops.read = usb_hidraw_read;
      usb_hid_ops.ioctl = usb_hid_ioctl;
      // hidraw_wq: lazily allocate the blocking-read wait queue now that the
      // device exists; the ISR __wake_ups it after enqueuing a HID report.
      if (!hidraw_wq) {
        hidraw_wq = (wait_queue_head *)kmalloc(sizeof(wait_queue_head));
        if (hidraw_wq)
          init_wait_queue_head(hidraw_wq);
      }
      devtmpfs_create("hidraw0", &usb_hid_ops, hid_shm);
      shm_put(hid_shm); // devtmpfs_create took a reference via shm_get
    }
  }

  // Initialize Transfer Rings with Link TRBs
  volatile uint32_t *ep0_ring = (volatile uint32_t *)ep0_ring_virt;
  ep0_ring[255 * 4 + 0] = (uint32_t)ep0_ring_phys;
  ep0_ring[255 * 4 + 1] = (uint32_t)(ep0_ring_phys >> 32);
  ep0_ring[255 * 4 + 2] = 0;
  ep0_ring[255 * 4 + 3] = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_TC | 1;

  volatile uint32_t *ep1_ring = (volatile uint32_t *)xhci_intrs[0].ring_virt;
  ep1_ring[255 * 4 + 0] = (uint32_t)xhci_intrs[0].ring_phys;
  ep1_ring[255 * 4 + 1] = (uint32_t)(xhci_intrs[0].ring_phys >> 32);
  ep1_ring[255 * 4 + 2] = 0;
  // Link TRB cycle bit must equal the producer's current cycle state (ccs=1 at
  // init) so the HC, when it reaches this slot at the end of a lap, sees a
  // cycle bit matching its own cycle state and follows the link. With TC=1 the
  // HC then toggles its cycle state for the next lap. A Link TRB whose cycle
  // bit never matches causes the HC to stall at slot 255 after one full lap
  // (exactly 255 NORMAL TRBs), reproducing the keyboard freeze.
  ep1_ring[255 * 4 + 3] = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_TC | 1;

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
