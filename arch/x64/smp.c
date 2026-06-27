#include <stddef.h>
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "arch/x64/apic.h"
#include "arch/x64/paging.h"
#include "arch/x64/trap.h"
#include "kernel/acpi.h"
#include "kernel/serial.h"
#include "kernel/mem/alloc.h"
#include "kernel/proc.h"

cpu_local_t cpu_locals[MAX_CPUS];
int ncpu = 1;
gdt_entry_t per_cpu_gdt[MAX_CPUS][8];
gdt_ptr_t per_cpu_gdtr[MAX_CPUS];
tss_t per_cpu_tss[MAX_CPUS];
uint64_t per_cpu_ist_stack[MAX_CPUS][3]; // IST1=NMI, IST2=DF, IST3=MCE

// Trampoline page at physical 0x8000, mapped via identity map
#define AP_TRAMPOLINE_PHYS 0x8000
#define AP_TRAMPOLINE_VIRT (AP_TRAMPOLINE_PHYS + VMA_BASE)

// Trampoline data layout (offsets within the trampoline page at 0x8000)
// BSP writes these before sending SIPI; AP reads them in real/protected mode
#define TRAMP_PML4      0xC0  // uint64_t: PML4 physical address
#define TRAMP_STACK     0xC8  // uint64_t: kernel stack top (virtual)
#define TRAMP_ENTRY     0xD0  // uint64_t: ap_entry_c virtual address
#define TRAMP_CPU_ID    0xD8  // uint32_t: cpu_id

// Trampoline code (defined in ap_trampoline.S)
extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];

static void set_gdt_gate(gdt_entry_t *gdt, int n, uint32_t base, uint32_t limit,
                         uint8_t access, uint8_t gran) {
    gdt[n].limit_low = L16(limit);
    gdt[n].base_low = L16(base);
    gdt[n].base_middle = (base >> 16) & 0xFF;
    gdt[n].access = access;
    gdt[n].granularity = ((gran & 0x0F) << 4) | ((limit >> 16) & 0x0F);
    gdt[n].base_high = (base >> 24) & 0xFF;
}

static void set_tss_gate(gdt_entry_t *gdt, int n, uint64_t base, uint32_t limit) {
    gdt[n].limit_low = L16(limit);
    gdt[n].base_low = L16(base);
    gdt[n].base_middle = (base >> 16) & 0xFF;
    gdt[n].access = 0x89;  // Available 64-bit TSS
    gdt[n].granularity = 0x00;
    gdt[n].base_high = (base >> 24) & 0xFF;

    uint32_t *hi = (uint32_t *)&gdt[n + 1];
    hi[0] = (uint32_t)(base >> 32);
    hi[1] = 0;
}

void reload_cs(void);

__attribute__((no_sanitize("kernel-address")))
void smp_init_cpu(int cpu_id, uint32_t apic_id, uint64_t kernel_stack) {
    // Fill cpu_local
    cpu_locals[cpu_id].cpu_id = cpu_id;
    cpu_locals[cpu_id].apic_id = apic_id;
    cpu_locals[cpu_id]._cur_proc = NULL;
    cpu_locals[cpu_id].lapic_base = NULL;
    cpu_locals[cpu_id].kernel_stack = kernel_stack;
    cpu_locals[cpu_id].tss_rsp0 = kernel_stack;
    cpu_locals[cpu_id].run_count = 0;
    cpu_locals[cpu_id].scheduler_lock.locked = 0;
    list_init(&cpu_locals[cpu_id].run_queue);
    list_init(&cpu_locals[cpu_id].timer_queue);
    for (int c = 0; c < NUM_KMALLOC_CLASSES; c++) {
        cpu_locals[cpu_id].active_slab[c] = NULL;
    }

    // Set up per-CPU GDT (8 entries)
    gdt_entry_t *gdt = per_cpu_gdt[cpu_id];
    set_gdt_gate(gdt, 0, 0, 0, 0, 0);                   // null
    set_gdt_gate(gdt, 1, 0, 0, 0x9A, 0x02);             // kernel code64 (L=1)
    set_gdt_gate(gdt, 2, 0, 0, 0x92, 0x00);             // kernel data
    set_gdt_gate(gdt, 3, 0, 0, 0xFA, 0x00);             // user code32 compat (DPL=3, STAR[63:48] base)
    set_gdt_gate(gdt, 4, 0, 0, 0xF2, 0x00);             // user data (DPL=3)
    set_gdt_gate(gdt, 5, 0, 0, 0xFA, 0x02);             // user code64 (DPL=3, L=1)

    // Set up per-CPU TSS
    tss_t *tss = &per_cpu_tss[cpu_id];
    for (size_t i = 0; i < sizeof(tss_t); i++)
        ((uint8_t *)tss)[i] = 0;
    tss->rsp0 = kernel_stack;
    tss->iomap_base = 104;  // offset of IOPM within TSS (after reserved3)
    // Initialize IOPM to deny all ports (all bits = 1)
    for (int i = 0; i < IOPM_SIZE; i++)
        tss->iopm[i] = 0xFF;

    // Allocate per-CPU IST stacks (1 page each: NMI, Double Fault, Machine Check)
    for (int i = 0; i < 3; i++) {
        Page *ist_page = bfc_alloc_page(1);
        if (!ist_page) {
            serial_puts("smp_init_cpu: IST alloc failed\n");
            halt();
        }
        uint64_t ist_phys = (uint64_t)(ist_page - bfc_frames) * PAGE_SIZE;
        per_cpu_ist_stack[cpu_id][i] = (__force uint64_t)phys_to_virt((__force phys_addr_t)ist_phys) + PAGE_SIZE; // top of page
    }
    tss->ist[0] = per_cpu_ist_stack[cpu_id][0]; // IST1 = NMI (#2)
    tss->ist[1] = per_cpu_ist_stack[cpu_id][1]; // IST2 = Double Fault (#8)
    tss->ist[2] = per_cpu_ist_stack[cpu_id][2]; // IST3 = Machine Check (#18)

    set_tss_gate(gdt, 6, (uint64_t)tss, sizeof(tss_t) - 1);

    // Fill GDTR (but don't load it yet)
    per_cpu_gdtr[cpu_id].limit = sizeof(per_cpu_gdt[0]) - 1;
    per_cpu_gdtr[cpu_id].base = (uint64_t)gdt;
}

// Apply per-CPU state to the currently running CPU.
__attribute__((no_sanitize("kernel-address")))
void smp_apply_cpu(int cpu_id) {
    lgdt(&per_cpu_gdtr[cpu_id]);
    reload_cs();

    // Set KERNEL_GS_BASE = cpu_locals (for swapgs on kernel entry from user mode)
    set_cpu_local(&cpu_locals[cpu_id]);

    // swapgs: GS_BASE(0) ↔ KERNEL_GS_BASE(cpu_locals) → GS_BASE = cpu_locals
    __asm__ volatile("swapgs");

    // Reload data segment registers (NOT gs — writing gs selector clears GS_BASE MSR in QEMU)
    __asm__ volatile(
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n" ::: "ax");

    // Load TR
    ltr(TSS_SEL);
}

// ===================== AP entry (called from trampoline code) =====================
__attribute__((no_sanitize("kernel-address")))
void ap_entry_c(int cpu_id) {
    // Apply per-CPU state (GDT, GS base, TR) to this AP
    smp_apply_cpu(cpu_id);

    // Program PAT MSR for this AP (must be done after paging enabled)
    pat_init();

    // Load IDT (same IDT as BSP, shared kernel address space)
    idt_install();

    // Setup SYSCALL/SYSRET MSRs for this CPU
    setup_syscall();

    // Enable LAPIC for this AP
    uint64_t msr = rdmsr(MSR_IA32_APIC_BASE);
    msr |= APIC_BASE_ENABLE;
    wrmsr(MSR_IA32_APIC_BASE, msr);

    // Software-enable LAPIC
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFF);

    // Mask unused LVT entries
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);

    // Start LAPIC timer (periodic, same config as BSP)
    lapic_write(LAPIC_TIMER_DCR, 0x0B); // divide by 1
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, lapic_timer_ticks_calibrated);

    // Set this AP's lapic_base in cpu_local
    cpu_locals[cpu_id].lapic_base = lapic_vaddr;

    serial_puts("AP ");
    serial_put_hex(cpu_id);
    serial_puts(" init finish\n");

    // Switch to idle process: set current_proc, switch to idle kernel stack
    proc_t *idle = cpu_locals[cpu_id].idle_proc;
    current_proc = idle;
    idle->state = RUNNING;
    per_cpu_tss[cpu_id].rsp0 = idle->k_stack_top;
    cpu_locals[cpu_id].tss_rsp0 = idle->k_stack_top;

    // Switch to idle kernel stack and enter idle_entry (never returns)
    uint64_t idle_rsp = idle->k_rsp;
    __asm__ volatile(
        "movq %0, %%rsp\n"
        "popq %%rbx\n"
        "popq %%rbp\n"
        "popq %%r12\n"
        "popq %%r13\n"
        "popq %%r14\n"
        "popq %%r15\n"
        "retq\n"
        :: "r"(idle_rsp)
        : "memory");
    // never reaches here
}

// ===================== LAPIC IPI helpers =====================

// Send INIT IPI to an AP
__attribute__((no_sanitize("kernel-address")))
static void lapic_send_init(uint32_t apic_id) {
    // Wait until ICR delivery status is idle
    while (lapic_read(LAPIC_ICR_LOW) & 0x1000)
        __asm__ volatile("pause");

    // ICR_HIGH: set destination APIC ID
    lapic_write(LAPIC_ICR_HIGH, (uint64_t)apic_id << 24);
    // ICR_LOW: INIT IPI, assert, physical destination
    // Delivery mode = INIT (101b), level = assert, trigger = edge
    lapic_write(LAPIC_ICR_LOW, 0x00004500);
}

// Send SIPI (Startup IPI) to an AP at given vector (page-aligned)
__attribute__((no_sanitize("kernel-address")))
static void lapic_send_sipi(uint32_t apic_id, uint8_t vector) {
    while (lapic_read(LAPIC_ICR_LOW) & 0x1000)
        __asm__ volatile("pause");

    lapic_write(LAPIC_ICR_HIGH, (uint64_t)apic_id << 24);
    // Delivery mode = Startup (110b), level = assert, vector = page number
    lapic_write(LAPIC_ICR_LOW, 0x00004600 | vector);
}

// Microsecond delay using LAPIC timer (one-shot mode)
__attribute__((no_sanitize("kernel-address")))
static void udelay(uint32_t us) {
    if (lapic_timer_ticks_calibrated == 0) return;
    // ticks per 10ms = lapic_timer_ticks_calibrated
    // ticks per 1us = lapic_timer_ticks_calibrated / 10000
    // Multiply first to avoid truncation
    uint32_t ticks = lapic_timer_ticks_calibrated * us / 10000;

    lapic_write(LAPIC_TIMER_DCR, 0x0B); // divide by 1
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED); // one-shot, masked
    lapic_write(LAPIC_TIMER_ICR, ticks);

    // Wait for timer to expire (CCR reaches 0)
    while (lapic_read(LAPIC_TIMER_CCR) != 0)
        __asm__ volatile("pause");
}

// ===================== Boot all APs via SIPI (called by BSP) =====================

__attribute__((no_sanitize("kernel-address")))
void smp_boot_aps() {
    if (g_madt.ncpus <= 1) return;

    ncpu = g_madt.ncpus;

    // Copy trampoline to physical 0x8000
    uint64_t trampoline_size = (uint64_t)ap_trampoline_end - (uint64_t)ap_trampoline_start;
    uint8_t *trampoline_dst = (uint8_t *)AP_TRAMPOLINE_VIRT;
    for (uint64_t i = 0; i < trampoline_size; i++) {
        trampoline_dst[i] = ap_trampoline_start[i];
    }

    // Boot each AP (cpu_id 1..ncpu-1) one at a time
    for (int i = 1; i < ncpu; i++) {
        uint32_t apic_id = g_madt.apic_ids[i];

        // Allocate kernel stack (8KB)
        Page *stack_pages = bfc_alloc_page(2);
        if (!stack_pages) {
            ncpu = i;
            break;
        }
        uint64_t k_stack_phys = (uint64_t)(stack_pages - bfc_frames) * PAGE_SIZE;
        uint64_t k_stack_top = (__force uint64_t)phys_to_virt((__force phys_addr_t)k_stack_phys) + 2 * PAGE_SIZE;

        // Initialize per-CPU data structures (fills cpu_locals, GDT, TSS)
        smp_init_cpu(i, apic_id, k_stack_top);

        // Create idle process for this AP
        create_idle_process(i);

        // Fill trampoline data fields (BSP writes, AP reads in 16-bit mode)
        volatile uint64_t *t_pml4   = (volatile uint64_t *)(trampoline_dst + TRAMP_PML4);
        volatile uint64_t *t_stack  = (volatile uint64_t *)(trampoline_dst + TRAMP_STACK);
        volatile uint64_t *t_entry  = (volatile uint64_t *)(trampoline_dst + TRAMP_ENTRY);
        volatile uint32_t *t_cpuid = (volatile uint32_t *)(trampoline_dst + TRAMP_CPU_ID);

        *t_pml4  = (__force uint64_t)PHY_ADDR((uintptr_t)pml4);
        *t_stack = k_stack_top;
        *t_entry = (uint64_t)ap_entry_c;
        *t_cpuid = (uint32_t)i;

        // Send INIT IPI
        lapic_send_init(apic_id);
        udelay(10000); // wait 10ms

        // Send SIPI (vector = 8, meaning AP starts at 0x8000)
        lapic_send_sipi(apic_id, 0x08);
        udelay(200); // wait 200us

        // Send second SIPI (per Intel manual recommendation)
        lapic_send_sipi(apic_id, 0x08);
        udelay(200);
    }

    // Restore BSP LAPIC timer (udelay clobbers it to masked one-shot)
    lapic_write(LAPIC_TIMER_DCR, 0x0B);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, lapic_timer_ticks_calibrated);
    serial_printf("smp: BSP timer restored vec=0x%x\n", LAPIC_TIMER_VECTOR);
}
