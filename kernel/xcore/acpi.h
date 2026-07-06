/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_ACPI_H
#define KERNEL_ACPI_H

#include <stdbool.h>
#include <stdint.h>

// ===================== ACPI table structures =====================

typedef struct acpi_rsdp {
  uint8_t signature[8]; // "RSD PTR "
  uint8_t checksum;
  uint8_t oem_id[6];
  uint8_t revision;
  uint32_t rsdt_address;
  uint32_t length;       // revision >= 2
  uint64_t xsdt_address; // revision >= 2
} __attribute__((packed)) acpi_rsdp;

typedef struct acpi_sdt_header {
  uint8_t signature[4];
  uint32_t length;
  uint8_t revision;
  uint8_t checksum;
  uint8_t oem_id[6];
  uint8_t oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header;

typedef struct acpi_madt {
  acpi_sdt_header header;
  uint32_t local_apic_address;
  uint32_t flags;
} __attribute__((packed)) acpi_madt;

typedef struct acpi_madt_entry {
  uint8_t type;
  uint8_t length;
} __attribute__((packed)) acpi_madt_entry;

typedef struct acpi_madt_lapic_entry {
  uint8_t type; // 0
  uint8_t length;
  uint8_t processor_id;
  uint8_t apic_id;
  uint32_t flags;
} __attribute__((packed)) acpi_madt_lapic_entry;

typedef struct acpi_madt_ioapic_entry {
  uint8_t type; // 1
  uint8_t length;
  uint8_t ioapic_id;
  uint8_t reserved;
  uint32_t ioapic_address;
  uint32_t gsi_base;
} __attribute__((packed)) acpi_madt_ioapic_entry;

// MADT Interrupt Source Override entry (type 2)
typedef struct acpi_madt_iso_entry {
  uint8_t type;   // 2
  uint8_t length; // 10
  uint8_t bus;    // 0 = ISA
  uint8_t irq;    // ISA IRQ number
  uint32_t gsi;   // Global System Interrupt
  uint16_t flags; // bit 0: polarity (0=high, 1=low), bit 1: trigger (0=edge,
                  // 1=level)
} __attribute__((packed)) acpi_madt_iso_entry;

// Parsed ISO override record
typedef struct acpi_iso_override {
  uint8_t irq;     // ISA IRQ
  uint32_t gsi;    // mapped GSI
  bool active_low; // polarity
  bool level_triggered;
} acpi_iso_override;

#define MAX_ISO_OVERRIDES 16

// MCFG table (PCI Firmware Spec 3.0)
typedef struct acpi_mcfg {
  acpi_sdt_header header;
  uint64_t reserved; // 8 bytes reserved
  // entries[] follow
} __attribute__((packed)) acpi_mcfg;

typedef struct acpi_mcfg_entry {
  uint64_t base_address; // ECAM base physical address
  uint16_t pci_segment;
  uint8_t start_bus;
  uint8_t end_bus;
  uint32_t reserved2;
} __attribute__((packed)) acpi_mcfg_entry;

// ===================== ACPI results (set by acpi_init) =====================

#define MAX_MCFG_ENTRIES 4

typedef struct acpi_madt_result {
  uint64_t lapic_base;
  uint64_t ioapic_base;
  uint32_t ioapic_gsi_base;
  uint32_t ncpus;
  uint32_t apic_ids[4];
  uint32_t num_iso;
  acpi_iso_override iso[MAX_ISO_OVERRIDES];
} acpi_madt_result;

typedef struct acpi_mcfg_result {
  uint64_t ecam_base;
  uint16_t segment;
  uint8_t start_bus;
  uint8_t end_bus;
} acpi_mcfg_result;

extern acpi_madt_result g_madt;
extern acpi_mcfg_result g_mcfg;

// ===================== ACPI API =====================

// Look up ISO override for an ISA IRQ. Returns NULL if no override.
const acpi_iso_override *acpi_find_iso(uint8_t isa_irq);

// Initialize ACPI: parse RSDP -> XSDT -> find MADT + MCFG
// Must be called after init_mem, before irq_init
void acpi_init(uint64_t rsdp_phys);

// Find an ACPI table by 4-byte signature
// Returns virtual address of the table header, or NULL if not found
void *acpi_find_table(const char signature[4]);

#endif // KERNEL_ACPI_H
