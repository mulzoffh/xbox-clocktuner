#include "platform.h"
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#define PLL_PCI_ADDR 0x8000036Cu        /* bus 0, device 0, function 3, register 0x6C */
#define NV2A_PFB_CFG1 ((volatile uint32_t *)0xFD100204u)

static inline void outl_(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(p)); }
static inline uint32_t inl_(uint16_t p) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t irq_save(void) { uint32_t f; __asm__ volatile("pushfl; popl %0; cli" : "=r"(f)::"memory"); return f; }
static inline void irq_restore(uint32_t f) { __asm__ volatile("pushl %0; popfl" ::"r"(f) : "memory", "cc"); }

static inline void outb_(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(p)); }
static inline uint8_t inb_(uint16_t p) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }

uint32_t pll_read_raw(void)
{
    uint32_t f = irq_save();
    outl_(0xCF8, PLL_PCI_ADDR);
    uint32_t v = inl_(0xCFC);
    irq_restore(f);
    return v;
}

uint32_t pll_read(void)
{
    uint32_t f = irq_save();
    outl_(0xCF8, PLL_PCI_ADDR);
    uint32_t v = inl_(0xCFC);
    irq_restore(f);
    return v & 0xFFFFFF;
}

void pll_write(uint32_t coeff)
{
    coeff &= 0xFFFFFF;
    uint32_t f = irq_save();
    outl_(0xCF8, PLL_PCI_ADDR);
    outl_(0xCFC, coeff);                 /* load coefficients ... */
    outl_(0xCF8, PLL_PCI_ADDR);
    outl_(0xCFC, coeff | 0x01000000u);   /* ... then apply, as the BIOS does */
    irq_restore(f);
}

uint32_t pfb_cfg1(void) { return *NV2A_PFB_CFG1; }

static uint64_t rdmsr64(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* effective memory type per MB from the variable MTRRs (UC wins, then WT, then WB) */
void mtrr_summary(char *out, size_t max, unsigned ram_mb)
{
    static const char *names[8] = {"UC", "WC", "?2", "?3", "WT", "WP", "WB", "?7"};
    uint64_t def = rdmsr64(0x2FF);
    unsigned vcnt = (unsigned)(rdmsr64(0xFE) & 0xFF);
    if (vcnt > 8) vcnt = 8;
    if (ram_mb > 512) ram_mb = 512;
    unsigned char type[512];
    for (unsigned mb = 0; mb < ram_mb; mb++) {
        uint64_t addr = (uint64_t)mb << 20;
        int t = (def & 0x800) ? (int)(def & 7) : 0, hit = 0, uc = 0, wt = 0;
        for (unsigned i = 0; i < vcnt; i++) {
            uint64_t mask = rdmsr64(0x201 + 2 * i);
            if (!(mask & 0x800)) continue;
            uint64_t base = rdmsr64(0x200 + 2 * i);
            uint64_t m = mask & 0xFFFFFF000ull;
            if ((addr & m) != (base & m)) continue;
            int bt = (int)(base & 7);
            if (!hit) t = bt;
            hit = 1;
            if (bt == 0) uc = 1;
            if (bt == 4) wt = 1;
        }
        if (!(def & 0x800)) t = 0;
        else if (uc) t = 0;
        else if (wt && hit) t = 4;
        type[mb] = (unsigned char)t;
    }
    size_t n = 0;
    out[0] = 0;
    for (unsigned mb = 0; mb < ram_mb && n < max - 20;) {
        unsigned e = mb;
        while (e + 1 < ram_mb && type[e + 1] == type[mb]) e++;
        n += (size_t)snprintf(out + n, max - n, "%s%s %u-%uMB", n ? ", " : "", names[type[mb] & 7], mb, e);
        mb = e + 1;
    }
}

uint64_t rdtsc64(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void cache_flush(void) { __asm__ volatile("wbinvd" ::: "memory"); }

uint32_t ticks_ms(void) { return KeTickCount; }

/* ---- cycle counter that Cerbios can't patch ----
   With an overclock saved, Cerbios (AdvCPUSupport) patches the programs it loads so their RDTSC
   reads as if the CPU ran at stock. rdtsc64() above is compiled into this XBE and gets patched.
   This builds the same instruction at run time in a buffer, where a load-time patch can't see it.
   The bytes are computed from volatile values so 0F 31 never appears in the image. */
static uint8_t tsc_stub[16];
static uint64_t (*tsc_real_fn)(void);

static void tsc_real_init(void)
{
    if (tsc_real_fn) return;
    volatile uint8_t a = 0x0E, b = 0x30, c = 0xC2;
    tsc_stub[0] = (uint8_t)(a + 1);         /* 0F */
    tsc_stub[1] = (uint8_t)(b + 1);         /* 31  rdtsc */
    tsc_stub[2] = (uint8_t)(c + 1);         /* C3  ret (edx:eax = 64-bit return) */
    __asm__ volatile("" ::: "memory");
    tsc_real_fn = (uint64_t (*)(void))(void *)tsc_stub;
}

uint64_t rdtsc_real(void)
{
    tsc_real_init();
    return tsc_real_fn();
}

/* 1 if the RDTSC inside rdtsc64() isn't there any more (patched at load) */
int rdtsc_patched(void)
{
    volatile uint8_t a = 0x0E, b = 0x30;
    const uint8_t *p = (const uint8_t *)(void *)rdtsc64;
    for (int i = 0; i < 24; i++)
        if (p[i] == (uint8_t)(a + 1) && p[i + 1] == (uint8_t)(b + 1)) return 0;
    return 1;
}

static int tick_measure(uint64_t (*ctr)(void))
{
    uint32_t t = ticks_ms();
    while (ticks_ms() == t) {}
    uint32_t t0 = ticks_ms();
    uint64_t c0 = ctr();
    while (ticks_ms() - t0 < 250) {}
    uint64_t c1 = ctr();
    uint32_t dt = ticks_ms() - t0;
    return (int)((c1 - c0) / ((uint64_t)dt * 1000u));
}

int measure_cpu_mhz(void) { return tick_measure(rdtsc_real); }
int measure_cpu_mhz_patched(void) { return tick_measure(rdtsc64); }

/* CPU speed timed against the CMOS real-time clock, which runs from its own 32.768 kHz crystal
   (nothing to do with the CPU/RAM PLL). Times two whole RTC seconds: about 2-3 s. 0 = RTC not ticking. */
static uint8_t cmos_read(uint8_t reg)
{
    uint32_t f = irq_save();
    outb_(0x70, reg);
    uint8_t v = inb_(0x71);
    irq_restore(f);
    return v;
}

/* waits for the next RTC second to start; -1 if it doesn't within the time left */
/* the CMOS clock as minutes since the start of the month-of-year grid (good enough to tell
   "a few minutes ago"); -1 if it can't be read */
int rtc_minutes(void)
{
    for (int tries = 0; tries < 1000; tries++) {
        if (cmos_read(0x0A) & 0x80) continue;        /* update in progress */
        int mi = cmos_read(0x02), h = cmos_read(0x04), d = cmos_read(0x07), mo = cmos_read(0x08);
        if (cmos_read(0x0A) & 0x80) continue;
        if (!(cmos_read(0x0B) & 0x04)) {             /* BCD */
            mi = (mi >> 4) * 10 + (mi & 15);
            d = (d >> 4) * 10 + (d & 15);
            mo = (mo >> 4) * 10 + (mo & 15);
            int pm = h & 0x80;
            h = ((h & 0x7F) >> 4) * 10 + (h & 15);
            if (pm) h = (h % 12) + 12;               /* 12-hour mode */
        }
        if (mi > 59 || h > 23 || d < 1 || d > 31 || mo < 1 || mo > 12) return -1;
        return (((mo * 31 + d) * 24) + h) * 60 + mi;
    }
    return -1;
}

static int rtc_next_second(int last, uint32_t start)
{
    while (KeTickCount - start < 4000) {
        if (cmos_read(0x0A) & 0x80) continue;        /* update in progress: seconds not valid */
        int sec = cmos_read(0x00);
        if (cmos_read(0x0A) & 0x80) continue;
        if (last >= 0 && sec != last) return sec;
        last = sec;
    }
    return -1;
}

int rtc_measure_cpu_mhz(void)
{
    uint32_t start = KeTickCount;
    int s = rtc_next_second(-1, start);
    if (s < 0) return 0;
    uint64_t c0 = rdtsc_real();
    s = rtc_next_second(s, start);
    if (s < 0) return 0;
    s = rtc_next_second(s, start);
    if (s < 0) return 0;
    uint64_t c1 = rdtsc_real();
    return (int)((c1 - c0 + 1000000u) / 2000000u);
}

/* A fixed loop counted for one RTC second: uses no cycle counter at all. Its rate is proportional
   to the CPU clock, so (rate / rate at stock) * stock MHz is a second, independent measurement. */
static void spin_chunk(void)
{
    uint32_t n = 200000;
    __asm__ volatile("1: dec %0\n\tjnz 1b" : "+r"(n));
}

uint32_t rtc_loop_rate(void)
{
    uint32_t start = KeTickCount;
    int s = rtc_next_second(-1, start);
    if (s < 0) return 0;
    uint32_t chunks = 0;
    for (;;) {
        spin_chunk();
        chunks++;
        if (cmos_read(0x0A) & 0x80) continue;
        int now = cmos_read(0x00);
        if (now != s) break;
        if (KeTickCount - start > 4000) return 0;
    }
    return chunks;
}

/* ---- undo load-time patches ----
   With an overclock saved, Cerbios (AdvCPUSupport) rewrites every 0F 31 (RDTSC) byte pair in a
   program's code as it loads. In this program that hits nxdk's QueryPerformanceCounter, which SDL
   times its input with (the controller stopped working on overclocked boots), and also byte pairs
   that aren't RDTSC at all, made of two neighbouring instructions. This puts our code back exactly
   as built, from the XBE file on disk. Only bytes within a few bytes of a 0F 31 pair in the file are
   touched, so nothing else the loader set up is changed. */

/* pure part (host-tested): restores mem[] from file[] around every 0F 31 pair in file[].
   Returns the number of places that differed; *bytes = bytes written. */
int restore_rdtsc_sites(uint8_t *mem, const uint8_t *file, size_t n, int *bytes)
{
    volatile uint8_t a = 0x0E, b = 0x30;
    const uint8_t op0 = (uint8_t)(a + 1), op1 = (uint8_t)(b + 1);
    int places = 0;
    *bytes = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        if (file[i] != op0 || file[i + 1] != op1) continue;
        size_t lo = i >= 8 ? i - 8 : 0, hi = i + 10 < n ? i + 10 : n;
        int changed = 0;
        for (size_t j = lo; j < hi; j++)
            if (mem[j] != file[j]) { mem[j] = file[j]; (*bytes)++; changed = 1; }
        places += changed;
    }
    return places;
}

#ifndef HOST_TEST
static int read_at(HANDLE h, uint32_t off, void *buf, uint32_t len)
{
    LARGE_INTEGER pos;
    pos.QuadPart = off;
    if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) return 0;
    DWORD got = 0, total = 0;
    while (total < len && ReadFile(h, (uint8_t *)buf + total, len - total, &got, NULL) && got) total += got;
    return total == len;
}

int undo_load_patches(int *bytes)
{
    *bytes = 0;
    const char *img = XeImageFileName->Buffer;
    int len = XeImageFileName->Length, k = len;
    while (k > 0 && img[k - 1] != '\\') k--;
    char path[300];
    snprintf(path, sizeof(path), "D:\\%.*s", len - k, img + k);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    uint32_t hdr[0x128 / 4];
    if (!read_at(h, 0, hdr, sizeof(hdr)) || hdr[0] != 0x48454258u /* "XBEH" */) { CloseHandle(h); return -1; }
    uint32_t base = hdr[0x104 / 4], nsec = hdr[0x11C / 4], secs = hdr[0x120 / 4];
    if (nsec == 0 || nsec > 64 || secs < base) { CloseHandle(h); return -1; }

    int places = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t sh[6];                              /* flags, vaddr, vsize, raw addr, raw size, name */
        if (!read_at(h, secs - base + i * 56, sh, sizeof(sh))) break;
        if (!(sh[0] & 4)) continue;                  /* executable sections only */
        uint32_t n = sh[2] < sh[4] ? sh[2] : sh[4];
        if (!n) continue;
        uint8_t *file = (uint8_t *)malloc(n);
        if (!file) continue;
        if (read_at(h, sh[3], file, n)) {
            uint8_t *mem = (uint8_t *)(uintptr_t)sh[1];
            ULONG old = MmQueryAddressProtect(mem);
            MmSetAddressProtect(mem, n, PAGE_READWRITE);
            int b = 0;
            places += restore_rdtsc_sites(mem, file, n, &b);
            *bytes += b;
            if (old) MmSetAddressProtect(mem, n, old);
        }
        free(file);
    }
    CloseHandle(h);
    __asm__ volatile("cpuid" ::"a"(0) : "ebx", "ecx", "edx", "memory");   /* serialize before running restored code */
    return places;
}
#endif

unsigned ram_total_mb(void)
{
    MM_STATISTICS s;
    s.Length = sizeof(s);
    MmQueryStatistics(&s);
    return (unsigned)(((uint64_t)s.TotalPhysicalPages * 4096u + (1u << 19)) >> 20);
}

unsigned ram_avail_mb(void)
{
    MM_STATISTICS s;
    s.Length = sizeof(s);
    MmQueryStatistics(&s);
    return (unsigned)(((uint64_t)s.AvailablePages * 4096u) >> 20);
}

int file_read(const char *path, char *buf, size_t max)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0, total = 0;
    while (total < max - 1 && ReadFile(h, buf + total, (DWORD)(max - 1 - total), &got, NULL) && got) total += got;
    CloseHandle(h);
    buf[total] = 0;
    return (int)total;
}

int file_write(const char *path, const char *data, size_t len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    BOOL ok = WriteFile(h, data, (DWORD)len, &put, NULL) && put == len;
    IO_STATUS_BLOCK io;
    NtFlushBuffersFile(h, &io);
    CloseHandle(h);
    return ok;
}

int file_exists(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

void log_line(const char *fmt, ...)
{
    static char buf[24576];
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    int n = file_read("D:\\log.txt", buf, sizeof(buf));
    if (n < 0) n = 0;
    size_t ll = strlen(line);
    if ((size_t)n + ll + 3 >= sizeof(buf)) {          /* keep the newest half */
        char *cut = strchr(buf + n / 2, '\n');
        size_t keep = cut ? (size_t)n - (size_t)(cut + 1 - buf) : 0;
        memmove(buf, cut ? cut + 1 : buf, keep);
        n = (int)keep;
    }
    memcpy(buf + n, line, ll);
    n += (int)ll;
    buf[n++] = '\r';
    buf[n++] = '\n';
    file_write("D:\\log.txt", buf, (size_t)n);
}

const char *mhz(double v)
{
    static char ring[8][16];
    static int k;
    char *b = ring[k++ & 7];
    int t = (int)(v * 10.0 + (v >= 0 ? 0.5 : -0.5));
    snprintf(b, 16, "%d.%d", t / 10, (t < 0 ? -t : t) % 10);
    return b;
}
