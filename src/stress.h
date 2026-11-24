#ifndef STRESS_H
#define STRESS_H
#include <stdint.h>

typedef struct {
    uint32_t ram_errors, cpu_errors;
    uint64_t ram_bytes;          /* bytes written + verified */
    uint32_t cpu_loops;
    uint32_t first_bad_addr;     /* physical address of first RAM error */
    int aborted;                 /* user pressed B */
} stress_result_t;

/* progress callback: return nonzero to abort */
typedef int (*stress_cb)(const char *label, uint32_t elapsed_ms, uint32_t total_ms, const stress_result_t *r);

void stress_init(int bga_256);          /* allocates the RAM test regions, computes CPU reference */
unsigned stress_region_mb(unsigned *rank1_mb);
int stress_ref_ok(void);               /* CPU reference self-check passed (see stress_init) */
void stress_free(void);
void stress_run(const char *label, uint32_t ms, stress_cb cb, stress_result_t *r);
#endif
