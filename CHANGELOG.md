# Changelog

## 1.8.1
- Fixed: with RAM timings saved, ClockTuner went straight back to the dashboard when opened from it. It also loosened the timings again each time. It now tells a power-on start apart by whether the timings are already set.

## 1.8
- RAM timings screen: loosen tRC/tRFC live, test them, and save them so they're set at every power-on.
- Removed the CAS latency experiments (see the README for what they found).

## 1.7 (experimental builds)
- Crash-safe memory-controller probing and CAS latency mapping. These found the NV2A read-timing field, `0x100228` bits 24-27, and showed that CL4 doesn't work on the stock Samsung chips.
- Screens can no longer overflow and blank out.

## 1.6
- Manual mode only (automatic tuning removed).
- Register dump (`pfb_dump.txt`) and RAM timing display.

## 1.5
- The real CPU speed is measured against the CMOS clock. The RDTSC patches Cerbios makes at load time are undone, which fixes the controller not responding on overclocked starts.
- FSB steps are limited to PLL settings inside `MinVCO`-`MaxVCO`.

## 1.4
- Clocks are only applied at power-on (live switching garbled the RAM). Every prompt times out.
