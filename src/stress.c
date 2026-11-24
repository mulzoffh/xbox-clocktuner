#include "stress.h"
#include "platform.h"
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <string.h>

#define CHUNK (2u << 20)
#define MAXREG 160

static volatile uint32_t *reg_p[MAXREG];
static uint32_t reg_phys[MAXREG];
static int nreg;
static unsigned rank1_chunks;
static volatile uint32_t sink;

/* ---------- RAM regions ---------- */
static int grab(ULONG_PTR lo, ULONG_PTR hi, unsigned max_chunks)
{
    unsigned got = 0;
    while (nreg < MAXREG && got < max_chunks) {
        if (ram_avail_mb() < 12) break;             /* leave room for the kernel, USB, video */
        void *p = MmAllocateContiguousMemoryEx(CHUNK, lo, hi, 4096, PAGE_READWRITE);
        if (!p) break;
        reg_p[nreg] = (volatile uint32_t *)p;
        reg_phys[nreg] = (uint32_t)MmGetPhysicalAddress(p);
        nreg++; got++;
    }
    return (int)got;
}

/* ---------- CPU workloads (deterministic) ---------- */
static uint32_t crc_tab[256];
static uint8_t crc_buf[32768];
static double ma[24][24], mb[24][24];
static uint32_t ref_crc, ref_fpu_lo, ref_fpu_hi, ref_mat_lo, ref_mat_hi;
static int ref_ok;
#define CRC_EXPECTED 0xAB9DA751u     /* work_crc(0x1234567), computed on a PC */

static uint32_t work_crc(uint32_t seed)
{
    uint32_t c = ~seed;
    for (int pass = 0; pass < 4; pass++)
        for (unsigned i = 0; i < sizeof(crc_buf); i++) c = crc_tab[(c ^ crc_buf[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static double work_fpu(volatile double start)
{
    double x = start, acc = 0.0;
    for (int i = 1; i <= 20000; i++) {
        x = x * 1.0000001 + 1.0 / (double)(i + 7);
        acc += x * 0.5 - x / (double)(i + 3);
    }
    return acc;
}

static double work_mat(volatile double k)
{
    double s = 0.0;
    for (int i = 0; i < 24; i++)
        for (int j = 0; j < 24; j++) {
            double t = 0.0;
            for (int q = 0; q < 24; q++) t += ma[i][q] * mb[q][j] * k;
            s += t * (double)(i + 1) / (double)(j + 2);
        }
    return s;
}

static void dbits(double d, uint32_t *lo, uint32_t *hi)
{
    uint32_t w[2];
    memcpy(w, &d, 8);
    *lo = w[0];
    *hi = w[1];
}

static int cpu_iteration(void)
{
    uint32_t lo, hi;
    int bad = 0;
    if (work_crc(0x1234567u) != ref_crc) bad++;
    dbits(work_fpu(1.0), &lo, &hi);
    if (lo != ref_fpu_lo || hi != ref_fpu_hi) bad++;
    dbits(work_mat(1.5), &lo, &hi);
    if (lo != ref_mat_lo || hi != ref_mat_hi) bad++;
    return bad;
}

void stress_init(int bga_256)
{
    nreg = 0;
    rank1_chunks = 0;
    unsigned total = ram_total_mb();
    ULONG_PTR top = (ULONG_PTR)total * 1024u * 1024u - 1u;
    if (bga_256 && total >= 250) {
        /* make sure the second rank (above 128MB) gets tested */
        rank1_chunks = (unsigned)grab(0x08000000u, top, 60);
        grab(0, 0x07FFFFFFu, 30);
    } else {
        grab(0, top, MAXREG);
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
    uint32_t s = 0xC0FFEEu;
    for (unsigned i = 0; i < sizeof(crc_buf); i++) { s = s * 1103515245u + 12345u; crc_buf[i] = (uint8_t)(s >> 16); }
    for (int i = 0; i < 24; i++)
        for (int j = 0; j < 24; j++) { ma[i][j] = (double)((i * 7 + j * 3) % 17) / 7.0; mb[i][j] = (double)((i * 5 + j * 11) % 13) / 3.0; }
    /* reference results at the clocks the console booted with. When those clocks are the ones
       under test (boot test), check them: the integer result must match the PC-computed value
       and five repeats of every workload must agree. */
    ref_crc = work_crc(0x1234567u);
    dbits(work_fpu(1.0), &ref_fpu_lo, &ref_fpu_hi);
    dbits(work_mat(1.5), &ref_mat_lo, &ref_mat_hi);
    ref_ok = ref_crc == CRC_EXPECTED;
    for (int i = 0; i < 4; i++)
        if (cpu_iteration()) ref_ok = 0;
}

int stress_ref_ok(void) { return ref_ok; }

unsigned stress_region_mb(unsigned *rank1_mb)
{
    if (rank1_mb) *rank1_mb = rank1_chunks * (CHUNK >> 20);
    return (unsigned)nreg * (CHUNK >> 20);
}

void stress_free(void)
{
    for (int i = 0; i < nreg; i++) MmFreeContiguousMemory((void *)reg_p[i]);
    nreg = 0;
}

static inline uint32_t xs(uint32_t s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

/* one region: random fill, verify, inverted fill, verify */
static void ram_region(int r, uint32_t seed, stress_result_t *res)
{
    volatile uint32_t *p = reg_p[r];
    const uint32_t words = CHUNK / 4;
    uint32_t s = seed ^ (uint32_t)r * 0x9E3779B9u;
    if (!s) s = 1;
    for (int inv = 0; inv < 2; inv++) {
        uint32_t t = s, x = inv ? 0xFFFFFFFFu : 0;
        for (uint32_t i = 0; i < words; i++) { t = xs(t); p[i] = t ^ x; }
        cache_flush();
        t = s;
        for (uint32_t i = 0; i < words; i++) {
            t = xs(t);
            if (p[i] != (t ^ x)) {
                if (!res->ram_errors) res->first_bad_addr = reg_phys[r] + i * 4;
                res->ram_errors++;
            }
        }
        res->ram_bytes += (uint64_t)CHUNK * 2;
    }
}

void stress_run(const char *label, uint32_t ms, stress_cb cb, stress_result_t *r)
{
    memset(r, 0, sizeof(*r));
    uint32_t t0 = ticks_ms(), last_ui = 0, seed = 0x13579BDFu ^ t0;
    int region = 0;
    while (1) {
        uint32_t el = ticks_ms() - t0;
        if (el - last_ui >= 500 || el >= ms) {
            last_ui = el;
            if (cb && cb(label, el > ms ? ms : el, ms, r)) { r->aborted = 1; return; }
        }
        if (el >= ms) break;
        if (nreg) {
            ram_region(region, seed, r);
            if (++region >= nreg) { region = 0; seed = xs(seed + 0x6D2B79F5u); }
        }
        r->cpu_errors += (uint32_t)cpu_iteration();
        r->cpu_loops++;
        if (r->cpu_errors > 50 || r->ram_errors > 100000) {   /* clearly failing: stop early */
            if (cb) cb(label, ticks_ms() - t0, ms, r);
            break;
        }
    }
    sink = r->cpu_loops;
}
