#include "kernel/xcore/acpi.h"
#include "arch/x64/paging.h"
#include "boot/boot.h"
#include "common/macro.h"
#include "kernel/xcore/log.h"
#include <stdbool.h>
#include <stddef.h>

acpi_madt_result_t g_madt = {0, 0, 0, 0, {0}, 0, {{0}}};
acpi_mcfg_result_t g_mcfg = {0, 0, 0, 0};

// Internal XSDT state for acpi_find_table
static uint64_t xsdt_virt = 0;
static uint8_t xsdt_entry_size = 8;
static uint32_t xsdt_entry_count = 0;

static bool acpi_checksum(const void *tbl, size_t len) {
  const uint8_t *p = (const uint8_t *)tbl;
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++)
    sum += p[i];
  return sum == 0;
}

static bool sig4_match(const uint8_t *a, const char *b) {
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

__attribute__((no_sanitize("kernel-address"))) void *
acpi_find_table(const char signature[4]) {
  if (xsdt_virt == 0)
    return NULL;

  for (uint32_t i = 0; i < xsdt_entry_count; i++) {
    uint64_t addr;
    if (xsdt_entry_size == 8) {
      addr = *(const uint64_t *)(xsdt_virt + sizeof(acpi_sdt_header_t) + i * 8);
    } else {
      addr = *(const uint32_t *)(xsdt_virt + sizeof(acpi_sdt_header_t) + i * 4);
    }
    uint64_t table_virt = addr + VMA_BASE;
    const acpi_sdt_header_t *t = (const acpi_sdt_header_t *)table_virt;
    if (sig4_match(t->signature, signature)) {
      return (void *)table_virt;
    }
  }
  return NULL;
}

static void parse_madt(const acpi_madt_t *madt) {
  g_madt.lapic_base = madt->local_apic_address;
  g_madt.ncpus = 0;

  size_t offset = sizeof(acpi_madt_t);
  while (offset < madt->header.length) {
    const acpi_madt_entry_t *e =
        (const acpi_madt_entry_t *)((const uint8_t *)madt + offset);
    if (e->length == 0)
      break;

    if (e->type == 0 && g_madt.ncpus < 4) {
      const acpi_madt_lapic_entry_t *lapic = (const acpi_madt_lapic_entry_t *)e;
      if (lapic->flags & 1) {
        g_madt.apic_ids[g_madt.ncpus] = lapic->apic_id;
        g_madt.ncpus++;
      }
    } else if (e->type == 1 && g_madt.ioapic_base == 0) {
      const acpi_madt_ioapic_entry_t *ioapic =
          (const acpi_madt_ioapic_entry_t *)e;
      g_madt.ioapic_base = ioapic->ioapic_address;
      g_madt.ioapic_gsi_base = ioapic->gsi_base;
    } else if (e->type == 2 && g_madt.num_iso < MAX_ISO_OVERRIDES) {
      const acpi_madt_iso_entry_t *iso = (const acpi_madt_iso_entry_t *)e;
      if (iso->bus == 0) { // only ISA overrides
        acpi_iso_override_t *o = &g_madt.iso[g_madt.num_iso];
        o->irq = iso->irq;
        o->gsi = iso->gsi;
        o->active_low = (iso->flags & 1) != 0;
        o->level_triggered = (iso->flags & 2) != 0;
        g_madt.num_iso++;
      }
    }
    offset += e->length;
  }
}

const acpi_iso_override_t *acpi_find_iso(uint8_t isa_irq) {
  for (uint32_t i = 0; i < g_madt.num_iso; i++) {
    if (g_madt.iso[i].irq == isa_irq)
      return &g_madt.iso[i];
  }
  return NULL;
}

static void parse_mcfg(const acpi_mcfg_t *mcfg) {
  size_t offset = sizeof(acpi_mcfg_t);
  if (offset + sizeof(acpi_mcfg_entry_t) > mcfg->header.length) {
    printk(LOG_WARN, "acpi: MCFG has no entries\n");
    return;
  }
  const acpi_mcfg_entry_t *entry =
      (const acpi_mcfg_entry_t *)((const uint8_t *)mcfg + offset);
  g_mcfg.ecam_base = entry->base_address;
  g_mcfg.segment = entry->pci_segment;
  g_mcfg.start_bus = entry->start_bus;
  g_mcfg.end_bus = entry->end_bus;
}

__attribute__((no_sanitize("kernel-address"))) void
acpi_init(uint64_t rsdp_phys) {
  printk(LOG_INFO, "acpi_init: rsdp_phys=0x%016lX\n", rsdp_phys);

  // 1. Map RSDP (ACPI tables in RAM, already mapped by extend_mapping)
  uint64_t rsdp_virt = rsdp_phys + VMA_BASE;
  const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)rsdp_virt;

  // 2. Validate RSDP signature "RSD PTR "
  static const char expected_sig[] = "RSD PTR ";
  for (int i = 0; i < 8; i++) {
    if (rsdp->signature[i] != expected_sig[i]) {
      printk(LOG_WARN, "acpi: RSDP signature mismatch\n");
      return;
    }
  }

  // 3. Validate RSDP checksum
  size_t rsdp_len = (rsdp->revision < 2) ? 20 : rsdp->length;
  if (!acpi_checksum(rsdp, rsdp_len)) {
    printk(LOG_WARN, "acpi: RSDP checksum failed\n");
    return;
  }

  printk(LOG_INFO, "acpi: RSDP valid, revision=%u\n", rsdp->revision);

  // 4. Follow XSDT or RSDT
  uint64_t sdt_phys = 0;
  if (rsdp->revision >= 2 && rsdp->xsdt_address) {
    sdt_phys = rsdp->xsdt_address;
    xsdt_entry_size = 8;
  } else {
    sdt_phys = rsdp->rsdt_address;
    xsdt_entry_size = 4;
  }
  xsdt_virt = sdt_phys + VMA_BASE;
  const acpi_sdt_header_t *sdt_hdr = (const acpi_sdt_header_t *)xsdt_virt;
  xsdt_entry_count =
      (sdt_hdr->length - sizeof(acpi_sdt_header_t)) / xsdt_entry_size;

  printk(LOG_INFO, "acpi: SDT at 0x%016lX entries=%u\n", sdt_phys,
         xsdt_entry_count);

  // 5. Parse MADT
  void *madt_ptr = acpi_find_table("APIC");
  if (madt_ptr) {
    parse_madt((const struct acpi_madt *)madt_ptr);
    printk(LOG_INFO,
           "acpi: MADT lapic=0x%016lX ioapic=0x%016lX gsi_base=0x%08X ncpus=%u "
           "iso=%u\n",
           g_madt.lapic_base, g_madt.ioapic_base, g_madt.ioapic_gsi_base,
           g_madt.ncpus, g_madt.num_iso);
    for (uint32_t i = 0; i < g_madt.num_iso; i++) {
      printk(LOG_INFO, "  ISO: irq=%u gsi=%u low=%d level=%d\n",
             g_madt.iso[i].irq, g_madt.iso[i].gsi,
             g_madt.iso[i].active_low ? 1 : 0,
             g_madt.iso[i].level_triggered ? 1 : 0);
    }
  } else {
    printk(LOG_WARN, "acpi: MADT not found\n");
  }

  // 6. Parse MCFG
  void *mcfg_ptr = acpi_find_table("MCFG");
  if (mcfg_ptr) {
    parse_mcfg((const struct acpi_mcfg *)mcfg_ptr);
    printk(LOG_INFO, "acpi: MCFG ecam_base=0x%016lX bus=%u-%u\n",
           g_mcfg.ecam_base, g_mcfg.start_bus, g_mcfg.end_bus);
  } else {
    printk(LOG_WARN, "acpi: MCFG not found\n");
  }
}
