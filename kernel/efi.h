/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_EFI_H
#define KERNEL_EFI_H

#include <stdint.h>

// ===================== Basic types =====================
typedef uint64_t efi_status;
typedef void *efi_handle;
typedef uint64_t efi_uintn;

// ===================== Status codes =====================
#define EFI_SUCCESS 0
#define EFI_BUFFER_TOO_SMALL 5
#define EFI_NOT_FOUND 14

// ===================== EFI GUID =====================
typedef struct {
  uint32_t data1;
  uint16_t data2;
  uint16_t data3;
  uint8_t data4[8];
} efi_guid;

// ===================== Memory type constants =====================
#define EfiReservedMemoryType 0
#define EfiLoaderCode 1
#define EfiLoaderData 2
#define EfiBootServicesCode 3
#define EfiBootServicesData 4
#define EfiRuntimeServicesCode 5
#define EfiRuntimeServicesData 6
#define EfiConventionalMemory 7
#define EfiUnusableMemory 8
#define EfiACPIReclaimMemory 9
#define EfiACPIMemoryNVS 10
#define EfiMemoryMappedIO 11
#define EfiMemoryMappedIOPortSpace 12
#define EfiPalCode 13

// ===================== Memory descriptor =====================
typedef struct {
  uint32_t type;
  uint32_t pad;
  uint64_t physical_start;
  uint64_t virtual_start;
  uint64_t number_of_pages;
  uint64_t attribute;
} efi_memory_descriptor;

// ===================== Table header =====================
typedef struct {
  uint64_t signature;
  uint32_t revision;
  uint32_t header_size;
  uint32_t crc32;
  uint32_t reserved;
} efi_table_header;

// ===================== Configuration table =====================
typedef struct {
  efi_guid vendor_guid;
  void *vendor_table;
} efi_configuration_table;

// ===================== GOP =====================
#define EFI_PIXEL_FORMAT_RGB 0
#define EFI_PIXEL_FORMAT_BGR 1

typedef struct {
  uint32_t version;
  uint32_t horizontal_resolution;
  uint32_t vertical_resolution;
  uint32_t pixel_format;
  uint32_t pixels_per_scan_line;
} efi_gop_mode_info;

typedef struct {
  uint32_t max_mode;
  uint32_t mode;
  efi_gop_mode_info *info;
  uint64_t size_of_info;
  uint64_t frame_buffer_base;
  uint64_t frame_buffer_size;
} efi_gop_mode;

typedef struct efi_gop efi_gop;
struct efi_gop {
  uint64_t query_mode;
  uint64_t set_mode;
  uint64_t blt;
  efi_gop_mode *mode;
};

// ===================== Loaded Image Protocol =====================
typedef struct {
  uint32_t revision;
  efi_handle parent_handle;
  efi_table_header *system_table;
  efi_handle device_handle;
  void *file_path;
  void *reserved;
  uint32_t load_options_size;
  void *load_options;
  void *image_base;
  uint64_t image_size;
  efi_status image_code_type;
  efi_status image_data_type;
  uint64_t unload;
} efi_loaded_image;

// ===================== Boot Services =====================
typedef struct {
  efi_table_header hdr;

  // Task Priority Services
  void *raise_tpl;
  void *restore_tpl;

  // Memory Services
  efi_status (*allocate_pages)(int, int, efi_uintn, uint64_t *);
  efi_status (*free_pages)(uint64_t, efi_uintn);
  efi_status (*get_memory_map)(efi_uintn *, void *, uint64_t *,
                                 efi_uintn *, uint32_t *);
  efi_status (*allocate_pool)(int, efi_uintn, void **);
  efi_status (*free_pool)(void *);

  // Event & Timer Services
  void *create_event;
  void *set_timer;
  void *wait_for_event;
  void *signal_event;
  void *close_event;
  void *check_event;

  // Protocol Handler Services
  efi_status (*install_protocol_interface)(efi_handle *, efi_guid *, int,
                                             void *);
  efi_status (*reinstall_protocol_interface)(efi_handle, efi_guid *,
                                               void *, void *);
  efi_status (*uninstall_protocol_interface)(efi_handle, efi_guid *,
                                               void *);
  efi_status (*handle_protocol)(efi_handle, efi_guid *, void **);
  void *reserved;
  void *register_protocol_notify;
  efi_status (*locate_handle)(int, efi_guid *, void *, efi_uintn *,
                                efi_handle *);
  efi_status (*locate_device_path)(efi_guid *, void **, efi_handle *);
  efi_status (*install_configuration_table)(efi_guid *, void *);

  // Image Services
  efi_status (*load_image)(uint8_t, efi_handle, void *, void *, efi_uintn,
                             efi_handle *);
  efi_status (*start_image)(efi_handle, efi_uintn *, uint16_t **);
  efi_status (*exit)(efi_handle, efi_status, efi_uintn, uint16_t *);
  efi_status (*unload_image)(efi_handle);
  efi_status (*exit_boot_services)(efi_handle, efi_uintn);

  // Misc Services
  void *get_next_monotonic_count;
  void *stall;
  void *set_watchdog_timer;

  // Driver Support Services
  void *connect_controller;
  void *disconnect_controller;

  // Protocol Services
  void *open_protocol;
  void *close_protocol;
  void *open_protocol_information;
  void *protocols_per_handle;
  void *locate_handle_buffer;
  efi_status (*locate_protocol)(efi_guid *, void *, void **);
  void *install_multiple_protocol_interfaces;
  void *uninstall_multiple_protocol_interfaces;

  // CRC Services
  void *calculate_crc32;

  // Misc
  void *copy_mem;
  void *set_mem;
  void *create_event_ex;
} efi_boot_services;

// ===================== System Table =====================
typedef struct {
  efi_table_header hdr;
  uint16_t *firmware_vendor;
  uint32_t firmware_revision;
  efi_handle console_in_handle;
  void *con_in;
  efi_handle console_out_handle;
  void *con_out;
  efi_handle standard_error_handle;
  void *std_err;
  efi_boot_services *boot_services;
  void *runtime_services;
  efi_uintn number_of_table_entries;
  efi_configuration_table *configuration_table;
} efi_system_table;

// ===================== GUIDs =====================
// ACPI 2.0 RSDP
#define EFI_ACPI20_TABLE_GUID                                                  \
  {                                                                            \
    0x8868E871, 0xE4F1, 0x11D3, {                                              \
      0xBC, 0xB1, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81                           \
    }                                                                          \
  }

// GOP
#define EFI_GOP_GUID                                                           \
  {                                                                            \
    0x9042A9DE, 0x23DC, 0x4A38, {                                              \
      0x96, 0xFB, 0x7A, 0xDE, 0xEE, 0x44, 0xB9, 0x75                           \
    }                                                                          \
  }

// Loaded Image Protocol
#define EFI_LOADED_IMAGE_GUID                                                  \
  {                                                                            \
    0x5B1B31A1, 0x9562, 0x11D2, {                                              \
      0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B                           \
    }                                                                          \
  }

#endif // KERNEL_EFI_H
