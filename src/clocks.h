// Clock math for the original Xbox CPU/memory PLL (PCI 0:0.3 register 0x6C).
// Field layout and formulas follow the Cerbios cerbios.ini documentation:
//   coeff = 0x[MEM_DIV][FSB_MULT][PLL_N][PLL_M]
//   FSB = XTAL / M * N,  VCO = FSB * FSB_MULT * 2,  RAM = VCO / (MEM_DIV * 2)
#ifndef CLOCKS_H
#define CLOCKS_H

#define XTAL_MHZ 16.66666
#define STOCK_COEFF 0x230801u   /* 733 / 200 MHz */
#define BGA_COEFF   0x330801u   /* what the 256MB BIOS uses for big chips: RAM 133 MHz */

enum { PROFILE_STOCK = 0, PROFILE_BGA = 1 };

typedef struct {
    unsigned n, m, fm, md;      /* PLL_N, PLL_M, FSB multiplier, memory divider */
    unsigned coeff;             /* 24-bit register value */
    double fsb, vco, ram;       /* MHz */
} clk_t;

typedef struct {
    double max_over;            /* stock profile: highest RAM allowed when the window is unreachable */
    double win_lo, win_hi;      /* stock profile RAM window */
    double max_vco;             /* never use a multiplier/divider pair above this VCO */
    double min_vco;             /* ... or below this one (stock is 800; lower is untested) */
} pick_rules_t;

/* GPU (NV2A core) PLL, NVPLLCoeff = 0x[P][N][M]: GPU = N * XTAL / 2^P / M */
#define GPU_STOCK_COEFF 0x011C01u   /* 233.3 MHz */
double gpu_mhz(unsigned coeff);
unsigned gpu_coeff_for_n(unsigned n);       /* P=1, M=1: 8.33 MHz steps */

unsigned clk_make(unsigned n, unsigned m, unsigned fm, unsigned md);
void clk_decode(unsigned coeff, clk_t *c);

/* FSB steps: start, start-2, ... down to >= 135, then stock 133.33 as the last step */
int fsb_steps_build(double start_mhz, unsigned n_out[], unsigned m_out[], int max);

/* All distinct RAM clocks reachable at this FSB (N/M fixed), sorted by RAM ascending.
   For each ratio the FSB_MULT/MEM_DIV pair with the VCO closest to stock 800 MHz is used.
   Pairs with a VCO outside [min_vco, max_vco] are skipped (0 = no limit). */
int ram_options(unsigned n, unsigned m, clk_t out[], int max, double min_vco, double max_vco);

/* Choose the RAM clock for one FSB step.
   stock profile: highest RAM in [win_lo, min(win_hi,cap)]; else the lowest RAM above win_hi
                  but <= min(cap, max_over); else the highest RAM <= cap.
   BGA profile:   highest RAM <= cap.
   Returns 0 if nothing fits. */
int pick_ram(int profile, unsigned n, unsigned m, double cap, const pick_rules_t *r, clk_t *out);

#endif
