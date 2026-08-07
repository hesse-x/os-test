/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/driver/ahci.h"
#include "arch/x64/apic.h"
#include "arch/x64/memlayout.h" // KERNEL_VMA_BOUNDARY
#include "arch/x64/paging.h"
#include "arch/x64/smp.h"
#include "arch/x64/trap.h"
#include "arch/x64/utils.h"
#include "kernel/driver/pci.h"
#include "kernel/xcore/acpi.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/mem/alloc.h"
#include "kernel/xcore/mem/kasan.h"
#include "kernel/xcore/perf/event.h"
#include "kernel/xcore/sched.h"
#include "kernel/xcore/sparse.h"
#include "kernel/xcore/spinlock.h"
#include "kernel/xcore/trap.h"
#include "kernel/xcore/xtask.h"
#include "xos/perf.h"
#include <stdbool.h>
#include <xos/errno.h>
#include <xos/syscall_nums.h>

// ===================== AHCI register offsets (from ABAR) =====================
// Global HBA registers
#define AHCI_CAP 0x00
#define AHCI_GHC 0x04 // HR=bit0, IE=bit1, AE=bit31
#define AHCI_IS 0x08
#define AHCI_PI 0x0C // Ports Implemented bitmap
#define AHCI_VS 0x10

// Port register offsets (from port base = ABAR + 0x100 + port*0x80)
#define PxCLB 0x00
#define PxCLBU 0x04
#define PxFB 0x08
#define PxFBU 0x0C
#define PxIS 0x10 // DHRS=bit0, TFES=bit30
#define PxIE 0x14
#define PxCMD                                                                  \
  0x18 // ST=bit0, SUD=bit1, POD=bit2, CLO=bit3, FRE=bit4, FR=bit14, CR=bit15
#define PxTFD 0x20
#define PxSIG 0x24
#define PxSSTS 0x28 // DET=bits0:3 (3=device present+PHY)
#define PxSCTL 0x2C
#define PxSERR 0x30
#define PxSACT 0x34
#define PxCI 0x38

// Command constants
#define CMD_READ_DMA_EXT 0x25
#define CMD_WRITE_DMA_EXT 0x34
#define CMD_IDENTIFY_DEVICE 0xEC
#define FIS_H2D 0x27
#define FIS_H2D_CMD 0x80

#define AHCI_PORT_IRQ_MASK ((1U << 0) | (1U << 29) | (1U << 30) | (1U << 31))

// Bounce buffer: 16 pages = 64KB
#define AHCI_BOUNCE_PAGES 16

// ===================== Module state =====================
static void __iomem *abar;
static int active_port = -1;
static uint64_t active_sector_count;

static spinlock ahci_queue_lock = SPINLOCK_INIT;

static struct page *cmd_list_page;
static struct page *fis_recv_page;
static struct page *cmd_table_page;
static struct page *bounce_page;

static uint64_t cmd_list_phys;
static void *cmd_list_virt;
static uint64_t fis_recv_phys;
static void *fis_recv_virt;
static uint64_t cmd_table_phys;
static void *cmd_table_virt;
static uint64_t bounce_phys;
static void *bounce_virt;

// ===================== Block request queue (async I/O) =====================
#define BLOCK_QUEUE_SIZE 32

typedef struct block_req {
  pid_t caller_pid;
  uint32_t lba;
  uint32_t count;  // sector count (1..AHCI_MAX_SECTORS)
  uint8_t dir;     // 0=read, 1=write
  void *user_buf;  // user-space virtual address
  uint32_t cookie; // monotonic ID for completion matching
  int result;      // 0=ok, EIO=error
  bool kernel_wait;
  void *kernel_buf;
  struct ahci_wait_ctx *wait;
  uint64_t enqueue_tsc;
  uint64_t issue_tsc;
  int16_t submit_cpu;
  uint8_t staging[AHCI_MAX_SECTORS * 512];
} block_req;

struct ahci_wait_ctx {
  volatile bool done;
  int result;
  xtask *waiter;
};

static block_req block_pool[BLOCK_QUEUE_SIZE];
static int bq_head = 0;                    // next slot to dequeue
static int bq_tail = 0;                    // next slot to enqueue
static int bq_count = 0;                   // number of queued requests
static block_req *ahci_current_req = NULL; // in-flight request
static uint32_t ahci_cookie_counter = 0;
static struct ahci_stats ahci_stats;
#ifdef PERF
static uint32_t ahci_stats_sequence;
#endif

static unsigned timing_bucket(uint64_t cycles) {
  unsigned bucket = 0;
  while (cycles > 1 && bucket + 1 < AHCI_TIMING_BUCKETS) {
    cycles >>= 1;
    bucket++;
  }
  return bucket;
}

static void update_max(uint64_t *value, uint64_t sample) {
  uint64_t old = __atomic_load_n(value, __ATOMIC_RELAXED);
  while (sample > old &&
         !__atomic_compare_exchange_n(value, &old, sample, true,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

#ifdef PERF
static void record_irq_stage(enum ahci_irq_stage stage, uint64_t begin,
                             uint64_t end) {
  if (end < begin) {
    ahci_stats.invalid_timing++;
    return;
  }
  uint64_t cycles = end - begin;
  struct ahci_stage_stats *stats = &ahci_stats.irq_stage[stage];
  stats->count++;
  stats->cycles += cycles;
  update_max(&stats->max, cycles);
  stats->hist[timing_bucket(cycles)]++;
}

static void record_irq_handler(uint64_t begin, uint64_t end) {
  if (end < begin) {
    ahci_stats.invalid_timing++;
    return;
  }
  uint64_t cycles = end - begin;
  ahci_stats.irq_handler_count++;
  ahci_stats.irq_handler_cycles += cycles;
  update_max(&ahci_stats.irq_handler_max, cycles);
  ahci_stats.irq_handler_hist[timing_bucket(cycles)]++;
}

static void record_irq_long_tail(const uint64_t stage_cycles[7],
                                 uint64_t handler_cycles) {
  uint64_t thresholds[2] = {tsc_freq / 10000U, tsc_freq / 1000U};
  for (unsigned threshold = 0; threshold < 2; threshold++) {
    if (!thresholds[threshold] || handler_cycles < thresholds[threshold])
      continue;
    ahci_stats.long_tail_count[threshold]++;
    for (unsigned stage = 0; stage < 7; stage++) {
      ahci_stats.long_tail_stage_cycles[threshold][stage] +=
          stage_cycles[stage];
      update_max(&ahci_stats.long_tail_stage_max[threshold][stage],
                 stage_cycles[stage]);
    }
  }
}
#endif

static uint32_t next_cookie_locked(void) {
  ahci_cookie_counter = (ahci_cookie_counter + 1U) & 0x7fffffffU;
  if (!ahci_cookie_counter)
    ahci_cookie_counter = 1;
  return ahci_cookie_counter;
}

static void queue_depth_change_locked(uint32_t new_depth, uint64_t now) {
  uint64_t last = ahci_stats.queue_depth_last_tsc;
  if (last && now >= last)
    ahci_stats.queue_depth_cycles +=
        (now - last) * (uint64_t)ahci_stats.queue_depth;
  else if (last && now < last)
    ahci_stats.invalid_timing++;
  ahci_stats.queue_depth_last_tsc = now;
  ahci_stats.queue_depth = new_depth;
  if (new_depth > ahci_stats.queue_depth_max)
    ahci_stats.queue_depth_max = new_depth;
}

void ahci_get_stats(struct ahci_stats *out) {
  if (!out)
    return;
  const uint64_t *source = (const uint64_t *)&ahci_stats;
  uint64_t *dest = (uint64_t *)out;
#ifdef PERF
  for (;;) {
    uint32_t before = __atomic_load_n(&ahci_stats_sequence, __ATOMIC_ACQUIRE);
    if (before & 1U)
      continue;
    for (size_t i = 0; i < sizeof(*out) / sizeof(uint64_t); i++)
      dest[i] = __atomic_load_n(&source[i], __ATOMIC_RELAXED);
    uint32_t after = __atomic_load_n(&ahci_stats_sequence, __ATOMIC_ACQUIRE);
    if (before == after && !(after & 1U))
      break;
  }
#else
  for (size_t i = 0; i < sizeof(*out) / sizeof(uint64_t); i++)
    dest[i] = __atomic_load_n(&source[i], __ATOMIC_RELAXED);
#endif
  uint64_t now = rdtsc64();
  if (out->queue_depth_last_tsc && now >= out->queue_depth_last_tsc)
    out->queue_depth_cycles +=
        (now - out->queue_depth_last_tsc) * out->queue_depth;
  else if (out->queue_depth_last_tsc && now < out->queue_depth_last_tsc)
    out->invalid_timing++;
}

// ===================== Helpers =====================
static inline void __iomem *port_reg(int port, uint32_t offset) {
  return (void __iomem *)((uint8_t __iomem *)abar + 0x100 + port * 0x80 +
                          offset);
}

uint64_t ahci_sector_count(void) {
  return __atomic_load_n(&active_sector_count, __ATOMIC_ACQUIRE);
}

static void ahci_puts(const char *s) { (void)s; }

// ===================== Page-table walk: bounce → user pages
// ===================== Walk target process's page tables from proc->cr3
// (physical address) to find physical pages backing the user buffer, then copy
// bounce buffer data via kernel higher-half mapping. Safe in IRQ context (no
// CR3 switch).

static uint64_t walk_user_pt(uint64_t cr3_phys, uint64_t vaddr) {
  uint64_t *pml4 =
      (__force uint64_t *)phys_to_virt((__force phys_addr_t)cr3_phys);
  int pml4_idx = (vaddr >> 39) & 0x1FF;
  if (!(pml4[pml4_idx] & PTE_PRESENT))
    return 0;

  uint64_t *pdpt = (__force uint64_t *)phys_to_virt(
      (__force phys_addr_t)(pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL));
  int pdpt_idx = (vaddr >> 30) & 0x1FF;
  if (!(pdpt[pdpt_idx] & PTE_PRESENT))
    return 0;
  if (pdpt[pdpt_idx] & PTE_PS) // 1GB huge page
    return (pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL) + (vaddr & 0x3FFFFFFF);

  uint64_t *pd = (__force uint64_t *)phys_to_virt(
      (__force phys_addr_t)(pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL));
  int pd_idx = (vaddr >> 21) & 0x1FF;
  if (!(pd[pd_idx] & PTE_PRESENT))
    return 0;
  if (pd[pd_idx] & PTE_PS) // 2MB huge page
    return (pd[pd_idx] & 0x000FFFFFFFFFF000ULL) + (vaddr & 0x1FFFFF);

  uint64_t *pt = (__force uint64_t *)phys_to_virt(
      (__force phys_addr_t)(pd[pd_idx] & 0x000FFFFFFFFFF000ULL));
  int pt_idx = (vaddr >> 12) & 0x1FF;
  if (!(pt[pt_idx] & PTE_PRESENT))
    return 0;
  return (pt[pt_idx] & 0x000FFFFFFFFFF000ULL) + (vaddr & 0xFFF); // 4KB page
}

// Copy bounce buffer data to a user process's buffer via page-table walk.
// Returns true on success, false if page walk failed.
__attribute__((no_sanitize("kernel-address"))) static bool
bounce_to_user_pages(pid_t pid, void *user_buf, uint32_t byte_len) {
  if (pid < 0 || pid >= MAX_PROC)
    return false;
  xtask *proc = task_get(pid);
  if (proc->pid != pid || proc->state == ZOMBIE || proc->state == REAPING)
    return false;

  uint64_t cr3 = proc->cr3; // physical address of PML4
  uint64_t dst_va = (uint64_t)user_buf;
  uint32_t offset = 0;

  while (offset < byte_len) {
    uint64_t page_va = (dst_va + offset) & ~0xFFFULL; // ALIGN_DOWN to page
    uint64_t page_offset = (dst_va + offset) - page_va;
    uint32_t remaining = byte_len - offset;
    uint32_t chunk = remaining < (4096 - page_offset)
                         ? remaining
                         : (4096 - (uint32_t)page_offset);

    uint64_t phys = walk_user_pt(cr3, page_va);
    if (phys == 0)
      return false;

    __memcpy((void __force *)((__force uint64_t)phys_to_virt(
                                  (__force phys_addr_t)phys) +
                              page_offset),
             (const void *)((uint8_t *)bounce_virt + offset), chunk);
    offset += chunk;
  }
  return true;
}

// ===================== DMA allocation =====================
static void ahci_alloc_dma() {
  cmd_list_page = bfc_alloc_page_low(1);
  fis_recv_page = bfc_alloc_page_low(1);
  cmd_table_page = bfc_alloc_page_low(1);
  bounce_page = bfc_alloc_page_low(AHCI_BOUNCE_PAGES);

  if (!cmd_list_page || !fis_recv_page || !cmd_table_page || !bounce_page) {
    ahci_puts("ahci: DMA alloc failed\n");
    halt();
  }

  cmd_list_phys = (__force uint64_t)page_to_phys(cmd_list_page);
  fis_recv_phys = (__force uint64_t)page_to_phys(fis_recv_page);
  cmd_table_phys = (__force uint64_t)page_to_phys(cmd_table_page);
  bounce_phys = (__force uint64_t)page_to_phys(bounce_page);

  cmd_list_virt =
      (__force void *)phys_to_virt((__force phys_addr_t)cmd_list_phys);
  fis_recv_virt =
      (__force void *)phys_to_virt((__force phys_addr_t)fis_recv_phys);
  cmd_table_virt =
      (__force void *)phys_to_virt((__force phys_addr_t)cmd_table_phys);
  bounce_virt = (__force void *)phys_to_virt((__force phys_addr_t)bounce_phys);

  __memset((void *)cmd_list_virt, 0, 4096);
  __memset((void *)fis_recv_virt, 0, 4096);
  __memset((void *)cmd_table_virt, 0, 4096);
  __memset((void *)bounce_virt, 0, AHCI_BOUNCE_PAGES * 4096);
}

// ===================== Port interrupt disable =====================
// Disable port-level interrupts and clear pending status.
// Used when switching ports, shutting down, or as a safety guard.
static void port_disable_interrupts(int port) {
  writel(port_reg(port, PxIE), 0);
  writel(port_reg(port, PxSERR), readl(port_reg(port, PxSERR)));
  writel(port_reg(port, PxIS), 0xFFFFFFFF);
}

static void port_enable_interrupts(int port) {
  writel(port_reg(port, PxIS), 0xFFFFFFFF);
  writel((void __iomem *)((uint8_t __iomem *)abar + AHCI_IS), 1U << port);
  writel(port_reg(port, PxIE), AHCI_PORT_IRQ_MASK);
}

// ===================== Port stop =====================
static void port_stop(int port) {
  void __iomem *cmd = port_reg(port, PxCMD);

  // Clear ST, wait for CR=0
  writel(cmd, readl(cmd) & ~(uint32_t)1);
  for (int i = 0; i < 500000; i++) {
    if (!(readl(cmd) & (1 << 15)))
      break;
  }

  // Clear FRE, wait for FR=0
  writel(cmd, readl(cmd) & ~(uint32_t)(1 << 4));
  for (int i = 0; i < 500000; i++) {
    if (!(readl(cmd) & (1 << 14)))
      break;
  }

  // Disable interrupts and clear pending status
  port_disable_interrupts(port);
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

  // Do NOT enable PxIE here — only the active port should have interrupts
  // enabled (done after ahci_init determines active_port). Enabling PxIE
  // on all probed ports causes a storm of spurious MSI from idle devices.

  // Enable FIS receive: set FRE, wait for FR=1
  void __iomem *cmd = port_reg(port, PxCMD);
  writel(cmd, readl(cmd) | (1 << 4));
  for (int i = 0; i < 500000; i++) {
    if (readl(cmd) & (1 << 14))
      break;
  }

  // Start port: set ST, wait for CR=1
  writel(cmd, readl(cmd) | 1);
  for (int i = 0; i < 500000; i++) {
    if (readl(cmd) & (1 << 15))
      break;
  }
}

// ===================== IDENTIFY DEVICE =====================
static int ahci_identify_device(int port, void *buf) {
  // Build Command Table: zero FIS area
  __memset((void *)cmd_table_virt, 0, 0x80);

  // H2D Register FIS: IDENTIFY DEVICE (0xEC)
  uint8_t *fis = (uint8_t *)cmd_table_virt;
  fis[0] = FIS_H2D;
  fis[1] = FIS_H2D_CMD;
  fis[2] = CMD_IDENTIFY_DEVICE;
  fis[3] = 0x00;  // Features (low)
  fis[4] = 0x00;  // LBA[0:7]
  fis[5] = 0x00;  // LBA[8:15]
  fis[6] = 0x00;  // LBA[16:23]
  fis[7] = 0x00;  // Device: no LBA bit per spec
  fis[8] = 0x00;  // LBA[24:31]
  fis[9] = 0x00;  // LBA[32:39]
  fis[10] = 0x00; // LBA[40:47]
  fis[11] = 0x00; // Features (high)
  fis[12] = 0x00; // Sector count low
  fis[13] = 0x00; // Sector Count high

  // PRD: point to bounce buffer, dbc=511 (512 bytes - 1)
  uint32_t *prd = (uint32_t *)((uint8_t *)cmd_table_virt + 0x80);
  prd[0] = (uint32_t)(bounce_phys & 0xFFFFFFFF);
  prd[1] = (uint32_t)((bounce_phys >> 32) & 0xFFFFFFFF);
  prd[2] = 0;
  prd[3] = (511 & 0x3FFFFF) | (1U << 31); // DBC=511 + IOC

  // Command Header (slot 0)
  uint32_t *hdr = (uint32_t *)cmd_list_virt;
  hdr[0] = (5 << 0) | (1 << 16); // CFL=5 DW, PRDTL=1
  hdr[1] = 0;
  hdr[2] = (uint32_t)(cmd_table_phys & 0xFFFFFFFF);
  hdr[3] = (uint32_t)((cmd_table_phys >> 32) & 0xFFFFFFFF);
  hdr[4] = 0;
  hdr[5] = 0;
  hdr[6] = 0;
  hdr[7] = 0;

  // Clear port interrupt status and issue command
  writel(port_reg(port, PxIS), 0xFFFFFFFF);
  writel(port_reg(port, PxCI), 1);

  // Poll until PxCI bit 0 clears
  for (int i = 0; i < 10000000; i++) {
    if (!(readl(port_reg(port, PxCI)) & 1))
      goto done;
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
// Completes the current async request, notifies caller, issues next queued
// request. EOI is issued early (after clearing PxIS + IS) so that
// higher-priority interrupts (e.g. LAPIC timer) can preempt the handler's work.

void ahci_issue_cmd(block_req *req); // forward declaration

static void ahci_irq_handler(trapframe *tf) {
#ifdef PERF
  uint64_t t0 = rdtsc64();
#endif
  // Check if our port generated the interrupt
  uint32_t pxis = readl(port_reg(active_port, PxIS));
  if (pxis == 0) {
    // Spurious interrupt from a non-active port — safety guard:
    // scan all PI ports and disable any with PxIE still set (shouldn't
    // happen, but prevents interrupt storms if PxIE leaks).
    uint32_t pi = readl((void __iomem *)((uint8_t __iomem *)abar + AHCI_PI));
    for (int i = 0; i < 32; i++) {
      if ((pi & (1 << i)) && i != active_port) {
        uint32_t ie = readl(port_reg(i, PxIE));
        if (ie)
          port_disable_interrupts(i);
      }
    }
    // Acknowledge global IS and EOI
    writel((void __iomem *)((uint8_t __iomem *)abar + AHCI_IS),
           readl((void __iomem *)((uint8_t __iomem *)abar + AHCI_IS)));
    lapic_eoi();
#ifdef PERF
    uint64_t end = rdtsc64();
    __atomic_fetch_add(&ahci_stats_sequence, 1, __ATOMIC_SEQ_CST);
    ahci_stats.spurious_count++;
    ahci_stats.spurious_cycles += end - t0;
    __atomic_fetch_add(&ahci_stats_sequence, 1, __ATOMIC_SEQ_CST);
#endif
    return;
  }

  // Acknowledge port interrupt status
  writel(port_reg(active_port, PxIS), pxis);
  // Acknowledge global IS
  writel((void __iomem *)((uint8_t __iomem *)abar + AHCI_IS),
         readl((void __iomem *)((uint8_t __iomem *)abar + AHCI_IS)));

  // EOI early — allows LAPIC timer and other high-priority interrupts
  // to preempt our completion processing
  lapic_eoi();
#ifdef PERF
  uint64_t t1 = rdtsc64();
#endif

  // No sti here. This is a hard-IRQ handler: every IDT gate is an interrupt
  // gate (IF=0 on entry), and hard-IRQs must stay non-reentrant. Re-enabling
  // interrupts would let the LAPIC timer nest into the AHCI completion path,
  // multiplying stack depth on the shared task stack (the #DF root cause).
  // Completion processing does not block (synchronous bounce-buffer copy), so
  // IF=0 throughout is correct. EOI ≠ sti: the early lapic_eoi() above already
  // lets the LAPIC latch the next interrupt, which is delivered once we IRETQ
  // with IF=1.

  // Check if a command was in flight
  spin_lock(&ahci_queue_lock);
#ifdef PERF
  uint64_t t2 = rdtsc64();
#endif
  if (!ahci_current_req) {
    spin_unlock(&ahci_queue_lock);
#ifdef PERF
    uint64_t end = rdtsc64();
    __atomic_fetch_add(&ahci_stats_sequence, 1, __ATOMIC_SEQ_CST);
    ahci_stats.orphan_count++;
    ahci_stats.orphan_cycles += end - t0;
    __atomic_fetch_add(&ahci_stats_sequence, 1, __ATOMIC_SEQ_CST);
#endif
    return;
  }

  // Check for error (TFES = bit30)
  bool error = (pxis & (1U << 30));
  uint64_t complete_tsc = rdtsc64();
  if (complete_tsc >= ahci_current_req->issue_tsc) {
    uint64_t service = complete_tsc - ahci_current_req->issue_tsc;
    ahci_stats.service_count++;
    ahci_stats.service_cycles += service;
    update_max(&ahci_stats.service_max, service);
    ahci_stats.service_hist[timing_bucket(service)]++;
  } else {
    ahci_stats.invalid_timing++;
  }
#ifdef PERF
  uint64_t t3 = rdtsc64();
#endif

  // For reads: copy bounce buffer data to user buffer via page-table walk
  if (ahci_current_req->dir == 0 && !error && ahci_current_req->kernel_wait) {
    __memcpy(ahci_current_req->kernel_buf, (const void *)bounce_virt,
             (size_t)ahci_current_req->count * 512);
  } else if (ahci_current_req->dir == 0 && !error) {
    uint32_t byte_len = ahci_current_req->count * 512;
    bool ok = bounce_to_user_pages(ahci_current_req->caller_pid,
                                   ahci_current_req->user_buf, byte_len);
    if (!ok)
      error = true; // page walk failed = I/O error
  }
#ifdef PERF
  uint64_t t4 = rdtsc64();
#endif

  ahci_current_req->result = error ? EIO : 0;

  // Build RECV_NOTIFY completion message
  recv_msg msg;
  msg.type = RECV_NOTIFY;
  msg.src = 0; // kernel disk completion
  __memset(msg.data, 0, 56);
  // Pack: cookie(4) + result(4) + lba(4) + count(4)
  __memcpy(msg.data, &ahci_current_req->cookie, 4);
  __memcpy(msg.data + 4, &ahci_current_req->result, 4);
  __memcpy(msg.data + 8, &ahci_current_req->lba, 4);
  __memcpy(msg.data + 12, &ahci_current_req->count, 4);

  pid_t caller = ahci_current_req->caller_pid;
  bool kernel_wait = ahci_current_req->kernel_wait;
  struct ahci_wait_ctx *wait = ahci_current_req->wait;
  uint32_t perf_cookie = 0x80000000U | ahci_current_req->cookie;
  perf_trace_causal(XOS_PERF_TRACE_IO, XOS_PERF_IO_COMPLETE, perf_cookie);
  ahci_stats.completed++;
  if (error)
    ahci_stats.errors++;
  ahci_current_req = NULL;
  queue_depth_change_locked((uint32_t)(bq_count - 1), complete_tsc);
  bq_count--;
  bq_head = (bq_head + 1) % BLOCK_QUEUE_SIZE;

  if (kernel_wait) {
    wait->result = error ? -EIO : 0;
    __atomic_store_n(&wait->done, true, __ATOMIC_RELEASE);
    if (wait->waiter) {
      wake_with_event(wait->waiter, WAIT_BLOCK_IO);
      ahci_stats.sync_wakes++;
      if (wait->waiter->assigned_cpu != (int)get_cpu_local()->cpu_id)
        ahci_stats.cross_cpu_wakes++;
      perf_trace_causal(XOS_PERF_TRACE_IO, XOS_PERF_IO_WAKE, perf_cookie);
    } else {
      ahci_stats.early_completes++;
      perf_trace_causal(XOS_PERF_TRACE_IO, XOS_PERF_IO_RESUME, perf_cookie);
    }
  } else {
    notify_and_wake(caller, &msg);
    ahci_stats.async_wakes++;
    perf_trace_causal(XOS_PERF_TRACE_IO, XOS_PERF_IO_WAKE, perf_cookie);
  }
#ifdef PERF
  uint64_t t5 = rdtsc64();
#endif

  // Issue next queued request if available
  if (bq_count > 0) {
    ahci_current_req = &block_pool[bq_head];
    ahci_issue_cmd(ahci_current_req);
  } else {
    // No request owns slot 0 or the shared bounce buffer. The next submit
    // re-enables PxIE immediately before ringing the doorbell.
    port_disable_interrupts(active_port);
    writel((void __iomem *)((uint8_t __iomem *)abar + AHCI_IS),
           1U << active_port);
  }
#ifdef PERF
  uint64_t t6 = rdtsc64();
#endif
  spin_unlock(&ahci_queue_lock);
#ifdef PERF
  uint64_t t8 = rdtsc64();
  __atomic_fetch_add(&ahci_stats_sequence, 1, __ATOMIC_SEQ_CST);
  record_irq_stage(AHCI_IRQ_ACK, t0, t1);
  record_irq_stage(AHCI_IRQ_LOCK_WAIT, t1, t2);
  record_irq_stage(AHCI_IRQ_BOOKKEEPING, t2, t3);
  record_irq_stage(AHCI_IRQ_COPY, t3, t4);
  record_irq_stage(AHCI_IRQ_WAKE, t4, t5);
  record_irq_stage(AHCI_IRQ_NEXT_SUBMIT, t5, t6);
  record_irq_stage(AHCI_IRQ_UNLOCK_EXIT, t6, t8);
  record_irq_stage(AHCI_IRQ_LOCKED_TOTAL, t2, t6);
  record_irq_handler(t0, t8);
  const uint64_t stage_cycles[7] = {t1 - t0, t2 - t1, t3 - t2, t4 - t3,
                                    t5 - t4, t6 - t5, t8 - t6};
  record_irq_long_tail(stage_cycles, t8 - t0);
  __atomic_fetch_add(&ahci_stats_sequence, 1, __ATOMIC_SEQ_CST);
#endif
}

// ===================== Command issue with memory barrier =====================
// Issue command slot 0 on the active port. Must be called after command table
// and header are fully written. Uses MFENCE to ensure memory writes are visible
// to the HBA before the PxCI doorbell write.
static inline void ahci_issue_command() {
  // Ensure all command table and header writes are visible before PxCI
  __sync_synchronize(); // full memory barrier (MFENCE on x86-64)
  writel(port_reg(active_port, PxCI), 1);
}

// ===================== ahci_issue_cmd =====================
// Build FIS + PRD + command header and issue PxCI=1 (no polling).
// For writes: bounce buffer must already contain the data (done in syscall
// context).

void ahci_issue_cmd(block_req *req) {
  req->issue_tsc = rdtsc64();
  if (req->issue_tsc >= req->enqueue_tsc) {
    uint64_t wait = req->issue_tsc - req->enqueue_tsc;
    ahci_stats.queue_wait_count++;
    ahci_stats.queue_wait_cycles += wait;
    update_max(&ahci_stats.queue_wait_max, wait);
    ahci_stats.queue_wait_hist[timing_bucket(wait)]++;
  } else {
    ahci_stats.invalid_timing++;
  }
  uint32_t lba = req->lba;
  uint32_t chunk = req->count;
  uint8_t cmd_byte = (req->dir == 0) ? CMD_READ_DMA_EXT : CMD_WRITE_DMA_EXT;

  if (req->dir == 1)
    __memcpy((void *)bounce_virt, req->staging, (size_t)chunk * 512);

  // Build Command Table: zero FIS area
  __memset((void *)cmd_table_virt, 0, 0x80);

  // H2D Register FIS
  uint8_t *fis = (uint8_t *)cmd_table_virt;
  fis[0] = FIS_H2D;     // FIS type
  fis[1] = FIS_H2D_CMD; // D2H register FIS with interrupt
  fis[2] = cmd_byte;
  fis[3] = 0x00; // Features (low)
  fis[4] = (uint8_t)(lba & 0xFF);
  fis[5] = (uint8_t)((lba >> 8) & 0xFF);
  fis[6] = (uint8_t)((lba >> 16) & 0xFF);
  fis[7] = 0x40; // Device: LBA mode
  fis[8] = (uint8_t)((lba >> 24) & 0xFF);
  fis[9] = 0x00;  // LBA[32:39]
  fis[10] = 0x00; // LBA[40:47]
  fis[11] = 0x00; // Features (high)
  fis[12] = (uint8_t)(chunk & 0xFF);
  fis[13] = (uint8_t)((chunk >> 8) & 0xFF);

  // PRD entry at offset 0x80 in command table
  uint32_t *prd = (uint32_t *)((uint8_t *)cmd_table_virt + 0x80);
  prd[0] = (uint32_t)(bounce_phys & 0xFFFFFFFF);
  prd[1] = (uint32_t)((bounce_phys >> 32) & 0xFFFFFFFF);
  prd[2] = 0;
  prd[3] = ((chunk * 512 - 1) & 0x3FFFFF) | (1U << 31);

  // Command Header (slot 0)
  uint32_t *hdr = (uint32_t *)cmd_list_virt;
  uint32_t cmd_flags = (5 << 0) | (1 << 16); // CFL=5 DW, PRDTL=1
  if (req->dir == 1)
    cmd_flags |= (1 << 6); // W=1 for write
  hdr[0] = cmd_flags;
  hdr[1] = 0;
  hdr[2] = (uint32_t)(cmd_table_phys & 0xFFFFFFFF);
  hdr[3] = (uint32_t)((cmd_table_phys >> 32) & 0xFFFFFFFF);
  hdr[4] = 0;
  hdr[5] = 0;
  hdr[6] = 0;
  hdr[7] = 0;

  // Clear port interrupt status before issuing
  writel(port_reg(active_port, PxIS), 0xFFFFFFFF);

  // Issue command with memory barrier
  ahci_issue_command();
}

// ===================== Port COMRESET =====================
// Force device detection via COMRESET on a port, then wait for DET=3.
// Returns 0 on success, -EIO if device not detected after reset.
static int ahci_comreset_port(int port) {
  // Stop port before reset
  void __iomem *cmd = port_reg(port, PxCMD);
  writel(cmd, readl(cmd) & ~(uint32_t)1); // clear ST
  for (int i = 0; i < 500000; i++) {
    if (!(readl(cmd) & (1 << 15)))
      break; // wait CR=0
  }
  writel(cmd, readl(cmd) & ~(uint32_t)(1 << 4)); // clear FRE
  for (int i = 0; i < 500000; i++) {
    if (!(readl(cmd) & (1 << 14)))
      break; // wait FR=0
  }

  // Issue COMRESET via PxSCTL.DET=1
  writel(port_reg(port, PxSCTL), 1);
  // Wait at least 1ms (approx loop)
  for (int i = 0; i < 100000; i++)
    __asm__ volatile("pause");
  // Clear COMRESET
  writel(port_reg(port, PxSCTL), 0);

  // Wait for device detection (DET=3), up to 100ms
  int det = 0;
  for (int i = 0; i < 5000000; i++) {
    uint32_t ssts = readl(port_reg(port, PxSSTS));
    det = ssts & 0xF;
    if (det == 3)
      break;
    __asm__ volatile("pause");
  }
  printk(LOG_WARN, "ahci: comreset port %d DET=%d\n", port, det);

  writel(port_reg(port, PxSERR), 0xFFFFFFFF);
  writel(port_reg(port, PxIS), 0xFFFFFFFF);
  return (det == 3) ? 0 : -EIO;
}

// ===================== ahci_set_active_port =====================
// Switch the active port while idle. Returns 0 on success, -EIO if port
// has no device. Tries COMRESET if the port doesn't show DET=3 initially.
int ahci_set_active_port(int port) {
  // Check if port has a device; try COMRESET if not detected
  uint32_t ssts = readl(port_reg(port, PxSSTS));
  uint32_t det = ssts & 0xF;
  if (det != 3) {
    printk(LOG_WARN, "ahci: port %d DET=%d trying COMRESET...\n", port, det);
    if (ahci_comreset_port(port) != 0) {
      printk(LOG_WARN, "ahci: port %d no device after COMRESET\n", port);
      return -EIO;
    }
  }
  // Disable interrupts on old active port to prevent spurious interrupts
  if (active_port >= 0 && active_port != port) {
    port_disable_interrupts(active_port);
  }
  printk(LOG_INFO, "ahci: switching to port %d\n", port);
  port_init(port);
  active_port = port;
  // Filesystem I/O is synchronous and polls PxCI. Interrupts are enabled only
  // while ahci_submit_async() has an in-flight request.
  port_disable_interrupts(port);
  // Clear global IS to remove any stale bits from the old port
  writel((void __iomem *)((uint8_t __iomem *)abar + AHCI_IS), 0xFFFFFFFF);
  return 0;
}

// ===================== ahci_init =====================
__attribute__((no_sanitize("kernel-address"))) void ahci_init() {
  // Find AHCI controller (class 0x0106 = SATA/AHCI)
  pci_device *dev = pci_find_device(PCI_CLASS_STORAGE_AHCI);
  if (!dev) {
    ahci_puts("ahci: no AHCI controller found\n");
    halt();
  }

  printk(LOG_INFO, "ahci: found at bus %x dev %x\n", dev->bus, dev->dev);

  // Enable device: map BAR MMIO + Bus Master
  pci_enable_device(dev);
  abar = dev->bar[5].vaddr;

  // HBA reset: set GHC.HR (bit 0)
  uint32_t ghc = readl((void __iomem *)((uint8_t __iomem *)abar + AHCI_GHC));
  writel((void __iomem *)((uint8_t __iomem *)abar + AHCI_GHC), ghc | 1);

  // Poll until HR clears
  for (int i = 0; i < 1000000; i++) {
    if (!(readl((void __iomem *)((uint8_t __iomem *)abar + AHCI_GHC)) & 1))
      break;
  }

  // Enable AHCI mode: GHC.AE (bit 31)
  writel((void __iomem *)((uint8_t __iomem *)abar + AHCI_GHC),
         readl((void __iomem *)((uint8_t __iomem *)abar + AHCI_GHC)) |
             (1U << 31));

  // One-time DMA allocation (shared by all candidate ports)
  ahci_alloc_dma();

  // Scan PI bitmap: probe each port with DET==3 via IDENTIFY DEVICE
  // Scan ALL ports — disk.img is a single disk (two partitions: ESP +
  // root FAT32) on port 0; keep the multi-port scan for robustness.
  uint8_t idbuf[512];
  int disk_count = 0;
  active_port = -1;
  uint32_t pi = readl((void __iomem *)((uint8_t __iomem *)abar + AHCI_PI));
  printk(LOG_INFO, "ahci: PI=%x\n", pi);
  for (int i = 0; i < 32; i++) {
    if (!(pi & (1 << i)))
      continue;
    uint32_t ssts = readl(port_reg(i, PxSSTS));
    uint32_t det = ssts & 0xF;
    printk(LOG_INFO, "ahci: port %d SSTS=%x DET=%d", i, ssts, det);
    if (det != 3) {
      ahci_puts(" (no device)\n");
      continue;
    }

    port_init(i);
    if (ahci_identify_device(i, idbuf) == 0) {
      ahci_puts(" SATA disk\n");
      // Keep the first detected port as active; try all ports for ELF later
      if (active_port < 0) {
        active_port = i;
        const uint16_t *words = (const uint16_t *)idbuf;
        active_sector_count =
            (uint64_t)words[100] | ((uint64_t)words[101] << 16) |
            ((uint64_t)words[102] << 32) | ((uint64_t)words[103] << 48);
        if (active_sector_count == 0)
          active_sector_count =
              (uint64_t)words[60] | ((uint64_t)words[61] << 16);
      }
      disk_count++;
    } else {
      ahci_puts(" ATAPI, skipping\n");
      port_stop(i);
    }
  }

  // Startup disk is fixed: UEFI boots from the ESP on the single disk where
  // BOOTX64.EFI/myos.elf/init.elf live, and the kernel mounts that disk's root
  // FAT32 for init. The main scan above already found it (active_port). No
  // fallback COMRESET scan — probing empty ports blocks ~1.25s each on COMRESET
  // timeout. Runtime multi-disk support (additional /dev/sd* block devices) is
  // a separate user-visible concern, not needed at early boot.

  printk(LOG_INFO, "ahci: disks=%d active=%d\n", disk_count, active_port);

  if (active_port < 0) {
    ahci_puts("ahci: no SATA disk found\n");
    halt();
  }

  // Idempotent re-initialize the active port
  port_init(active_port);

  // Enable AHCI interrupts via MSI (bypasses I/O APIC entirely):
  // pci_enable_msi allocates a vector, writes Message Address/Data to the
  // device's MSI capability, enables MSI, and disables INTx.
  // Timer vector (0x78, priority class 7) is higher than MSI vectors
  // (64-95, priority class 4-5), so the LAPIC timer can always preempt
  // AHCI interrupt processing. The handler also does EOI early to minimize
  // the non-preemptible window.
  int msi_ret = pci_enable_msi(dev);
  if (msi_ret < 0) {
    // MSI unavailable — fall back to INTx via I/O APIC
    uint8_t ahci_gsi =
        (uint8_t)pci_read_config(dev->bus, dev->dev, dev->func, 0x3C) & 0xFF;
    uint8_t ahci_irq_vec = 32 + ahci_gsi;
    irq_register(ahci_irq_vec, ahci_irq_handler);

    uint32_t bsp_apic_id = lapic_read(LAPIC_ID) >> 24;
    const acpi_iso_override *iso = acpi_find_iso(ahci_gsi);
    bool level = iso ? iso->level_triggered : false;
    bool low = iso ? iso->active_low : false;
    ioapic_set_irq(ahci_gsi, ahci_irq_vec, bsp_apic_id, false, level, low);

    printk(LOG_WARN, "ahci: INTx fallback (GSI=%d vec=%d)\n", ahci_gsi,
           ahci_irq_vec);
  } else {
    uint8_t msi_vec = (uint8_t)dev->msix_vector_base;
    irq_register(msi_vec, ahci_irq_handler);

    printk(LOG_INFO, "ahci: MSI enabled (vec=%d)\n", msi_vec);
  }

  // Clear ALL port interrupt status before enabling GHC.IE.
  // Even with PxIE=0, QEMU AHCI can set PxIS bits (e.g. D2H FIS on FRE),
  // which sets GHC.IS bits. If we enable GHC.IE with stale IS bits,
  // MSI fires immediately in a storm: handler clears IS, EOI, but QEMU
  // regenerates the edge from the port's PxIS that we never cleared.
  for (int i = 0; i < 32; i++) {
    if (pi & (1 << i)) {
      port_disable_interrupts(i);
    }
  }
  // Clear global IS last
  writel((void __iomem *)((uint8_t __iomem *)abar + AHCI_IS), 0xFFFFFFFF);

  // Enable GHC.IE (global HBA interrupt enable, bit 1)
  writel((void __iomem *)((uint8_t __iomem *)abar + AHCI_GHC),
         readl((void __iomem *)((uint8_t __iomem *)abar + AHCI_GHC)) |
             (1U << 1));

  ahci_puts("ahci: init done\n");
}

// ===================== Async block I/O interface =====================

// Submit async block request. Returns cookie (>0) on success, -errno on error.
// Completion delivered via RECV_NOTIFY to caller.
int ahci_submit_async(uint32_t lba, void *buf, uint32_t count, uint8_t dir) {
  // Validate user buffer
  uint64_t ptr = (uint64_t)buf;
  if (!ptr || ptr >= KERNEL_VMA_BOUNDARY)
    return -EFAULT;
  uint64_t end = ptr + (uint64_t)count * 512;
  if (end < ptr || end > KERNEL_VMA_BOUNDARY)
    return -EFAULT;
  if (count == 0 || count > AHCI_MAX_SECTORS)
    return -EINVAL;
  if (dir > 1)
    return -EINVAL;
  uint64_t capacity = ahci_sector_count();
  if (capacity && ((uint64_t)lba + count > capacity))
    return -EINVAL;

  uint64_t flags;
  spin_lock_irqsave(&ahci_queue_lock, &flags);

  if (bq_count >= BLOCK_QUEUE_SIZE) {
    ahci_stats.queue_full++;
    spin_unlock_irqrestore(&ahci_queue_lock, flags);
    return -EBUSY;
  }

  // Allocate request from pool
  block_req *req = &block_pool[bq_tail];
  if (dir == 1 && copy_from_user(req->staging, (const void __user *)buf,
                                 (size_t)count * 512)) {
    spin_unlock_irqrestore(&ahci_queue_lock, flags);
    return -EFAULT;
  }
  req->caller_pid = current_task->pid;
  req->lba = lba;
  req->count = count;
  req->dir = dir;
  req->user_buf = buf;
  req->cookie = next_cookie_locked();
  perf_trace_causal(XOS_PERF_TRACE_IO, XOS_PERF_IO_SUBMIT,
                    0x80000000U | req->cookie);
  req->result = 0;
  req->kernel_wait = false;
  req->kernel_buf = NULL;
  req->wait = NULL;
  req->enqueue_tsc = rdtsc64();
  req->issue_tsc = 0;
  req->submit_cpu = (int16_t)get_cpu_local()->cpu_id;

  bq_tail = (bq_tail + 1) % BLOCK_QUEUE_SIZE;
  queue_depth_change_locked((uint32_t)bq_count + 1, req->enqueue_tsc);
  bq_count++;
  ahci_stats.async_submitted++;

  // Issue immediately if AHCI idle, else queue
  if (!ahci_current_req) {
    ahci_current_req = req;
    port_enable_interrupts(active_port);
    ahci_issue_cmd(req);
    // Command completion is handled by ahci_irq_handler (MSI ISR).
    // ISR reads PxIS, copies data, builds notification, and calls
    // notify_and_wake to unblock the caller.
  }

  spin_unlock_irqrestore(&ahci_queue_lock, flags);
  return (int)req->cookie; // positive cookie = success
}

int ahci_submit_sync(uint32_t lba, uint32_t count, void *buf, uint8_t dir) {
  if (get_cpu_local()->in_hardirq != 0)
    return -EWOULDBLOCK;

  struct ahci_wait_ctx wait = {
      .done = false, .result = -EIO, .waiter = current_task};
  uint64_t flags;
  spin_lock_irqsave(&ahci_queue_lock, &flags);
  if (bq_count >= BLOCK_QUEUE_SIZE) {
    ahci_stats.queue_full++;
    spin_unlock_irqrestore(&ahci_queue_lock, flags);
    return -EBUSY;
  }
  block_req *req = &block_pool[bq_tail];
  req->caller_pid = -1;
  req->lba = lba;
  req->count = count;
  req->dir = dir;
  req->cookie = next_cookie_locked();
  req->result = 0;
  req->kernel_wait = true;
  req->kernel_buf = buf;
  req->wait = &wait;
  req->enqueue_tsc = rdtsc64();
  req->issue_tsc = 0;
  req->submit_cpu = current_task ? (int16_t)current_task->assigned_cpu : -1;
  perf_trace_causal(XOS_PERF_TRACE_IO, XOS_PERF_IO_SUBMIT,
                    0x80000000U | req->cookie);
  if (dir)
    __memcpy(req->staging, buf, (size_t)count * 512);
  bq_tail = (bq_tail + 1) % BLOCK_QUEUE_SIZE;
  queue_depth_change_locked((uint32_t)bq_count + 1, req->enqueue_tsc);
  bq_count++;
  ahci_stats.sync_submitted++;

  int cpu = current_task ? current_task->assigned_cpu : 0;
  if (current_task) {
    spin_lock(&cpu_locals[cpu].scheduler_lock);
    current_task->state = BLOCKED;
    current_task->wait_event = WAIT_BLOCK_IO;
    spin_unlock(&cpu_locals[cpu].scheduler_lock);
  }
  if (!ahci_current_req) {
    ahci_current_req = req;
    port_enable_interrupts(active_port);
    ahci_issue_cmd(req);
  }
  spin_unlock_irqrestore(&ahci_queue_lock, flags);

  /* Before kernel_main installs the BSP idle task, filesystem discovery can
   * still issue requests. It uses the same MSI completion path; there is no
   * PxCI polling fallback. HLT merely waits for the completion interrupt. */
  if (!current_task) {
    while (!__atomic_load_n(&wait.done, __ATOMIC_ACQUIRE))
      __asm__ volatile("sti; hlt; cli" ::: "memory");
    __asm__ volatile("pushq %0; popfq" : : "r"(flags));
    return wait.result;
  }

  for (;;) {
    if (__atomic_load_n(&wait.done, __ATOMIC_ACQUIRE)) {
      sched_cancel_spurious_wake(current_task);
      return wait.result;
    }
    schedule();
    if (__atomic_load_n(&wait.done, __ATOMIC_ACQUIRE))
      return wait.result;
    spin_lock_irqsave(&ahci_queue_lock, &flags);
    spin_lock(&cpu_locals[cpu].scheduler_lock);
    if (!__atomic_load_n(&wait.done, __ATOMIC_ACQUIRE)) {
      current_task->state = BLOCKED;
      current_task->wait_event = WAIT_BLOCK_IO;
    }
    spin_unlock(&cpu_locals[cpu].scheduler_lock);
    spin_unlock_irqrestore(&ahci_queue_lock, flags);
  }
}

// ===================== Driver registry =====================
#include "kernel/driver/driver.h"

dev_driver ahci_driver = {
    .name = "ahci",
    .pci_class = 0x010600, // SATA AHCI (class=0x01, subclass=0x06)
    .pci_vendor = 0,
    .pci_device = 0,
    .init = ahci_init,
    .ops = NULL, // blk_dev_ops is in blk_dev.c
};
