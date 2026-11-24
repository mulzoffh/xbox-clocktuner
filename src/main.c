/*
 * ClockTuner - automatic CPU/RAM clock tuning for the original Xbox with Cerbios.
 *
 * Manual clock setting only. Clocks are never changed while it runs: changing the PLL live makes
 * the RAM lose sync and crashes the console. A clock you pick is saved to cerbios.ini, the
 * console restarts so Cerbios applies it at start-up, and then the chosen test runs. While it is
 * checking clocks it is Cerbios' boot dashboard (DashPath), so it picks up again after the
 * restart or a freeze; when it is done it hands DashPath back to the real dashboard.
 *
 * Each start it also writes the NV2A memory controller and bus registers to pfb_dump.txt
 * (read only), for working out the memory timing registers.
 *
 * The CPU speed is measured with an RDTSC built at run time (Cerbios patches the ones it finds
 * in programs on overclocked consoles) against the CMOS clock, and checked with a fixed loop.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <nxdk/mount.h>
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "clocks.h"
#include "platform.h"
#include "settings.h"
#include "stress.h"
#include <stdarg.h>

#define VERSION "1.8.1"
#define CFG_PATH "D:\\tuner.ini"
#define STATE_PATH "D:\\state.ini"
#define MAXSTEP 80

enum { B_A = 1, B_B = 2, B_X = 4, B_Y = 8, B_START = 16, B_BACK = 32, B_UP = 64, B_DOWN = 128,
       B_LEFT = 256, B_RIGHT = 512, B_WHITE = 1024, B_BLACK = 2048 };

static kv_t cfg, st;
static int profile;                 /* PROFILE_STOCK / PROFILE_BGA */
static unsigned ram_mb;
static double cpu_mult = 5.5;
static clk_t boot_clk;              /* clocks this boot came up with */
static unsigned default_coeff;      /* what the BIOS uses with Overclocking = False */
static unsigned step_n[MAXSTEP], step_m[MAXSTEP];
static int nsteps;
static pick_rules_t rules;
static char note[160];              /* one-line message shown under the header */
static int have_cini;
static char self_path[200];         /* this XBE in Cerbios DashPath form */
static int stress_ready;
#define NVPLL ((volatile uint32_t *)0xFD680500u)
static unsigned boot_gpu;           /* NVPLL coefficient this boot came up with */
static int meas_tick, meas_rtc;     /* CPU MHz at boot: against the kernel tick / the CMOS clock */
static int measured;                /* the one we trust: RTC if it ticks, else the tick */
static int speed_ok = 1;            /* measured CPU matches what the PLL setting should give */
static int undo_places, undo_bytes; /* Cerbios load-time patches put back at start (-1: couldn't) */
static int top_fsb_capped;          /* StartFSB is above the highest FSB the VCO limits allow */
static int loop_mhz;                /* fixed loop vs RTC, scaled from a stock-clock calibration; 0 = none yet */

/* ------------------------------------------------------------------ input */
static SDL_GameController *pads[4];

static int map_button(int b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return B_A;
    case SDL_CONTROLLER_BUTTON_B: return B_B;
    case SDL_CONTROLLER_BUTTON_X: return B_X;
    case SDL_CONTROLLER_BUTTON_Y: return B_Y;
    case SDL_CONTROLLER_BUTTON_START: return B_START;
    case SDL_CONTROLLER_BUTTON_BACK: return B_BACK;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return B_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return B_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return B_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return B_RIGHT;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return B_WHITE;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return B_BLACK;
    }
    return 0;
}

/* returns buttons newly pressed since the last call */
static int poll_pressed(void)
{
    SDL_Event e;
    int pressed = 0;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_CONTROLLERDEVICEADDED) {
            for (int i = 0; i < 4; i++) if (!pads[i]) { pads[i] = SDL_GameControllerOpen(e.cdevice.which); break; }
        } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
            SDL_GameController *g = SDL_GameControllerFromInstanceID(e.cdevice.which);
            for (int i = 0; i < 4; i++) if (pads[i] == g) pads[i] = NULL;
            if (g) SDL_GameControllerClose(g);
        } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            pressed |= map_button(e.cbutton.button);
        }
    }
    return pressed;
}

/* ------------------------------------------------------------------ screen */
static const char *coeff_line(const clk_t *c)
{
    static char b[2][96];
    static int k;
    char *o = b[k++ & 1];
    snprintf(o, 96, "FSB %s  CPU %s  RAM %s  VCO %s  (0x%06X)", mhz(c->fsb), mhz(c->fsb * cpu_mult),
             mhz(c->ram), mhz(c->vco), c->coeff);
    return o;
}

extern int nextRow;                 /* nxdk debug console cursor (pixels) */
#define CON_X 25
#define PROMPT_Y 410

/* ------------------------------------------------------------------ RAM row timings */
/* The low two bytes of the NV2A's 0x100220: 12 and 14 cycles at power-on on a stock 1.6. Going by
   the next NVIDIA generation's layout they're tRC (row cycle) and tRFC (refresh). The BIOS never
   writes this register, so it holds its power-on value until something changes it, and a change
   lasts until the console is switched off (the dashboard starting a game doesn't reset it).
   ClockTuner adds the saved number of cycles to both whenever it starts. While cycles are saved
   it stays Cerbios' boot dashboard, so that happens at every power-on, then it starts the real
   dashboard straight away. Loosening them live has been safe. */
#define PFB(off) (*(volatile uint32_t *)(uintptr_t)(0xFD100000u + (off)))
#define ROW_MAX 16

static uint32_t row_base(void)                 /* power-on low 16 bits: tRFC << 8 | tRC */
{
    return (uint32_t)kv_hex(&st, "RowBase", PFB(0x220) & 0xFFFF) & 0xFFFF;
}
static int row_running(void)
{
    int d = (int)(PFB(0x220) & 0xFF) - (int)(row_base() & 0xFF);
    return d < 0 ? 0 : d;
}
static int row_saved(void)
{
    int e = (int)kv_int(&st, "RowExtra", 0);
    return e < 0 ? 0 : e > ROW_MAX ? ROW_MAX : e & ~1;
}
static void row_set(int extra)
{
    uint32_t b = row_base();
    uint32_t rc = (b & 0xFF) + (uint32_t)extra, rfc = ((b >> 8) & 0xFF) + (uint32_t)extra;
    if (rc > 0xFF) rc = 0xFF;
    if (rfc > 0xFF) rfc = 0xFF;
    uint32_t v = (PFB(0x220) & 0xFFFF0000u) | (rfc << 8) | rc;
    if (PFB(0x220) != v) PFB(0x220) = v;
}

static void history_add(const char *fmt, const clk_t *c, const char *result)
{
    char line[160];
    (void)fmt;
    char rt[12] = "";
    if (row_running()) snprintf(rt, sizeof(rt), " +%d", row_running());
    snprintf(line, sizeof(line), "FSB %s CPU %d RAM %d%s  %s", mhz(c->fsb), (int)(c->fsb * cpu_mult + 0.5),
             (int)(c->ram + 0.5), rt, result);
    for (int i = 7; i > 0; i--) {                    /* History1 = newest */
        char kf[16], kt[16];
        snprintf(kf, sizeof(kf), "History%d", i);
        snprintf(kt, sizeof(kt), "History%d", i + 1);
        kv_set(&st, kt, kv_get(&st, kf, ""));
    }
    kv_set(&st, "History1", line);
    log_line("%s", line);
}

/* Screen output for menus. nxdk's console wraps a line once it passes 62 characters (a 62-character
   line plus its newline then takes two rows) and clears the whole screen when text reaches the
   bottom, which is what blanked the menus once the result list grew. scr() wraps long lines at a
   space itself and drops anything that would run into the prompt row, so a screen can't overflow. */
extern int nextCol;
#define CON_RIGHT 590                  /* nxdk wraps once a character would start past this */

static void scr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void scr(const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char *s = buf;
    while (*s) {
        if (nextCol <= CON_X && nextRow + 16 > PROMPT_Y) return;   /* full: keep the prompt row clear */
        const char *nl = strchr(s, '\n');
        int len = nl ? (int)(nl - s) : (int)strlen(s);
        int room = (CON_RIGHT - nextCol) / 9;
        if (room < 1) room = 1;
        char out[80];
        if (len <= room) {
            memcpy(out, s, (size_t)len);
            out[len] = 0;
            debugPrint("%s%s", out, nl ? "\n" : "");
            s += len + (nl ? 1 : 0);
        } else {
            int cut = room;
            for (int i = room; i > room / 2; i--) if (s[i] == ' ') { cut = i; break; }
            memcpy(out, s, (size_t)cut);
            out[cut] = 0;
            debugPrint("%s\n", out);
            s += cut;
            while (*s == ' ') s++;
        }
    }
}

static void header(const char *title)
{
    debugClearScreen();
    scr("CLOCK TUNER %s  -  %.36s\n", VERSION, title);
    scr("RAM %uMB %s  CPU x%s  GPU %s  measured CPU %d%s\n", ram_mb,
        profile == PROFILE_BGA ? "BGA" : "stock", mhz(cpu_mult), mhz(gpu_mhz(boot_gpu)), measured,
        speed_ok ? "" : " (!)");
    scr("Now: %s\n", coeff_line(&boot_clk));
    if (note[0]) scr("%.62s\n", note);
    scr("--------------------------------------------------------------\n");
}

/* the last few results, one row each (cut to fit) */
static void show_history_n(int lines)
{
    if (nextRow + 2 * 17 + 16 > PROMPT_Y) return;      /* no room for the title and one entry */
    scr("Results (newest first):\n");
    for (int i = 1; i <= lines; i++) {
        char k[16];
        snprintf(k, sizeof(k), "History%d", i);
        const char *h = kv_get(&st, k, "");
        if (h[0]) scr(" %.61s\n", h);
    }
}

static void show_history(void) { show_history_n(3); }

/* RAM timings as far as they're known. CAS latency and burst length come from the mode register
   value the BIOS sent to the chips (PFB 0x1002C0; Cerbios writes 0x132 = CL3, burst 4, DLL reset).
   The NV2A's own timing registers (0x100220-0x10022C) hold tRCD/tRP/tRAS and the rest, but their
   layout isn't documented, so they're shown raw. */
static const char *mrs_cl(uint32_t mr)
{
    switch ((mr >> 4) & 7) {
    case 2: return "2";
    case 3: return "3";
    case 4: return "4";
    case 5: return "5";
    case 6: return "2.5";
    default: return NULL;
    }
}

static void ram_timing_lines(double ram_mhz)
{
    volatile const uint32_t *pfb = (volatile const uint32_t *)0xFD100000u;
    uint32_t mr = pfb[0x2C0 / 4];
    const char *cl = mrs_cl(mr);
    int bl = 1 << (mr & 7);                         /* 1 = 2, 2 = 4, 3 = 8 */
    if (cl && (mr & 7) >= 1 && (mr & 7) <= 3) {
        /* no %f anywhere: nxdk's printf doesn't convert floating point (it hangs); use mhz() */
        double clv = cl[1] ? 2.5 : (double)(cl[0] - '0');
        scr("RAM: CL%s = %s ns at %s MHz, burst %d (mode 0x%03X)\n", cl,
                   mhz(ram_mhz > 1.0 ? clv * 1000.0 / ram_mhz : 0.0), mhz(ram_mhz), bl, (unsigned)(mr & 0xFFF));
    } else {
        scr("RAM: CL3 (BIOS value) = %s ns at %s MHz\n", mhz(ram_mhz > 1.0 ? 3000.0 / ram_mhz : 0.0), mhz(ram_mhz));
    }
    scr("Row timings: tRC %u tRFC %u (+%d running, +%d saved)\n", (unsigned)(pfb[0x220 / 4] & 0xFF),
        (unsigned)((pfb[0x220 / 4] >> 8) & 0xFF), row_running(), row_saved());
}

/* waits for one of `mask`; after `auto_s` seconds (0 = never) returns `def` */
static int prompt(const char *text, int mask, int auto_s, int def)
{
    uint32_t t0 = ticks_ms();
    int shown = -1;
    poll_pressed();
    while (1) {
        int left = auto_s ? auto_s - (int)((ticks_ms() - t0) / 1000) : -1;
        if (left != shown) {
            shown = left;
            debugMoveCursor(CON_X, PROMPT_Y);
            debugPrint("%-50.50s", text);
            if (auto_s) debugPrint(" auto %2ds", left > 0 ? left : 0);
            debugPrint("\n");
        }
        int p = poll_pressed() & mask;
        if (p) {
            for (int b = 1; b <= B_BLACK; b <<= 1) if (p & b) return b;
        }
        if (auto_s && left <= 0) return def;
        Sleep(30);
    }
}

static int same_clock(unsigned a, unsigned b)
{
    clk_t x, y;
    clk_decode(a, &x);
    clk_decode(b, &y);
    double df = x.fsb - y.fsb, dr = x.ram - y.ram;
    return df < 0.5 && df > -0.5 && dr < 0.5 && dr > -0.5;
}

static int same_gpu(unsigned a, unsigned b)
{
    double d = gpu_mhz(a) - gpu_mhz(b);
    return d < 1.0 && d > -1.0;
}

/* RAM rules in use: BGA chips first find the CPU limit with RAM <= 133, then (phase RAMUP)
   use the same RAM window as stock chips, never above the FSB step that passed */
static int ram_mode(void);

/* ------------------------------------------------------------------ state */
static void save_state(void) { kv_save(&st, STATE_PATH, "; ClockTuner state - delete this file to start over\r\n"); }

static const char *phase(void) { return kv_get(&st, "Phase", ""); }
static void set_phase(const char *p) { kv_set(&st, "Phase", p); }

static void reset_state(void)
{
    static kv_t keep;
    keep = st;
    memset(&st, 0, sizeof(st));
    for (int i = 1; i <= 8; i++) {                   /* keep the result history */
        char k[16];
        snprintf(k, sizeof(k), "History%d", i);
        kv_set(&st, k, kv_get(&keep, k, ""));
    }
    kv_set(&st, "GpuCommitted", kv_get(&keep, "GpuCommitted", "0x011C01"));
    kv_set(&st, "CpuMult10", kv_get(&keep, "CpuMult10", "55"));
    kv_set(&st, "LoopRate", kv_get(&keep, "LoopRate", "0"));
    if (kv_get(&keep, "RowBase", NULL)) kv_set(&st, "RowBase", kv_get(&keep, "RowBase", ""));
    kv_set(&st, "RowExtra", kv_get(&keep, "RowExtra", "0"));
    kv_seti(&st, "Profile", profile);
    kv_seti(&st, "RamMB", (long)ram_mb);
    set_phase("MANUAL");                            /* first start: manual screen */
    kv_seti(&st, "Step", 0);
    kv_seti(&st, "Pending", 0);
    kv_setx(&st, "Committed", default_coeff);
    if (!kv_get(&st, "GpuCommitted", NULL)) kv_setx(&st, "GpuCommitted", GPU_STOCK_COEFF);
    save_state();
}

static int ram_mode(void)
{
    return (profile == PROFILE_STOCK || !strcmp(kv_get(&st, "Phase", ""), "RAMUP")) ? PROFILE_STOCK : PROFILE_BGA;
}


/* ------------------------------------------------------------------ cerbios.ini */
/* writes CPU/RAM and GPU clocks to cerbios.ini; stock values are written as 0x000000 (= leave alone) */
static int commit_clock(const clk_t *c, unsigned gpu)
{
    if (!have_cini) return 0;
    int cpu_oc = !same_clock(c->coeff, default_coeff), gpu_oc = !same_gpu(gpu, GPU_STOCK_COEFF);
    char v[16], g[16];
    snprintf(v, sizeof(v), "0x%06X", cpu_oc ? c->coeff : 0);
    snprintf(g, sizeof(g), "0x%06X", gpu_oc ? gpu : 0);
    cini_set("Overclocking", (cpu_oc || gpu_oc) ? "True" : "False");
    cini_set("CPUMPLLCoeff", v);
    cini_set("NVPLLCoeff", g);
    /* the next boot has to come back here to check these clocks; the "Done" screen (or BACK)
       gives DashPath back to the real dashboard */
    if (self_path[0] && kv_get(&cfg, "RealDash", "")[0]) cini_set("DashPath", self_path);
    if (!cini_save()) return 0;
    kv_setx(&st, "Committed", cpu_oc ? c->coeff : default_coeff);
    kv_setx(&st, "GpuCommitted", gpu_oc ? gpu : GPU_STOCK_COEFF);
    log_line("Saved: Overclocking=%s CPUMPLLCoeff=%s NVPLLCoeff=%s", (cpu_oc || gpu_oc) ? "True" : "False", v, g);
    return 1;
}

static unsigned gpu_saved(void) { return (unsigned)kv_hex(&st, "GpuCommitted", GPU_STOCK_COEFF); }

/* last clocks that passed a test at boot (stock until something has) */
static unsigned good_coeff(void) { return (unsigned)kv_hex(&st, "GoodCoeff", default_coeff); }
static unsigned good_gpu(void) { return (unsigned)kv_hex(&st, "GoodGpu", GPU_STOCK_COEFF); }
static void mark_good(void)
{
    kv_setx(&st, "GoodCoeff", (unsigned)kv_hex(&st, "Committed", default_coeff));
    kv_setx(&st, "GoodGpu", gpu_saved());
}

/* saves the last good clocks again (after the saved ones failed) */
static void commit_good(void)
{
    clk_t g;
    clk_decode(good_coeff(), &g);
    commit_clock(&g, good_gpu());
    kv_seti(&st, "BootTest", 0);
    save_state();
}

/* index of the FSB step equal to c's FSB, or the next lower one (steps run from high to low) */
static int step_of(const clk_t *c)
{
    for (int i = 0; i < nsteps; i++)
        if (XTAL_MHZ / step_m[i] * step_n[i] <= c->fsb + 0.05) return i;
    return nsteps - 1;
}

static void reboot_now(void) __attribute__((noreturn));
static void reboot_now(void)
{
    save_state();
    header("Restarting");
    scr("\n Restarting so Cerbios applies the saved clocks...\n");
    Sleep(1500);
    HalReturnToFirmware(HalRebootRoutine);
}

/* ------------------------------------------------------------------ dashboard */
static int dash_to_kernel_path(const char *dash, char *out, size_t max, char *dos, size_t dmax)
{
    const char *p = dash;
    int disk = 0;
    if ((p[0] == 'H' || p[0] == 'h') && (p[1] == 'D' || p[1] == 'd') && (p[2] == 'D' || p[2] == 'd') && p[4] == '-') {
        disk = p[3] - '0';
        p += 5;
    }
    if (!p[0] || p[1] != ':') return 0;
    int part;
    switch (p[0] | 32) {
    case 'e': part = 1; break;
    case 'c': part = 2; break;
    case 'x': part = 3; break;
    case 'y': part = 4; break;
    case 'z': part = 5; break;
    case 'f': part = 6; break;
    case 'g': part = 7; break;
    default: return 0;
    }
    snprintf(out, max, "\\Device\\Harddisk%d\\Partition%d%s", disk, part, p + 2);
    char letter = disk ? 'H' : (char)(p[0] & ~32);
    char dev[64];
    snprintf(dev, sizeof(dev), "\\Device\\Harddisk%d\\Partition%d\\", disk, part);
    if (disk) nxUnmountDrive('H');
    if (!nxIsDriveMounted(letter)) nxMountDrive(letter, dev);
    snprintf(dos, dmax, "%c:%s", letter, p + 2);
    return 1;
}

/* while tuning or checking clocks ClockTuner is the boot dashboard, so it can pick up after a
   restart or a freeze. When it's done it hands DashPath back to the real dashboard. */
static void restore_dashpath(void)
{
    const char *real = kv_get(&cfg, "RealDash", "");
    char dash[200] = "";
    if (!have_cini || !real[0] || str_ieq(real, self_path)) return;
    /* row timings saved: stay the boot dashboard so they're set at every power-on */
    int hook = row_saved() > 0 && kv_int(&cfg, "Enabled", 1) && self_path[0];
    const char *want = hook ? self_path : real;
    cini_get("DashPath", dash, sizeof(dash));
    if (str_ieq(dash, want)) return;
    cini_set("DashPath", want);
    if (!cini_save()) return;
    if (hook) log_line("DashPath set to ClockTuner: it sets the saved RAM timings at every start");
    else log_line("DashPath back to %s", real);
}

static void launch_dash(void) __attribute__((noreturn));
static void launch_dash(void)
{
    char kp[260], dp[260];
    const char *real = kv_get(&cfg, "RealDash", "");
    if (!strcmp(phase(), "DONE") || !kv_int(&cfg, "Enabled", 1)) restore_dashpath();
    if (real[0] && !str_ieq(real, self_path) && dash_to_kernel_path(real, kp, sizeof(kp), dp, sizeof(dp)) &&
        file_exists(dp)) {
        if (stress_ready) stress_free();
        XLaunchXBE(kp);
    }
    header("Dashboard not found");
    scr("\n RealDash in tuner.ini is '%s'\n and it can't be launched.\n", real);
    scr(" Set RealDash to your dashboard, for example:\n   RealDash = HDD0-C:\\evoxdash.xbe\n");
    int b = prompt("[A] Cerbios Recovery   [B] retry", B_A | B_B, 60, B_A);
    if (b == B_A && file_exists("E:\\Cerbios\\Recovery\\default.xbe"))
        XLaunchXBE("\\Device\\Harddisk0\\Partition1\\Cerbios\\Recovery\\default.xbe");
    HalReturnToFirmware(HalRebootRoutine);
}

/* ------------------------------------------------------------------ testing */
static const char *test_title;
static const clk_t *test_clk;
static int test_measured;

static int progress_y;

static int test_cb(const char *label, uint32_t el, uint32_t total, const stress_result_t *r)
{
    static const char *last_label;
    if (label != last_label || el < 600) {          /* full redraw at the start of each test */
        last_label = label;
        header(test_title);
        scr("Testing:\n %s\n", coeff_line(test_clk));
        int expect = (int)(test_clk->fsb * cpu_mult + 0.5);
        scr("Measured CPU: %d MHz (expected %d)%s\n", test_measured, expect,
                   (test_measured < expect * 97 / 100 || test_measured > expect * 103 / 100) ? " - NOT AS SET" : "");
        progress_y = nextRow;
        scr("\n\n\n\n");
        show_history();
        debugMoveCursor(CON_X, PROMPT_Y);
        debugPrint("%-60s\n", "[B] stop test");
    }
    debugMoveCursor(CON_X, progress_y);
    debugPrint("%-12s %3u of %3u s            \n", label, el / 1000, total / 1000);
    debugPrint("RAM: %5u MB checked  %u errors            \n", (unsigned)(r->ram_bytes >> 20), r->ram_errors);
    debugPrint("CPU: %5u loops        %u errors            \n", r->cpu_loops, r->cpu_errors);
    return (poll_pressed() & B_B) != 0;
}

enum { T_PASS, T_FAIL_CPU, T_FAIL_RAM, T_ABORT };

/* Layout padding: shifts the code after it so that no address stored in the program's data ends
   in the byte pair 0F 31, which Cerbios would patch as if it were RDTSC. Checked at build time by
   check_rdtsc_pattern.py; change PAD_BYTES if that check fails. */
#define PAD_BYTES 16
__attribute__((used, noinline)) void layout_pad(void) { __asm__ volatile(".fill " "16" ",1,0x90"); }

static void ensure_stress(void)
{
    if (stress_ready) return;
    header("Preparing tests");
    scr("\n Reserving memory for the RAM test...\n");
    stress_init(profile == PROFILE_BGA && ram_mb >= 250);   /* CPU reference computed at boot clocks */
    unsigned r1, tot = stress_region_mb(&r1);
    if (r1) snprintf(note, sizeof(note), "RAM test covers %u MB (%u MB above 128MB)", tot, r1);
    else snprintf(note, sizeof(note), "RAM test covers %u MB", tot);
    stress_ready = 1;
}

/* Test at the clocks this boot is running. Cerbios set them at start-up; nothing is switched
   while the app runs. full = quick test + full test. B stops it. */
static int run_boot_test(const clk_t *c, int full)
{
    kv_seti(&st, "Pending", 1);
    kv_setx(&st, "Candidate", c->coeff);
    save_state();                                   /* written through before the risky part */
    log_line("Testing %s at boot clocks (%s)", coeff_line(c), full ? "full" : "quick");
    ensure_stress();
    test_clk = c;
    test_title = "Testing at boot clocks";
    test_measured = measured;

    stress_result_t r;
    memset(&r, 0, sizeof(r));
    unsigned qs = (unsigned)kv_int(&cfg, "QuickSeconds", 30), fm = (unsigned)kv_int(&cfg, "FullMinutes", 5);
    if (!stress_ref_ok()) {
        r.cpu_errors = 1;                           /* CPU results already wrong while setting up */
    } else {
        stress_run("Quick test", qs * 1000u, test_cb, &r);
        if (full && !r.aborted && !r.cpu_errors && !r.ram_errors)
            stress_run("Full test", fm * 60000u, test_cb, &r);
    }
    kv_seti(&st, "Pending", 0);

    int res = r.aborted ? T_ABORT : r.ram_errors ? T_FAIL_RAM : r.cpu_errors ? T_FAIL_CPU : T_PASS;
    char rs[96];
    if (res == T_PASS) snprintf(rs, sizeof(rs), full ? "PASS" : "PASS quick");
    else if (res == T_FAIL_RAM) snprintf(rs, sizeof(rs), "FAIL RAM (%u err)", r.ram_errors);
    else if (res == T_FAIL_CPU) snprintf(rs, sizeof(rs), "FAIL CPU (%u err)", r.cpu_errors);
    else snprintf(rs, sizeof(rs), "test stopped");
    if (res == T_FAIL_RAM) log_line("  first RAM error at physical 0x%08X", r.first_bad_addr);
    history_add(NULL, c, rs);
    save_state();
    return res;
}

/* one FSB step below the tested clock, RAM no higher than what passed */
static clk_t back_off(int step, const clk_t *tested)
{
    clk_t f = *tested;
    double cap = tested->ram;                       /* never more RAM than what passed */
    if (step + 1 < nsteps && pick_ram(PROFILE_BGA /* = highest RAM <= cap */, step_n[step + 1], step_m[step + 1],
                                      cap + 0.05, &rules, &f))
        return f;
    return *tested;
}

static void manual_menu(void) __attribute__((noreturn));

/* Cerbios only applies clocks at start-up: show what will be saved and ask for the restart.
   Returns 1 for [A] (restart), 0 for [B] (back). */
static int confirm_restart(const char *title, const clk_t *c, unsigned gpu, const char *after, int auto_s, int allow_y)
{
    header(title);
    scr("\n Save:  %s\n        GPU %s MHz\n", coeff_line(c), mhz(gpu_mhz(gpu)));
    scr("\n Cerbios applies clocks when the console starts, so it\n restarts now. %s\n", after);
    scr("\n If it won't start at all: hold Eject for Cerbios Recovery\n and set Overclocking = False in cerbios.ini.\n");
    show_history();
    /* a save that isn't confirmed goes back after a minute */
    (void)allow_y;
    int b = prompt("[A] restart now   [B] back", B_A | B_B, auto_s ? auto_s : 60, B_B);
    return b;
}

static void save_or_fail(const clk_t *c, unsigned gpu)
{
    if (commit_clock(c, gpu)) return;
    header("Could not save");
    scr("\n Couldn't write %s.\n", cini_path());
    prompt("[A] manual", B_A, 30, B_A);
    manual_menu();
}

/* ------------------------------------------------------------------ RAM timings screen */
/* Memory read latency: a pointer chase through 4 MB (far past the 128 KB L2) in random order, timed
   with the run-time RDTSC. Shows what a timing change costs. */
static uint32_t *lat_buf;
static double lat_entry, lat_now;
static int lat_entry_extra;
static volatile uint32_t lat_sink;

static double measure_latency_ns(void)
{
    const uint32_t nodes = 131072, stride = 8;     /* one 32-byte cache line per node: 4 MB */
    if (!lat_buf) {
        lat_buf = (uint32_t *)malloc((size_t)nodes * 32);
        uint32_t *perm = (uint32_t *)malloc((size_t)nodes * 4);
        if (!lat_buf || !perm) { free(lat_buf); free(perm); lat_buf = NULL; return 0; }
        for (uint32_t i = 0; i < nodes; i++) perm[i] = i;
        uint32_t x = 2463534242u;
        for (uint32_t i = nodes - 1; i > 0; i--) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            uint32_t j = x % (i + 1), t = perm[i];
            perm[i] = perm[j]; perm[j] = t;
        }
        for (uint32_t i = 0; i < nodes; i++) lat_buf[perm[i] * stride] = perm[(i + 1) % nodes] * stride;
        free(perm);
    }
    double best = 0;
    for (int run = 0; run < 3; run++) {
        uint32_t q = 0;
        for (int k = 0; k < 20000; k++) q = lat_buf[q];          /* warm up the chase */
        uint64_t t0 = rdtsc_real();
        for (int k = 0; k < 400000; k++) q = lat_buf[q];
        uint64_t t1 = rdtsc_real();
        lat_sink = q;
        double ns = (double)(t1 - t0) * 1000.0 / ((double)measured * 400000.0);
        if (run == 0 || ns < best) best = ns;
    }
    return best;
}

static void exp_mark(const char *what)          /* written through before a step that could freeze */
{
    kv_set(&st, "ExpTrying", what);
    save_state();
    log_line("RAM timings: trying %s", what);
}

/* quick/full RAM + CPU test at whatever is running now (no Pending: nothing to undo after a freeze,
   the timings go back to power-on with the restart unless they were saved) */
static void timing_test(int full)
{
    ensure_stress();
    test_clk = &boot_clk;
    test_title = "RAM timing test";
    test_measured = measured;
    stress_result_t r;
    memset(&r, 0, sizeof(r));
    unsigned qs = (unsigned)kv_int(&cfg, "QuickSeconds", 30), fm = (unsigned)kv_int(&cfg, "FullMinutes", 5);
    char what[48];
    snprintf(what, sizeof(what), "%s test, row timings +%d", full ? "full" : "quick", row_running());
    exp_mark(what);
    stress_run("Quick test", qs * 1000u, test_cb, &r);
    if (full && !r.aborted && !r.cpu_errors && !r.ram_errors) stress_run("Full test", fm * 60000u, test_cb, &r);
    kv_set(&st, "ExpTrying", "");
    char rs[64];
    if (r.aborted) snprintf(rs, sizeof(rs), "test stopped");
    else if (r.ram_errors) snprintf(rs, sizeof(rs), "%s FAIL RAM (%u err) live", full ? "full" : "quick", r.ram_errors);
    else if (r.cpu_errors) snprintf(rs, sizeof(rs), "%s FAIL CPU (%u err) live", full ? "full" : "quick", r.cpu_errors);
    else snprintf(rs, sizeof(rs), "%s PASS live", full ? "full" : "quick");
    clk_t c = boot_clk;
    history_add(NULL, &c, rs);
    save_state();
}

static void row_step(int extra)                 /* live change, not saved */
{
    char what[48];
    snprintf(what, sizeof(what), "row timings +%d (live)", extra);
    exp_mark(what);
    row_set(extra);
    kv_set(&st, "ExpTrying", "");
    save_state();
    lat_now = measure_latency_ns();
    log_line("RAM timings: now +%d (tRC %u tRFC %u), memory latency %s ns", row_running(),
             (unsigned)(PFB(0x220) & 0xFF), (unsigned)((PFB(0x220) >> 8) & 0xFF), mhz(lat_now));
}

static void timings_menu(void) __attribute__((noreturn));
static void timings_menu(void)
{
    if (!lat_entry) {
        scr("\n Measuring memory latency...\n");
        lat_entry = lat_now = measure_latency_ns();
        lat_entry_extra = row_running();
        log_line("RAM timings: memory latency %s ns at +%d", mhz(lat_entry), lat_entry_extra);
    }
    for (;;) {
        int run = row_running(), sav = row_saved();
        uint32_t b = row_base();
        header("RAM timings");
        ram_timing_lines(boot_clk.ram);
        scr("Power-on: tRC %u  tRFC %u.  Running +%d, saved +%d.\n", (unsigned)(b & 0xFF),
            (unsigned)((b >> 8) & 0xFF), run, sav);
        scr("Memory latency: %s ns (when this screen opened, +%d: %s)\n", mhz(lat_now), lat_entry_extra,
            mhz(lat_entry));
        scr("\n[Up] looser +2   [Down] tighter -2   (live, not saved)\n");
        scr("[A] quick test   [X] full test   (with what's running now)\n");
        scr("[START] save +%d: set every time the console starts\n", run);
        scr("[BACK] power-on timings and nothing saved   [B] back\n");
        if (sav) scr("While extra cycles are saved, ClockTuner starts for a moment\n"
                     "at every power-on to set them, then starts your dashboard.\n");
        show_history_n(3);
        int btn = prompt("No button pressed: back in", B_A | B_B | B_X | B_START | B_BACK | B_UP | B_DOWN, 180, B_B);
        if (btn == B_UP) {
            if (run + 2 > ROW_MAX) snprintf(note, sizeof(note), "+%d is the most this allows.", ROW_MAX);
            else row_step(run + 2);
        } else if (btn == B_DOWN) {
            if (run < 2) snprintf(note, sizeof(note), "Already at the power-on timings.");
            else row_step(run - 2);
        } else if (btn == B_A || btn == B_X) {
            timing_test(btn == B_X);
        } else if (btn == B_START) {
            kv_seti(&st, "RowExtra", run);
            restore_dashpath();                     /* ClockTuner at boot only while something is saved */
            char h[48];
            snprintf(h, sizeof(h), run ? "row timings +%d saved" : "row timings: power-on saved", run);
            clk_t c = boot_clk;
            history_add(NULL, &c, h);
            save_state();
            snprintf(note, sizeof(note), run ? "Saved: +%d cycles at every start." : "Saved: power-on timings.", run);
        } else if (btn == B_BACK) {
            if (run) row_step(0);
            if (sav) {
                kv_seti(&st, "RowExtra", 0);
                restore_dashpath();
                clk_t c = boot_clk;
                history_add(NULL, &c, "row timings back to power-on");
                save_state();
            }
            snprintf(note, sizeof(note), "Power-on timings, nothing saved.");
        } else if (btn == B_B) {
            manual_menu();
        }
    }
}

/* ------------------------------------------------------------------ manual */
static void manual_menu(void)
{
    clk_t opts[16];
    clk_t saved;
    clk_decode((unsigned)kv_hex(&st, "Committed", default_coeff), &saved);
    int si = step_of(&saved), oi = 0;               /* start on the saved FSB, or the step just below it */
    unsigned gmin = (unsigned)(kv_int(&cfg, "GpuMin", 200) * 2 / 16.66666 + 0.5);
    unsigned gmax = (unsigned)(kv_int(&cfg, "GpuMax", 300) * 2 / 16.66666 + 0.5);
    unsigned gn = (unsigned)(gpu_mhz(gpu_saved()) * 2 / 16.66666 + 0.5);   /* N at P=1, M=1 */
    if (gn < gmin) gn = gmin;
    if (gn > gmax) gn = gmax;
    set_phase("MANUAL");
    save_state();
    int redraw = 1, cnt = 0;
    for (;;) {
        clk_t all[16];
        int n_all = ram_options(step_n[si], step_m[si], all, 16, rules.min_vco, rules.max_vco);
        cnt = 0;
        for (int i = 0; i < n_all; i++) if (all[i].ram >= 95.0 && all[i].ram <= 270.0) opts[cnt++] = all[i];
        if (!cnt && n_all) { opts[0] = all[n_all - 1]; cnt = 1; }
        if (!cnt) { clk_decode(default_coeff, &opts[0]); cnt = 1; }   /* can't happen: steps are filtered */
        if (redraw) {
            for (int i = 0; i < cnt; i++) if (opts[i].ram <= saved.ram + 0.1) oi = i;   /* nearest at/below saved RAM */
            redraw = 0;
        }
        if (oi >= cnt) oi = cnt - 1;
        clk_t sel = opts[oi];
        unsigned gsel = gpu_coeff_for_n(gn);
        char cache[96];
        mtrr_summary(cache, sizeof(cache), ram_mb);
        header("Manual");
        scr("\nSaved:\n %s\nSelected:\n %s\n", coeff_line(&saved), coeff_line(&sel));
        scr("GPU: selected %s MHz (0x%06X), saved %s\n", mhz(gpu_mhz(gsel)), gsel, mhz(gpu_mhz(gpu_saved())));
        ram_timing_lines(boot_clk.ram);
        scr("CPU cache: %.22s  loop check: %d MHz%s\n", cache, loop_mhz,
                   undo_places > 0 ? "  (patch undone)" : undo_places < 0 ? "  (patch NOT undone)" : "");
        scr("Up/Down FSB   Left/Right RAM   White/Black GPU -/+\n");
        scr("Save+restart, then [A] quick test [X] full test [START] none\n");
        scr("[Y] RAM timings  [BACK] all stock  [B] dashboard\n");
        show_history_n(3);
        /* if no button comes in for 3 minutes, go to the dashboard (never trap the console here) */
        int b = prompt("No button pressed: dashboard in", B_A | B_B | B_X | B_Y | B_START | B_BACK | B_UP | B_DOWN |
                       B_LEFT | B_RIGHT | B_WHITE | B_BLACK, 180, B_B);
        if (b == B_UP && si == 0 && top_fsb_capped)
            snprintf(note, sizeof(note), "Top FSB for VCO %d-%d. Raise MaxVCO in tuner.ini for more.",
                     (int)rules.min_vco, (int)rules.max_vco);
        if (b == B_UP && si > 0) { si--; redraw = 1; }
        else if (b == B_DOWN && si < nsteps - 1) { si++; redraw = 1; }
        else if (b == B_LEFT && oi > 0) oi--;
        else if (b == B_RIGHT && oi < cnt - 1) oi++;
        else if (b == B_WHITE && gn > gmin) gn--;
        else if (b == B_BLACK && gn < gmax) gn++;
        else if (b == B_A || b == B_X || b == B_START) {
            int test = b == B_A ? 1 : b == B_X ? 2 : 0;
            const char *after = test == 1 ? "The quick test runs after it."
                              : test == 2 ? "The full test runs after it." : "No test after it.";
            if (confirm_restart("Save and restart", &sel, gsel, after, 0, 0) == B_A) {
                set_phase("VERIFY");
                kv_set(&st, "VerifyFrom", "MANUAL");
                kv_seti(&st, "BootTest", test);
                save_or_fail(&sel, gsel);
                reboot_now();
            }
        } else if (b == B_BACK) {
            clk_t d;
            clk_decode(default_coeff, &d);
            if (confirm_restart("All stock", &d, GPU_STOCK_COEFF, "RAM timings too. Your dashboard starts after it.", 0, 0) == B_A) {
                kv_seti(&st, "RowExtra", 0);
                save_or_fail(&d, GPU_STOCK_COEFF);
                mark_good();                         /* stock is always good */
                set_phase("DONE");
                restore_dashpath();
                reboot_now();
            }
        } else if (b == B_Y) {
            timings_menu();
        } else if (b == B_B) {
            int running = same_clock(saved.coeff, boot_clk.coeff) && same_gpu(gpu_saved(), boot_gpu);
            set_phase(running ? "DONE" : "VERIFY");   /* VERIFY: saved clocks take effect next boot */
            if (!running) { kv_set(&st, "VerifyFrom", "MANUAL"); kv_seti(&st, "BootTest", 0); }
            save_state();
            launch_dash();
        }
    }
}

/* Read-only snapshot of the NV2A memory controller (PFB) and bus pad (PBUS) registers, for working
   out the memory timing registers. Only reads; nothing is written to the hardware. */
static void dump_registers(void)
{
    static char buf[6144];
    int n = snprintf(buf, sizeof(buf),
                     "ClockTuner %s register dump (read only)\r\nRunning: %s\r\nMeasured CPU %d MHz, RAM %u MB %s, GPU %s MHz\r\n",
                     VERSION, coeff_line(&boot_clk), measured, ram_mb, profile == PROFILE_BGA ? "BGA" : "stock",
                     mhz(gpu_mhz(boot_gpu)));
    static const struct { uint32_t from, to; const char *name; } r[] = {
        {0x100200, 0x1002FC, "PFB (memory controller) 0x100200-0x1002FC"},
        {0x001200, 0x0012FC, "PBUS 0x001200-0x0012FC"},
    };
    for (unsigned k = 0; k < sizeof(r) / sizeof(r[0]); k++) {
        n += snprintf(buf + n, sizeof(buf) - n, "\r\n%s\r\n", r[k].name);
        for (uint32_t a = r[k].from; a <= r[k].to && n < (int)sizeof(buf) - 80; a += 16) {
            volatile const uint32_t *p = (volatile const uint32_t *)(uintptr_t)(0xFD000000u + a);
            n += snprintf(buf + n, sizeof(buf) - n, "0x%06X: %08X %08X %08X %08X\r\n", (unsigned)a, p[0], p[1], p[2], p[3]);
        }
    }
    file_write("D:\\pfb_dump.txt", buf, (size_t)n);
    volatile const uint32_t *t = (volatile const uint32_t *)0xFD100220u;
    uint32_t mr = *(volatile const uint32_t *)0xFD1002C0u;
    log_line("  RAM mode register 0x%03X (CL%s, burst %d); PFB timing 0x100220-0x10022C: %08X %08X %08X %08X",
             (unsigned)(mr & 0xFFF), mrs_cl(mr) ? mrs_cl(mr) : "?", 1 << (mr & 7), t[0], t[1], t[2], t[3]);
}

/* ------------------------------------------------------------------ setup */
static void make_self_path(void)
{
    const char *k = XeImageFileName->Buffer;          /* \Device\Harddisk0\Partition1\Apps\x\default.xbe */
    int len = XeImageFileName->Length;
    char tmp[200];
    if (len >= (int)sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, k, (size_t)len);
    tmp[len] = 0;
    self_path[0] = 0;
    const char *h = strstr(tmp, "Harddisk");
    const char *pp = strstr(tmp, "Partition");
    if (!h || !pp) return;
    int disk = h[8] - '0', part = pp[9] - '0';
    const char letters[] = "?ECXYZFG";
    if (part < 1 || part > 7) return;
    const char *rest = strchr(pp, '\\');
    snprintf(self_path, sizeof(self_path), "HDD%d-%c:%s", disk, letters[part], rest ? rest : "");
}

static void default_config(void)
{
    const char *defs[][2] = {
        {"Enabled", "1"}, {"StartFSB", "166"}, {"QuickSeconds", "30"}, {"FullMinutes", "5"},
        {"MaxVCO", "1050"}, {"MinVCO", "800"}, {"GpuMin", "200"}, {"GpuMax", "300"},
        {"AutoContinueSec", "20"},
        {"CerbiosIni", "E:\\Cerbios\\cerbios.ini"}, {"RealDash", ""},
    };
    for (unsigned i = 0; i < sizeof(defs) / sizeof(defs[0]); i++)
        if (!kv_get(&cfg, defs[i][0], NULL)) kv_set(&cfg, defs[i][0], defs[i][1]);
}

static void save_config(void)
{
    kv_save(&cfg, CFG_PATH,
            "; ClockTuner settings\r\n"
            "; Enabled=0 skips tuning and goes straight to RealDash.\r\n"
            "; StartFSB: highest FSB on the manual screen (MHz). QuickSeconds/FullMinutes: test lengths.\r\n"
            "; MaxVCO: highest PLL VCO MHz ever used (stock is 800).\r\n"
            "; MinVCO: lowest PLL VCO MHz ever used (stock is 800; lower is untested).\r\n"

            "; GpuMin/GpuMax: GPU clock range offered on the manual screen (stock 233).\r\n"
            "; RealDash: your real dashboard (filled in automatically the first time).\r\n");
}

int main(void)
{
    /* First, before video, USB or SDL: with an overclock saved, Cerbios patched RDTSC byte pairs
       in this program as it loaded, including nxdk's QueryPerformanceCounter that SDL's input
       timing uses (the controller died on overclocked boots). Put our code back as built. */
    undo_places = undo_load_patches(&undo_bytes);

    kv_load(&cfg, CFG_PATH);
    default_config();
    kv_load(&st, STATE_PATH);
    nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    nxMountDrive('C', "\\Device\\Harddisk0\\Partition2\\");

    /* RAM row timings, before anything else runs: set the saved ones. Whether they're already set
       is also how a power-on start is told apart from being opened from the dashboard: the register
       only holds its power-on value until something changes it (1.8 went by the kernel's launch
       data instead, which is empty however ClockTuner is started, so it went straight back to the
       dashboard when opened from it). */
    int row_froze = 0, row_found, hook_recent = 0;
    {
        uint32_t cur = PFB(0x220) & 0xFFFF;
        uint32_t b = (uint32_t)kv_hex(&st, "RowBase", cur);
        if (!kv_get(&st, "RowBase", NULL) || (cur & 0xFF) < (b & 0xFF) || (cur >> 8) < ((b >> 8) & 0xFF))
            kv_setx(&st, "RowBase", cur);             /* first start, or lower than the "power-on" value */
        row_found = row_running();
        int now = rtc_minutes(), last = (int)kv_int(&st, "HookAt", -1);
        hook_recent = now >= 0 && last >= 0 && now - last >= 0 && now - last < 5;
        if (kv_int(&st, "RowApplying", 0)) {          /* setting them froze last time: drop them */
            kv_seti(&st, "RowApplying", 0);
            kv_seti(&st, "RowExtra", 0);
            row_froze = 1;
        } else if (row_saved() && row_found != row_saved()) {
            kv_seti(&st, "RowApplying", 1);
            save_state();                             /* written through first */
            row_set(row_saved());
            kv_seti(&st, "RowApplying", 0);
        }
        save_state();
    }
    /* Power-on start just to set them (the register still had its power-on value): straight on to
       the real dashboard. Not if it did that less than 5 minutes ago: then the timings didn't
       survive the dashboard starting ClockTuner, and it must open instead of bouncing back. */
    if (row_saved() && row_found == 0 && !hook_recent && !row_froze && kv_int(&cfg, "Enabled", 1) &&
        !kv_int(&st, "Pending", 0) && !strcmp(phase(), "DONE") && !kv_get(&st, "ExpTrying", "")[0]) {
        char kp[260], dp[260];
        const char *real = kv_get(&cfg, "RealDash", "");
        if (real[0] && dash_to_kernel_path(real, kp, sizeof(kp), dp, sizeof(dp)) && file_exists(dp)) {
            kv_seti(&st, "HookAt", rtc_minutes());
            save_state();
            log_line("Power-on: RAM row timings +%d set, starting %s", row_running(), real);
            XLaunchXBE(kp);
        }
    }

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugClearScreen();
    SDL_Init(SDL_INIT_GAMECONTROLLER);

    ram_mb = ram_total_mb();
    profile = ((pfb_cfg1() & 0x1010u) == 0x1010u) ? PROFILE_BGA : PROFILE_STOCK;
    default_coeff = profile == PROFILE_BGA ? BGA_COEFF : STOCK_COEFF;
    clk_decode(pll_read(), &boot_clk);
    if (!boot_clk.fsb) clk_decode(default_coeff, &boot_clk);
    scr("CLOCK TUNER %s\n\n Measuring CPU speed (about 5 seconds)...\n", VERSION);
    meas_tick = measure_cpu_mhz();
    meas_rtc = rtc_measure_cpu_mhz();
    measured = meas_rtc ? meas_rtc : meas_tick;
    uint32_t loop_rate = rtc_loop_rate();
    /* The CPU multiplier is fixed. Learn it only at stock clocks: at other settings a mismatch
       means the setting isn't running as the formula says, not a different multiplier. */
    double m_now = (double)(int)(measured / boot_clk.fsb * 2.0 + 0.5) / 2.0;
    if (same_clock(boot_clk.coeff, default_coeff) && m_now >= 3.0 && m_now <= 12.0) {
        cpu_mult = m_now;
        kv_seti(&st, "CpuMult10", (long)(m_now * 10.0 + 0.5));
        if (loop_rate) kv_seti(&st, "LoopRate", (long)loop_rate);    /* calibration for loop_mhz */
    } else {
        cpu_mult = kv_int(&st, "CpuMult10", 55) / 10.0;
    }
    {
        long cal = kv_int(&st, "LoopRate", 0);
        clk_t d;
        clk_decode(default_coeff, &d);
        if (cal > 0 && loop_rate) loop_mhz = (int)((double)loop_rate / (double)cal * d.fsb * cpu_mult + 0.5);
    }
    {
        /* two independent checks; the clocks count as "not as set" only if both disagree */
        double expect = boot_clk.fsb * cpu_mult;
        int tsc_ok = measured > expect * 0.98 && measured < expect * 1.02;
        int loop_ok = loop_mhz > expect * 0.97 && loop_mhz < expect * 1.03;
        speed_ok = same_clock(boot_clk.coeff, default_coeff) || tsc_ok || loop_ok;
    }
    rules.win_lo = 200.0;
    rules.win_hi = (double)kv_int(&cfg, "RamMax", 225);
    rules.max_over = rules.win_hi;
    rules.max_vco = (double)kv_int(&cfg, "MaxVCO", 1050);
    rules.min_vco = (double)kv_int(&cfg, "MinVCO", 800);
    boot_gpu = *NVPLL & 0x7FFFF;
    if (gpu_mhz(boot_gpu) < 50.0 || gpu_mhz(boot_gpu) > 600.0) boot_gpu = GPU_STOCK_COEFF;
    nsteps = fsb_steps_build((double)kv_int(&cfg, "StartFSB", 166), step_n, step_m, MAXSTEP);
    {
        /* keep only FSB steps that have at least one PLL setting inside MinVCO..MaxVCO; with the
           stock FSB multiplier the VCO is 6 x FSB, so above 175 MHz nothing fits MaxVCO 1050 */
        /* walk up from stock and stop at the first step with no setting, so the list never jumps
           (FSB 200 fits again at VCO 800 with a lower multiplier, but that's a 25 MHz leap) */
        int lo = nsteps - 1;
        while (lo > 0) {
            clk_t tmp[16];
            if (ram_options(step_n[lo - 1], step_m[lo - 1], tmp, 16, rules.min_vco, rules.max_vco) <= 0) break;
            lo--;
        }
        int k = 0;
        for (int i = lo; i < nsteps; i++, k++) { step_n[k] = step_n[i]; step_m[k] = step_m[i]; }
        nsteps = k;
        top_fsb_capped = (double)kv_int(&cfg, "StartFSB", 166) > XTAL_MHZ / step_m[0] * step_n[0] + 1.0;
    }

    /* cerbios.ini + install as boot dashboard */
    have_cini = cini_load(kv_get(&cfg, "CerbiosIni", "E:\\Cerbios\\cerbios.ini"));
    if (!have_cini && cini_load("C:\\Cerbios\\cerbios.ini")) {
        have_cini = 1;
        kv_set(&cfg, "CerbiosIni", "C:\\Cerbios\\cerbios.ini");
    }
    make_self_path();
    if (have_cini && self_path[0]) {
        /* Opening ClockTuner doesn't make it the boot dashboard any more (if it ever hung, every
           boot would come back into it). It only takes over DashPath while saved clocks wait to be
           checked after a restart (see commit_clock). Here it just remembers the real dashboard. */
        char dash[200] = "";
        cini_get("DashPath", dash, sizeof(dash));
        if (!str_ieq(dash, self_path) && dash[0] && !kv_get(&cfg, "RealDash", "")[0]) kv_set(&cfg, "RealDash", dash);
    }
    save_config();
    /* DashPath still points here but no saved clocks are waiting to be checked (older versions took
       it over on every start, and a hang then sent every boot back here): give it back now */
    if (have_cini && self_path[0] && !kv_int(&st, "Pending", 0) && strcmp(phase(), "VERIFY")) {
        char dash[200] = "";
        cini_get("DashPath", dash, sizeof(dash));
        if (str_ieq(dash, self_path)) restore_dashpath();
    }
    {
        char cache[160];
        mtrr_summary(cache, sizeof(cache), ram_mb);
        log_line("Boot: %uMB %s, %s, CPU cache: %s", ram_mb, profile ? "BGA" : "stock", coeff_line(&boot_clk), cache);
        log_line("  PLL register 0x%08X, CPU %d MHz (clock chip), %d (kernel tick), %d (loop), x%s%s",
                 pll_read_raw(), meas_rtc, meas_tick, loop_mhz, mhz(cpu_mult), speed_ok ? "" : "  - NOT AS SET");
        if (undo_places < 0) log_line("  couldn't read this XBE to undo Cerbios load-time patches");
        else log_line("  Cerbios load-time patches undone: %d places, %d bytes", undo_places, undo_bytes);
        log_line("  RAM row timings: +%d found at start, +%d saved, now +%d%s", row_found, row_saved(),
                 row_running(), hook_recent && row_saved() && !row_found
                 ? " - found reset less than 5 min after the power-on start: they didn't last" : "");
        dump_registers();
    }

    if (!kv_int(&cfg, "Enabled", 1)) launch_dash();

    if (!phase()[0] || kv_int(&st, "Profile", -1) != profile || kv_int(&st, "RamMB", -1) != (long)ram_mb) {
        if (phase()[0]) {
            log_line("RAM setup changed (%uMB, %s): starting over", ram_mb, profile ? "BGA" : "stock");
            clk_t d;
            clk_decode(default_coeff, &d);
            commit_clock(&d, GPU_STOCK_COEFF);       /* old clocks were for different RAM */
        }
        reset_state();
    }

    /* cerbios.ini is the truth: if it was edited by hand, take its clocks as the saved ones */
    if (have_cini) {
        char v[32] = "";
        cini_get("Overclocking", v, sizeof(v));
        int oc = str_ieq(v, "True");
        v[0] = 0;
        cini_get("CPUMPLLCoeff", v, sizeof(v));
        unsigned ic = (unsigned)strtoul(v, NULL, 16) & 0xFFFFFF;
        v[0] = 0;
        cini_get("NVPLLCoeff", v, sizeof(v));
        unsigned ig = (unsigned)strtoul(v, NULL, 16) & 0x7FFFF;
        unsigned want_c = (oc && ic) ? ic : default_coeff, want_g = (oc && ig) ? ig : GPU_STOCK_COEFF;
        unsigned have_c = (unsigned)kv_hex(&st, "Committed", default_coeff);
        if (!same_clock(want_c, have_c) || !same_gpu(want_g, gpu_saved())) {
            log_line("cerbios.ini was changed outside ClockTuner: CPUMPLLCoeff 0x%06X, NVPLLCoeff 0x%06X", want_c, want_g);
            kv_setx(&st, "Committed", want_c);
            kv_setx(&st, "GpuCommitted", want_g);
            if (!kv_int(&st, "Pending", 0)) set_phase("DONE");
            save_state();
        }
    }

    if (!have_cini) {
        header("cerbios.ini not found");
        scr("\n Looked for %s and C:\\Cerbios\\cerbios.ini.\n", kv_get(&cfg, "CerbiosIni", ""));
        scr(" Set CerbiosIni in tuner.ini to where your cerbios.ini is.\n");
        prompt("[A] dashboard", B_A, 30, B_A);
        launch_dash();
    }

    /* legacy phases from v1.3 and older (live testing) */
    if (!strcmp(phase(), "SEARCH") || !strcmp(phase(), "RAMUP") || !strcmp(phase(), "AUTO")) set_phase("MANUAL");

    /* an experiment step that never finished: it crashed (nothing was saved, so nothing to undo) */
    {
        const char *tr = kv_get(&st, "ExpTrying", "");
        if (tr[0]) {
            char h[64];
            snprintf(h, sizeof(h), "FROZE: %.48s", tr);
            history_add(NULL, &boot_clk, h);
            log_line("Froze during: %s", tr);
            kv_set(&st, "ExpTrying", "");
            kv_set(&st, "ProbeSkip", "");
            save_state();
        }
    }

    if (row_froze) {
        clk_t c = boot_clk;
        history_add(NULL, &c, "FROZE setting row timings: cleared");
        log_line("Setting the saved RAM row timings froze last time: they're cleared");
        restore_dashpath();
        save_state();
        snprintf(note, sizeof(note), "Setting the saved RAM timings froze, so they're cleared.");
    }

    /* the test running last time never finished: the console froze or was switched off */
    if (kv_int(&st, "Pending", 0)) {
        clk_t c;
        clk_decode((unsigned)kv_hex(&st, "Candidate", 0), &c);
        history_add(NULL, &c, "FAIL - FROZE");
        kv_seti(&st, "Pending", 0);
        save_state();
        commit_good();
        int running = same_clock(boot_clk.coeff, good_coeff()) && same_gpu(boot_gpu, good_gpu());
        header("Last test froze");
        scr("\n The console stopped during the test at\n   %s\n", coeff_line(&c));
        scr(" The last good clocks are saved again.\n");
        show_history();
        if (running) {
            prompt("[A] manual", B_A, 30, B_A);
            manual_menu();
        }
        set_phase("VERIFY");
        kv_set(&st, "VerifyFrom", "MANUAL");
        kv_seti(&st, "BootTest", 0);
        save_state();
        int b = prompt("[A] restart with the last good clocks   [Y] manual", B_A | B_Y, 30, B_A);
        if (b == B_Y) manual_menu();
        reboot_now();
    }

    unsigned committed = (unsigned)kv_hex(&st, "Committed", default_coeff);

    /* clocks saved on the manual screen, after the restart */
    if (!strcmp(phase(), "VERIFY")) {
        clk_t c;
        clk_decode(committed, &c);
        int cpu_on = same_clock(boot_clk.coeff, committed), gpu_on = same_gpu(boot_gpu, gpu_saved());
        if (!cpu_on || !gpu_on || !speed_ok) {
            char rs[64];
            if (!cpu_on) snprintf(rs, sizeof(rs), "FAIL - DIDN'T BOOT");
            else if (!speed_ok) snprintf(rs, sizeof(rs), "NOT APPLIED (CPU %d MHz)", measured);
            else snprintf(rs, sizeof(rs), "GPU clock not applied");
            history_add(NULL, &c, rs);
            commit_good();
            snprintf(note, sizeof(note), "Saved clocks didn't come up. Last good ones saved again.");
            manual_menu();
        }
        int test = (int)kv_int(&st, "BootTest", 0);
        const char *result = "BOOT OK";
        if (test) {
            int r = run_boot_test(&c, test == 2);
            kv_seti(&st, "BootTest", 0);
            if (r == T_FAIL_CPU || r == T_FAIL_RAM) {
                commit_good();                       /* never leave failing clocks saved */
                set_phase("VERIFY");
                kv_set(&st, "VerifyFrom", "MANUAL");
                save_state();
                header(r == T_FAIL_RAM ? "RAM test failed" : "CPU test failed");
                scr("\n These clocks failed:\n   %s\n", coeff_line(&c));
                scr(" The last good clocks are saved again.\n");
                show_history();
                int b = prompt("[A] restart with the last good clocks   [Y] manual", B_A | B_Y, 30, B_A);
                if (b == B_Y) manual_menu();
                reboot_now();
            }
            if (r == T_PASS) mark_good();
            result = NULL;                           /* run_boot_test already logged it */
        }
        if (result) history_add(NULL, &c, result);
        set_phase("DONE");
        save_state();
        header("Done");
        char cache[96];
        mtrr_summary(cache, sizeof(cache), ram_mb);
        scr("\n Saved and running:\n   %s\n", coeff_line(&c));
        scr(" GPU %s MHz   CPU measured %d MHz\n CPU cache: %.52s\n", mhz(gpu_mhz(boot_gpu)), measured, cache);
        if (row_saved())
            scr("\n From now on ClockTuner starts for a moment at power-on to set\n"
                " the RAM timings (+%d), then your dashboard starts.\n", row_saved());
        else
            scr("\n Your dashboard starts directly from now on. To change clocks,\n"
                " open ClockTuner from your dashboard.\n");
        show_history();
        int b = prompt("[A] dashboard   [Y] manual", B_A | B_Y, (int)kv_int(&cfg, "AutoContinueSec", 20), B_A);
        if (b == B_Y) manual_menu();
        launch_dash();
    }

    /* opened from the dashboard */
    if (!strcmp(phase(), "DONE")) {
        if (!kv_get(&st, "GoodCoeff", NULL) && same_clock(boot_clk.coeff, committed) && speed_ok) {
            mark_good();                             /* older version's clocks, running as set */
            save_state();
        }
        int cpu_on = same_clock(boot_clk.coeff, committed), gpu_on = same_gpu(boot_gpu, gpu_saved());
        if (!cpu_on || !gpu_on || !speed_ok) {
            clk_t c;
            clk_decode(committed, &c);
            header("Saved clocks not running");
            scr("\nSaved:\n %s\n GPU %s MHz\n", coeff_line(&c), mhz(gpu_mhz(gpu_saved())));
            if (!speed_ok) scr("The CPU measures %d MHz instead of %d.\n", measured, (int)(c.fsb * cpu_mult + 0.5));
            else scr("This boot is running different clocks.\n");
            show_history();
            int b = prompt("[A] one step lower + restart  [Y] manual  [B] dash", B_A | B_Y | B_B, 60, B_A);
            if (b == B_B) launch_dash();
            if (b == B_Y) manual_menu();
            clk_t f = back_off(step_of(&c), &c);
            save_or_fail(&f, gpu_saved());
            set_phase("VERIFY");
            kv_set(&st, "VerifyFrom", "MANUAL");
            kv_seti(&st, "BootTest", 1);
            reboot_now();
        }
    }

    manual_menu();
    return 0;
}
