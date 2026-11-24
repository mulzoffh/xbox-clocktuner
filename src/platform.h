#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdint.h>
#include <stddef.h>

uint32_t pll_read(void);                 /* PCI 0:0.3 reg 0x6C, low 24 bits = coefficient */
void pll_write(uint32_t coeff);          /* same two-write sequence the BIOS X-codes use */
uint32_t pfb_cfg1(void);                 /* NV2A memory controller CFG1 (column bits) */
uint64_t rdtsc64(void);
void cache_flush(void);                  /* wbinvd */
uint32_t ticks_ms(void);                 /* KeTickCount */
int measure_cpu_mhz(void);               /* real TSC against the kernel tick, ~250 ms */
int measure_cpu_mhz_patched(void);       /* same with the compiled-in RDTSC (Cerbios may patch it) */
int rtc_measure_cpu_mhz(void);           /* real TSC against the CMOS clock's own crystal, ~2-3 s; 0 if not ticking */
uint32_t rtc_loop_rate(void);
int rtc_minutes(void);                   /* CMOS clock in minutes (for "a few minutes ago"); -1 = unreadable */            /* fixed-loop chunks in one RTC second (no TSC); 0 if not ticking */
uint64_t rdtsc_real(void);               /* RDTSC built at run time, out of reach of load-time patching */
int rdtsc_patched(void);                 /* 1 if the compiled-in RDTSC was changed at load */
int undo_load_patches(int *bytes);      /* puts Cerbios-patched RDTSC sites back as built; places or -1 */
int restore_rdtsc_sites(uint8_t *mem, const uint8_t *file, size_t n, int *bytes);
uint32_t pll_read_raw(void);             /* full register, including the apply bit (24) */
unsigned ram_total_mb(void);
unsigned ram_avail_mb(void);

/* CPU cache ranges (MTRRs) as text, e.g. "WB 0-255MB" */
void mtrr_summary(char *out, size_t max, unsigned ram_mb);

/* files (write-through + flush so state survives a freeze) */
int file_read(const char *path, char *buf, size_t max);       /* returns length or -1 */
int file_write(const char *path, const char *data, size_t len);
int file_exists(const char *path);
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* formatting without %f: 1 decimal place */
const char *mhz(double v);               /* rotating static buffers */
#endif
