#include "clocks.h"

static double fabs_(double x) { return x < 0 ? -x : x; }

unsigned clk_make(unsigned n, unsigned m, unsigned fm, unsigned md)
{
    return ((md & 0xF) << 20) | ((fm & 0xF) << 16) | ((n & 0xFF) << 8) | (m & 0xFF);
}

void clk_decode(unsigned coeff, clk_t *c)
{
    c->coeff = coeff & 0xFFFFFF;
    c->m = coeff & 0xFF;
    c->n = (coeff >> 8) & 0xFF;
    c->fm = (coeff >> 16) & 0xF;
    c->md = (coeff >> 20) & 0xF;
    if (!c->m || !c->n || !c->fm || !c->md) {
        c->fsb = c->vco = c->ram = 0;
        return;
    }
    c->fsb = XTAL_MHZ / c->m * c->n;
    c->vco = c->fsb * c->fm * 2;
    c->ram = c->vco / (c->md * 2);
}

int fsb_steps_build(double start_mhz, unsigned n_out[], unsigned m_out[], int max)
{
    int count = 0;
    for (double t = start_mhz; t >= 135.0 && count < max - 1; t -= 2.0) {
        unsigned bn = 0, bm = 0;
        double best = 1e9;
        for (unsigned m = 1; m <= 8; m++)            /* same ranges as Cerbios Overclock Calc.py */
            for (unsigned n = 1; n <= 63; n++) {
                double f = XTAL_MHZ / m * n, d = fabs_(f - t);
                if (d < best - 1e-9) { best = d; bn = n; bm = m; }
            }
        /* skip duplicates and anything at/below stock */
        double f = XTAL_MHZ / bm * bn;
        if (f <= 134.0) break;
        if (count && n_out[count - 1] * bm == bn * m_out[count - 1]) continue;
        n_out[count] = bn; m_out[count] = bm; count++;
    }
    n_out[count] = 8; m_out[count] = 1; count++;      /* stock FSB 133.33 */
    return count;
}

int ram_options(unsigned n, unsigned m, clk_t out[], int max, double min_vco, double max_vco)
{
    int count = 0;
    double fsb = XTAL_MHZ / m * n;
    for (unsigned fm = 1; fm <= 4; fm++)
        for (unsigned md = 1; md <= 4; md++) {
            clk_t c;
            clk_decode(clk_make(n, m, fm, md), &c);
            if (max_vco > 0 && c.vco > max_vco + 0.05) continue;
            if (min_vco > 0 && c.vco < min_vco - 0.05) continue;
            int dup = -1;
            for (int i = 0; i < count; i++)
                if (out[i].fm * md == fm * out[i].md) { dup = i; break; }   /* same ratio */
            if (dup >= 0) {
                if (fabs_(c.vco - 800.0) < fabs_(out[dup].vco - 800.0)) out[dup] = c;
            } else if (count < max) {
                out[count++] = c;
            }
        }
    (void)fsb;
    for (int i = 1; i < count; i++) {                 /* sort by RAM ascending */
        clk_t k = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].ram > k.ram) { out[j + 1] = out[j]; j--; }
        out[j + 1] = k;
    }
    return count;
}

int pick_ram(int profile, unsigned n, unsigned m, double cap, const pick_rules_t *r, clk_t *out)
{
    clk_t opt[16];
    int cnt = ram_options(n, m, opt, 16, r->min_vco, r->max_vco);
    const double eps = 0.1;   /* MHz: XTAL is 16.66666, so 4/3 x 150 lands at 199.9999 */
    if (profile == PROFILE_STOCK) {
        double hi = r->win_hi < cap ? r->win_hi : cap;
        for (int i = cnt - 1; i >= 0; i--)                       /* highest inside the window */
            if (opt[i].ram >= r->win_lo - eps && opt[i].ram <= hi + eps) { *out = opt[i]; return 1; }
        double over_max = r->max_over < cap ? r->max_over : cap;
        for (int i = 0; i < cnt; i++)                            /* just over the window */
            if (opt[i].ram > r->win_hi + eps && opt[i].ram <= over_max + eps) { *out = opt[i]; return 1; }
    }
    for (int i = cnt - 1; i >= 0; i--)                           /* highest at or under the cap */
        if (opt[i].ram <= cap + eps) { *out = opt[i]; return 1; }
    return 0;
}

double gpu_mhz(unsigned coeff)
{
    unsigned m = coeff & 0xFF, n = (coeff >> 8) & 0xFF, p = (coeff >> 16) & 0x7;
    if (!m || !n) return 0;
    return n * XTAL_MHZ / (double)(1u << p) / m;
}

unsigned gpu_coeff_for_n(unsigned n) { return (1u << 16) | ((n & 0xFF) << 8) | 1u; }
