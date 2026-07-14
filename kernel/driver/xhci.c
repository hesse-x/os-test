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

// Poll counter for the IP-stuck detector's rate-limiting in xhci_poll.
static int xhci_poll_count = 0;

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

// Track consecutive polls with IP stuck. The boot-time spurious IP clears
// quickly once the first real interrupt fires, so we only dump after IP stays
// asserted for many polls (real hang). Re-arm after a dump so we can re-capture
// the state again if it recurs / changes.
static int xhci_ip_stuck_ticks = 0;
static int xhci_ip_last_dump_tick = 0;
#define XHCI_IP_STUCK_DUMP_THRESHOLD 20 // ~2s of stuck polls (poll runs ~10Hz)

// xhci_poll_count is defined above xhci_isr (used here for dump rate-limiting).

static void xhci_dump_int_state(uint32_t iman);

// Dump the complete interrupt-delivery chain for the xHCI MSI-X vector, to
// localize where a pending interrupt gets lost between the HC and the CPU.
// Prints: IMAN, MSI-X entry0 (mask + message addr/data), LAPIC TMR/IRR/ISR
// bit for the vector (edge/level + pending-in-lapic + in-service), IDT gate
// 64, and CPU RFLAGS.IF. Called once when IP is first observed stuck.
static void xhci_dump_int_state(uint32_t iman) {
  int vec = xhci_dev->msix_vector_base;
  printk(LOG_WARN, "=== xHCI INT DUMP (vec=%d) ===\n", vec);
  printk(LOG_WARN, "IMAN=0x%x (IE=%d IP=%d)\n", iman, (iman >> 1) & 1,
         iman & 1);

  // MSI-X capability global state: Message Control bits.
  //   bit 15 = MSI-X Enable, bit 14 = Function Mask (globally masks all vectors
  //   when set). If Function Mask is set or MSI-X Enable is clear, no MSI-X
  //   interrupt can be delivered regardless of per-entry state.
  uint32_t cap_dword = pci_read_config(
      xhci_dev->bus, xhci_dev->dev, xhci_dev->func, xhci_dev->msix_cap_offset);
  uint16_t msg_ctrl = (cap_dword >> 16) & 0xFFFF;
  printk(LOG_WARN, "MSI-X cap: Enable=%d FunctionMask=%d (msg_ctrl=0x%x)\n",
         (msg_ctrl >> 15) & 1, (msg_ctrl >> 14) & 1, msg_ctrl);

  // xHCI HC global state. USBSTS tells us whether the HC is halted (HCH), has a
  // host-system error (HSE), an event interrupt pending (EINT), or is still
  // controller-not-ready (CNR). USBCMD confirms interrupts are enabled (INTE)
  // and the HC is running (RS). EINT=1 with IP=1 but no LAPIC delivery means
  // the HC asserted its interrupt but the MSI never reached the LAPIC.
  uint32_t usbsts = op_read(XHCI_USBSTS);
  uint32_t usbcmd = op_read(XHCI_USBCMD);
  printk(LOG_WARN,
         "USBSTS=0x%x (HCH=%d HSE=%d EINT=%d PCD=%d CNR=%d) USBCMD=0x%x (RS=%d "
         "INTE=%d)\n",
         usbsts, usbsts & USBSTS_HCH ? 1 : 0, usbsts & USBSTS_HSE ? 1 : 0,
         usbsts & USBSTS_EINT ? 1 : 0, usbsts & USBSTS_PCD ? 1 : 0,
         usbsts & USBSTS_CNR ? 1 : 0, usbcmd, usbcmd & USBCMD_RS ? 1 : 0,
         usbcmd & USBCMD_INTE ? 1 : 0);

  // Event ring state. event_ring_dequeue is the software consume pointer;
  // ERDP.EHB (bit 3) is set by the HC when it has produced events the software
  // hasn't yet acknowledged (via the ERDP write). If EHB=1 with IP=1, the HC
  // has undelivered/unacked events AND an unacked interrupt — and our ISR never
  // ran to drain them. Peek the TRB at the dequeue slot to see if a fresh event
  // is waiting (cycle bit matches CCS).
  uint32_t erdp_lo = intr_read(0, XHCI_ERDP_LO);
  uint32_t erdp_hi = intr_read(0, XHCI_ERDP_HI);
  uint64_t erdp_val = ((uint64_t)erdp_hi << 32) | erdp_lo;
  uint64_t erdp_idx_phys = erdp_val & ~0xFu; // low 4 bits are flags (EHB etc.)
  int hc_dequeue =
      (erdp_idx_phys == 0) ? -1 : (int)((erdp_idx_phys - event_ring_phys) / 16);
  volatile uint32_t *ering = (volatile uint32_t *)event_ring_virt;
  int eidx = event_ring_dequeue * 4;
  uint32_t trb_d3 = ering[eidx + 3];
  int trb_cycle = trb_d3 & 1;
  printk(LOG_WARN,
         "event_ring: dequeue=%d hc_dequeue=%d ERDP_EHB=%d peek TRB cycle=%d "
         "(CCS=%d) match=%d\n",
         event_ring_dequeue, hc_dequeue, (erdp_lo & ERDP_EHB) ? 1 : 0,
         trb_cycle, event_ring_ccs & 1, trb_cycle == (event_ring_ccs & 1));

  // Software CCS flip count vs HC lap count. The HC flips its cycle bit each
  // time it wraps the ring; software should flip CCS exactly the same number of
  // times. To infer the HC's lap count, scan all 256 event TRBs and count how
  // many carry the *current* CCS cycle vs the opposite: a freshly-written lap
  // has cycle==CCS, the stale previous lap has cycle!=CCS. With a single
  // segment the HC writes strictly in order, so the boundary between
  // "opposite-cycle" (old) and "same-cycle" (new) slots marks where the HC's
  // producer currently is. ccs_flips should equal the HC lap count; a mismatch
  // is direct proof the consumer flipped CCS at the wrong point.
  {
    int same = 0, opp = 0;
    for (int i = 0; i < 256; i++) {
      uint32_t c = ering[i * 4 + 3] & 1;
      if (c == (event_ring_ccs & 1))
        same++;
      else
        opp++;
    }
    printk(LOG_WARN, "ccs_flips(sw)=%d  ring cycle: same-as-CCS=%d opp=%d\n",
           event_ring_ccs_flips, same, opp);
  }

  // EP1-IN transfer ring state: the producer ring the HC drains to fetch IN
  // transfers. enqueue/ccs are the software producer pointers; the slot at
  // `enqueue` is the next TRB the HC has not yet consumed. If enqueue is near
  // 254/255 the ring has nearly lapped and the Link TRB at slot 255 (whose
  // cycle bit is set once at init and never updated) may be mis-gating the HC.
  // Also dump the Link TRB itself and the TRB the HC is currently sitting on.
  volatile uint32_t *xr = (volatile uint32_t *)xhci_intrs[0].ring_virt;
  {
    int enq = xhci_intrs[0].enqueue;
    int xccs = xhci_intrs[0].ccs & 1;
    uint32_t link_d3 = xr[255 * 4 + 3];
    uint32_t enq_d3 = xr[enq * 4 + 3];
    printk(LOG_WARN,
           "ep1_ring: enqueue=%d ccs=%d  link255[type=%d TC=%d cycle=%d]  "
           "slot%d[cycle=%d]\n",
           enq, xccs, (link_d3 >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK,
           (link_d3 >> 1) & 1, link_d3 & 1, enq, enq_d3 & 1);
  }

  // HC's EP1-IN TR dequeue pointer, read from the Output Device Context the HC
  // maintains. Output ctx = Slot(dword0-7) + EP contexts (8 dwords each, DCI n
  // at dword 8+(n-1)*8). EP1-IN is DCI 3 → dword 24; its TR Dequeue Pointer is
  // dword 26/27 (lo/hi), low bit = DCS (Dequeue Cycle State). Comparing the HC
  // dequeue slot against the software enqueue tells us whether the HC stalled
  // with pending TRBs (enqueue != hc_deq) or simply has nothing to consume
  // (enqueue == hc_deq, ring empty).
  if (xhci_intrs[0].slot_id != 0 && dev_ctx_phys != 0) {
    volatile uint32_t *octx =
        (volatile uint32_t *)phys_to_virt((__force phys_addr_t)dev_ctx_phys);
    uint32_t tr_lo = octx[26];
    uint32_t tr_hi = octx[27];
    uint64_t tr = ((uint64_t)tr_hi << 32) | (tr_lo & ~0xFu);
    int hc_deq = (tr == 0) ? -1 : (int)((tr - xhci_intrs[0].ring_phys) / 16);
    int hc_dcs = tr_lo & 1;
    int sw_enq = xhci_intrs[0].enqueue;
    // Peek the cycle bits around the HC dequeue pointer: the slot the HC is
    // about to consume must carry cycle==DCS, else the HC stalls. Dump the
    // AT-dequeue slot and one ahead so we can see exactly where the mismatch
    // is.
    int d0 = (hc_deq >= 0 && hc_deq < 256) ? (int)(xr[hc_deq * 4 + 3] & 1) : -1;
    int d1 = (hc_deq >= 0 && hc_deq + 1 < 256)
                 ? (int)(xr[(hc_deq + 1) * 4 + 3] & 1)
                 : -1;
    // EP1-IN endpoint state from Output Context dword 24 bits[0:2]:
    // 0=disabled, 1=running, 2=halted (set by HC on transfer error), 3=stopped.
    // If the EP is halted, the HC won't consume TRBs until software resets it
    // (Stop Endpoint + Reset Endpoint + Restart). This is the likely reason a
    // pending TRB with a matching cycle bit still isn't consumed.
    uint32_t ep_dw0 = octx[24];
    int ep_state = ep_dw0 & 0x7;
    printk(LOG_WARN,
           "hc_ep1: dequeue_slot=%d DCS=%d  sw_enqueue=%d  pending=%d  "
           "slot%d[cyc=%d] slot%d[cyc=%d]  EPstate=%d\n",
           hc_deq, hc_dcs, sw_enq,
           (hc_deq >= 0) ? ((sw_enq - hc_deq + 256) % 256) : -1, hc_deq, d0,
           hc_deq + 1, d1, ep_state);
  }

  // LAPIC TMR/IRR/ISR for the vector: 256-bit bitmasks, 32 per 32-bit reg.
  // Reg base: ISR 0x100, TMR 0x180, IRR 0x200; vector V -> reg (V/32)*0x10,
  // bit (V%32). A set IRR bit = pending in LAPIC but not yet delivered; a set
  // ISR bit = currently in-service (CPU is inside the ISR).
  uint32_t tmr = lapic_read(0x180 + (vec / 32) * 0x10);
  uint32_t irr = lapic_read(0x200 + (vec / 32) * 0x10);
  uint32_t isr = lapic_read(0x100 + (vec / 32) * 0x10);
  uint32_t vbit = 1u << (vec % 32);
  printk(LOG_WARN,
         "LAPIC vec%d: TMR bit=%d (0=edge 1=level) IRR bit=%d (pending in "
         "LAPIC) ISR bit=%d (in-service)\n",
         vec, (tmr & vbit) != 0, (irr & vbit) != 0, (isr & vbit) != 0);

  // LAPIC Error Status Register: a non-zero ESR means the LAPIC rejected an
  // incoming interrupt (illegal vector, receive/redirect errors). If the HC is
  // firing MSIs at a bogus address/vector, ESR may record it. Write 0 first to
  // clear, then read back (ESR is read-after-clear).
  lapic_write(0x280, 0); // clear ESR
  uint32_t esr = lapic_read(0x280);
  printk(LOG_WARN, "LAPIC ESR=0x%x (illegal-vec/redirect errors, if any)\n",
         esr);

  // Last successful ISR snapshot: shows what the most recent xhci_isr saw at
  // entry and left behind at exit. If ccs/dequeue look wrong here, the ISR
  // mis-handled the event ring cycle flip on its last run, leaving the HC with
  // an event it can never get the software to consume.
  printk(LOG_WARN,
         "last_isr: count=%d iman_in=0x%x usbsts_in=0x%x consumed=%d "
         "exit_dequeue=%d exit_ccs=%d\n",
         last_isr.count, last_isr.iman_in, last_isr.usbsts, last_isr.consumed,
         last_isr.dequeue, last_isr.ccs);
  printk(LOG_WARN, "last_ep1_xfer: cc=%d len=%d (CC_SUCCESS=1)\n",
         last_isr.last_cc, last_isr.last_len);

  // Active probe: manually clear IMAN.IP now. If the next poll sees IP set
  // again, the HC is still asserting (real pending event the ISR never drained)
  // — the interrupt delivery is stuck at the HC. If IP stays clear, the HC had
  // a one-shot pending that we just cleared, and the real problem is the MSI
  // never reached the LAPIC in the first place (delivery path).
  intr_write(0, XHCI_IMAN, iman | IMAN_IP | IMAN_IE);
  uint32_t iman_after = intr_read(0, XHCI_IMAN);
  printk(LOG_WARN,
         "manual IP clear: iman before=0x%x after=0x%x (IP still set=%d)\n",
         iman, iman_after, (iman_after & IMAN_IP) ? 1 : 0);

  // IDT gate / handler registration for the vector. irq_handlers[] is static in
  // trap.c; use the public irq_has_handler() predicate (keys the stub
  // dispatch). irq_owner < 0 = not bound to a user-space driver (which would
  // reroute the IRQ to a RECV_IRQ path and bypass the kernel handler).
  printk(LOG_WARN, "irq_has_handler(%d)=%d irq_owner[%d]=%d\n", vec,
         irq_has_handler(vec), vec,
         (int)__atomic_load_n(&irq_owner[vec], __ATOMIC_ACQUIRE));

  uint64_t flags;
  __asm__ volatile("pushfq; popq %0" : "=r"(flags));
  printk(LOG_WARN, "RFLAGS.IF=%d (CPU interrupt enable)\n",
         (int)((flags >> 9) & 1));
  printk(LOG_WARN, "=== end dump ===\n");
}

void xhci_poll() {
  if (!xhci_poll_initialized)
    return;
  xhci_poll_count++;
  uint32_t iman = intr_read(0, XHCI_IMAN);
  if (iman & IMAN_IP) {
    xhci_ip_stuck_ticks++;
    // Dump only after sustained IP-stuck (skip boot spurious), and rate-limit
    // repeats so we can re-capture if the state later changes.
    if (xhci_ip_stuck_ticks >= XHCI_IP_STUCK_DUMP_THRESHOLD &&
        xhci_poll_count - xhci_ip_last_dump_tick >=
            XHCI_IP_STUCK_DUMP_THRESHOLD) {
      xhci_ip_last_dump_tick = xhci_poll_count;
      xhci_dump_int_state(iman);
    }
  } else {
    xhci_ip_stuck_ticks = 0;
  }

  // Ring the EP1-IN doorbell every poll. QEMU's xHCI only retries a NAK'ed
  // interrupt-IN transfer on a doorbell write (not on timer expiry), so without
  // this a key press that arrives while the previous transfer is NAK'ing would
  // never be delivered.
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
