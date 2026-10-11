/* The two read-only pages every 3DS process can read without a system call:
 *
 *   0x1FF80000  kernel configuration: kernel/firmware versions, the memory
 *               layout (APPMEMTYPE and region sizes), boot environment
 *   0x1FF81000  shared configuration: the time reference that osGetTime()
 *               works from, the 3D slider, LEDs, Wi-Fi state, headset
 *
 * libctru reads them directly (osGetFirmVersion, osGetTime, time(),
 * osGet3DSliderState, ...). A zeroed page breaks things quietly: osGetTime
 * divides by the clock rate stored here. Values are those of an original
 * 3DS on firmware 11.17, with the clock starting at 2026-01-01 00:00:00 so
 * runs stay deterministic. Layouts from libctru's os.h / 3dbrew. */
#include <string.h>

#include "kernel.h"

#define KERNEL_CONFIG 0x1FF80000u
#define SHARED_CONFIG 0x1FF81000u

/* Milliseconds from 1900-01-01 to 2026-01-01. */
#define START_MS 3976214400000ull

static void put8(uint8_t *page, uint32_t off, uint8_t v) { page[off] = v; }
static void put32(uint8_t *page, uint32_t off, uint32_t v) { memcpy(page + off, &v, 4); }
static void put64(uint8_t *page, uint32_t off, uint64_t v) { memcpy(page + off, &v, 8); }

void config_mem_init(uint8_t *mem)
{
    uint8_t *k = mem + KERNEL_CONFIG;
    memset(k, 0, 0x1000);
    uint32_t version = 2u << 24 | 58u << 16;    /* kernel 2.58-0 (firmware 11.17) */
    put32(k, 0x00, version);                    /* kernel_ver */
    put32(k, 0x10, 2);                          /* kernel_syscore_ver */
    put8(k, 0x14, 1);                           /* env_info: retail */
    put8(k, 0x15, 0x13);                        /* unit_info: retail unit */
    put32(k, 0x30, 0);                          /* app_memtype 0: 64 MiB for applications */
    put32(k, 0x40, 0x04000000);                 /* memregion sizes: APPLICATION, */
    put32(k, 0x44, 0x02C00000);                 /*   SYSTEM, */
    put32(k, 0x48, 0x01400000);                 /*   BASE */
    put32(k, 0x60, version);                    /* firm_ver */
    put32(k, 0x64, 2);                          /* firm_syscore_ver */

    uint8_t *s = mem + SHARED_CONFIG;
    memset(s, 0, 0x1000);
    put32(s, 0x00, 0);                          /* timeref_cnt: timeref[0] is current */
    put8(s, 0x04, 1);                           /* running_hw: a retail product */
    put64(s, 0x20, START_MS);                   /* timeref[0].value_ms */
    put64(s, 0x28, 0);                          /*   .value_tick: taken at boot */
    put64(s, 0x30, TICKS_PER_SEC);              /*   .sysclock_hz */
    put64(s, 0x38, 0);                          /*   .drift_ms */
    put8(s, 0x67, 7);                           /* network_state: Wi-Fi off */
    put8(s, 0x85, 1);                           /* led_battery: on, not charging */
}
