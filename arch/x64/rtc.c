/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// arch/x64/rtc.c — CMOS MC146818 RTC reader (wall-clock source for
// CLOCK_REALTIME).
//
// Ports: 0x70 (write) selects a CMOS register; bit7 disables NMI.  0x71 (read)
// returns the selected register's value.  Registers 0..9 = sec/min/hour/day/
// month/year/century/A/B/C/D.  Values are BCD unless register B bit2 (DM) is
// set.  Register A bit7 (UIP) is the update-in-progress flag: while set the
// time fields may be mid-update, so we wait for it to clear and double-read to
// defend against the UIP boundary.
//
// Reference: Linux drivers/rtc/rtc-cmos.c, OSDev "CMOS".  QEMU's MC146818
// emulation defaults to BCD + 24h + century register 0x32 present; we handle
// binary mode and the missing-century fallback for real hardware too.

#include "arch/x64/rtc.h"
#include "arch/x64/apic.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/log.h"

uint64_t wall_clock_boot_ns = 0;

// Read one CMOS register.  Disables NMI (0x70 bit7) for the duration of the
// access; NMI is not restored — the kernel has no NMI handler and boot-time NMI
// state is unspecified under UEFI, so leaving it masked is harmless (todo:
// restore NMI when an NMI watchdog is added).  Caller must hold interrupts
// disabled across the full read sequence so an IRQ doesn't stall the access
// past a UIP flip.
static uint8_t cmos_read_reg(int reg) {
  outb(0x70, (uint8_t)(reg | 0x80)); // select reg, NMI disabled
  return inb(0x71);
}

// Wait for UIP (register A bit7) to clear, bounded by a ~1ms rdtsc budget so a
// stuck UIP can't hang boot.  Returns 1 if UIP cleared, 0 on timeout.
static int cmos_wait_uip_clear(void) {
  uint64_t budget = tsc_freq / 1000; // ~1ms in TSC ticks
  uint64_t start = rdtsc64();
  for (;;) {
    if ((cmos_read_reg(0x0A) & 0x80) == 0)
      return 1;
    if (rdtsc64() - start >= budget)
      return 0;
    __asm__ volatile("pause");
  }
}

// Decode a raw CMOS field: BCD→binary when the DM flag is clear, binary pass-
// through when set.
static int decode_field(uint8_t raw, int binary_mode) {
  if (binary_mode)
    return (int)raw;
  return (raw >> 4) * 10 + (raw & 0x0F);
}

// Read all time fields once into t.  Returns the DM flag (register B bit2) so
// the caller can decode; 24h flag is register B bit1 (0 = 12h, 1 = 24h).
static void cmos_read_fields(struct rtc_time *t, int *binary_mode,
                             int *hour_24) {
  uint8_t b = cmos_read_reg(0x0B);
  *binary_mode = (b & 0x04) ? 1 : 0;
  *hour_24 = (b & 0x02) ? 1 : 0;

  uint8_t sec_raw = cmos_read_reg(0x00);
  uint8_t min_raw = cmos_read_reg(0x02);
  uint8_t hour_raw = cmos_read_reg(0x04);
  uint8_t day_raw = cmos_read_reg(0x07);
  uint8_t mon_raw = cmos_read_reg(0x08);
  uint8_t year_raw = cmos_read_reg(0x09);

  t->sec = decode_field(sec_raw, *binary_mode);
  t->min = decode_field(min_raw, *binary_mode);
  t->mday = decode_field(day_raw, *binary_mode);
  t->mon = decode_field(mon_raw, *binary_mode);
  t->year = decode_field(year_raw, *binary_mode); // 2-digit year, fixed below

  // Hour: in 12h mode bit7 of the raw byte marks PM.  Clear it before decoding
  // (BCD PM byte has bit7 set), then add 12 for PM (12h→0h midnight edge: 12
  // AM = 0, 12 PM = 12).
  if (!*hour_24) {
    int pm = (hour_raw & 0x80) ? 1 : 0;
    int h = decode_field(hour_raw & 0x7F, *binary_mode);
    if (pm)
      t->hour = (h % 12) + 12; // 12 PM -> 12, 1..11 PM -> 13..23
    else
      t->hour = h % 12; // 12 AM -> 0, 1..11 AM -> 1..11
  } else {
    t->hour = decode_field(hour_raw, *binary_mode);
  }
}

// Resolve a 4-digit year: prefer the century register 0x32 (BCD); fall back to
// the Linux-style 1900/2000 split on the 2-digit year.  The OS boots in 2000+,
// so year<70 → 2000, year>=70 → 1900 (correct until ~2070).
static int resolve_year(int year2, int binary_mode) {
  uint8_t cent_raw = cmos_read_reg(0x32);
  int century = decode_field(cent_raw, binary_mode);
  if (century >= 19 && century <= 99)
    return century * 100 + year2;
  return year2 < 70 ? year2 + 2000 : year2 + 1900;
}

// Broken-down calendar (UTC) → Unix epoch seconds.  Gregorian day count since
// 1970-01-01 (OSDev / Linux mktime simplified); leap rule 4/100/400.
static uint64_t rtc_to_epoch(const struct rtc_time *t) {
  int y = t->year;
  int m = t->mon;
  int d = t->mday;
  if (m <= 2) {
    y -= 1;
    m += 12;
  }
  int days = 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d -
             719499;
  return (uint64_t)days * 86400ULL + (uint64_t)t->hour * 3600ULL +
         (uint64_t)t->min * 60ULL + (uint64_t)t->sec;
}

uint64_t rtc_read_epoch_seconds(void) {
  // Disable interrupts for the whole read: a late IRQ mid-sequence could flip
  // UIP and make the double-read disagree pointlessly.  CMOS access is pure
  // port I/O with no IRQ dependency.
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags));

  struct rtc_time t, t2;
  int bm, h24;
  int ok = 0;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (!cmos_wait_uip_clear()) {
      printk(LOG_WARN, "rtc: UIP stuck (attempt %d)\n", attempt);
      continue;
    }
    cmos_read_fields(&t, &bm, &h24);
    t.year = resolve_year(t.year, bm);

    // Double-read: if UIP flipped between the two reads the fields may differ;
    // retry.
    if (!cmos_wait_uip_clear())
      continue;
    cmos_read_fields(&t2, &bm, &h24);
    t2.year = resolve_year(t2.year, bm);

    if (t.sec == t2.sec && t.min == t2.min && t.hour == t2.hour &&
        t.mday == t2.mday && t.mon == t2.mon && t.year == t2.year) {
      ok = 1;
      break;
    }
  }

  // Restore IF.
  if (flags & 0x0200)
    __asm__ volatile("sti");

  if (!ok) {
    printk(LOG_WARN,
           "rtc: read failed, CLOCK_REALTIME falls back to monotonic\n");
    return 0;
  }

  // Sanity-check the decoded fields; a wildly out-of-range value means the
  // CMOS isn't presenting a valid clock (treat as no wall clock).
  if (t.mon < 1 || t.mon > 12 || t.mday < 1 || t.mday > 31 || t.hour > 23 ||
      t.min > 59 || t.sec > 60 || t.year < 1970 || t.year > 2100) {
    printk(LOG_WARN, "rtc: out-of-range %d-%02d-%02d %02d:%02d:%02d\n", t.year,
           t.mon, t.mday, t.hour, t.min, t.sec);
    return 0;
  }

  return rtc_to_epoch(&t);
}
