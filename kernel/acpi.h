#ifndef KERNEL_ACPI_H
#define KERNEL_ACPI_H

#include <stdint.h>
#include <stddef.h>

// ===================== ACPI table structures =====================

struct acpi_rsdp {
  uint8_t  signature[8];   // "RSD PTR "
  uint8_t  checksum;
  uint8_t  oem_id[6];
  uint8_t  revision;
  uint32_t rsdt_address;
  uint32_t length;         // revision >= 2
  uint64_t xsdt_address;   // revision >= 2
} __attribute__((packed));

struct acpi_sdt_header {
  uint8_t  signature[4];
  uint32_t length;
  uint8_t  revision;
  uint8_t  checksum;
  uint8_t  oem_id[6];
  uint8_t  oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
  struct acpi_sdt_header header;
  uint32_t local_apic_address;
  uint32_t flags;
} __attribute__((packed));

struct acpi_madt_entry {
  uint8_t type;
  uint8_t length;
} __attribute__((packed));

struct acpi_madt_lapic_entry {
  uint8_t  type;       // 0
  uint8_t  length;
  uint8_t  processor_id;
  uint8_t  apic_id;
  uint32_t flags;
} __attribute__((packed));

struct acpi_madt_ioapic_entry {
  uint8_t  type;       // 1
  uint8_t  length;
  uint8_t  ioapic_id;
  uint8_t  reserved;
  uint32_t ioapic_address;
  uint32_t gsi_base;
} __attribute__((packed));

// MCFG table (PCI Firmware Spec 3.0)
struct acpi_mcfg {
  struct acpi_sdt_header header;
  uint64_t reserved;    // 8 bytes reserved
  // entries[] follow
} __attribute__((packed));

struct acpi_mcfg_entry {
  uint64_t base_address;   // ECAM base physical address
  uint16_t pci_segment;
  uint8_t  start_bus;
  uint8_t  end_bus;
  uint32_t reserved2;
} __attribute__((packed));

// ===================== ACPI results (set by acpi_init) =====================

#define MAX_MCFG_ENTRIES 4

struct acpi_madt_result {
  uint64_t lapic_base;
  uint64_t ioapic_base;
  uint32_t ncpus;
  uint32_t apic_ids[4];
};

struct acpi_mcfg_result {
  uint64_t ecam_base;
  uint16_t segment;
  uint8_t  start_bus;
  uint8_t  end_bus;
};

extern struct acpi_madt_result g_madt;
extern struct acpi_mcfg_result g_mcfg;

// ===================== ACPI API =====================

// Initialize ACPI: parse RSDP -> XSDT -> find MADT + MCFG
// Must be called after init_mem, before isr_init
void acpi_init(uint64_t rsdp_phys);

// Find an ACPI table by 4-byte signature
// Returns virtual address of the table header, or nullptr if not found
void *acpi_find_table(const char signature[4]);

#endif // KERNEL_ACPI_H
