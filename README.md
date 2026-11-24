# Xbox ClockTuner

A homebrew app for original Xbox consoles running the **Cerbios** BIOS. It lets you:
- **Set CPU/FSB, RAM and GPU clocks** from the controller. They're saved to `cerbios.ini`.
- **Check them after a restart.** It measures the real CPU speed against the clock chip, then runs a RAM and CPU stress test.
- **Go back automatically when a test fails.** If a test fails, or the console freezes, it restores the last clocks that passed.
- **Loosen the RAM row/refresh timings** (tRC/tRFC) and have them set at every power-on.

Built with [nxdk](https://github.com/XboxDev/nxdk). Tested on a v1.6 console with stock 64 MB RAM.

> **Use at your own risk.** Overclocking can make a console unstable, and clocks that are too high may not boot at all. Cerbios applies the saved clocks before anything else runs. So if a setting won't boot, the way back is **Cerbios Recovery** (hold Eject while powering on), where you set `Overclocking = False`.

## Install
1. Download `Xbox_ClockTuner_v1.8.1.zip` from [Releases](../../releases).
2. Copy the `ClockTuner` folder to your console, e.g. `E:\Apps\ClockTuner\`.
3. Start it from your dashboard. The first start learns where your dashboard is (`RealDash` in `tuner.ini`). It looks for `cerbios.ini` at `E:\Cerbios\cerbios.ini` (or `C:\Cerbios\`). If yours is somewhere else, set `CerbiosIni` in `tuner.ini`.

**Updating:** copy only `default.xbe` over the old one, and keep your `tuner.ini` and `state.ini`.

## Manual screen
- **Up/Down:** FSB. **Left/Right:** RAM clock. **White/Black:** GPU clock.
- **[A] / [X] / [START]:** save to `cerbios.ini` and restart (it asks first). After the restart it runs the quick test (30 s), the full test (5 min), or no test.
- **[Y]:** RAM timings screen.
- **[BACK]:** all stock (CPU, RAM, GPU and RAM timings), then restart to your dashboard.
- **[B]:** dashboard. If you press nothing for 3 minutes, it also goes to your dashboard.

Clocks are only ever applied by Cerbios at power-on. ClockTuner never switches them while running, because that garbles the RAM.

After the restart, ClockTuner briefly becomes Cerbios's boot dashboard (`DashPath`), checks that the clocks came up as saved, and runs the test you picked:
- **Passes, or you picked no test:** the clocks stay saved, and your dashboard is the boot dashboard again.
- **Fails or freezes:** the last clocks that passed (stock until something has) are saved again, and it restarts with them. After a freeze, turn the console off and on.
- **Doesn't start at all:** hold Eject for Cerbios Recovery and set `Overclocking = False`.

## RAM timings ([Y] on the manual screen)
These are the low two bytes of the NV2A register `0x100220`: 12 and 14 cycles at power-on on a stock v1.6. Going by the next NVIDIA generation's register layout, they're tRC (row cycle) and tRFC (refresh). Looser values give the RAM more time, which can help at higher RAM clocks. They cost a little memory latency, about 0.7 ns per +2.
- **Up / Down:** looser or tighter by 2 cycles, live (not saved), from the power-on values up to +16.
- **[A] / [X]:** quick or full test with what's running now.
- **[START]:** save what's running.
- **[BACK]:** power-on timings, and nothing saved.

The BIOS never sets this register, so while extra cycles are saved, ClockTuner stays Cerbios's boot dashboard. At power-on it sets them and starts your real dashboard straight away, with nothing on screen. Games started from the dashboard keep them until the console is switched off. Save +0 (or press [BACK]) and your dashboard is the boot dashboard again.

**Testing a higher RAM clock with them:** set +2 or +4 and press [START]. Then, on the manual screen, pick the RAM clock and press [A]. After the restart the timings are set before the test runs. The results list shows them, e.g. `RAM 230 +4  PASS quick`.

If setting the timings ever freezes at power-on, the next start notices, clears them and says so. If the console won't reach your dashboard at all, use Cerbios Recovery to set `DashPath` back to your dashboard.

## Settings (`tuner.ini`)
- **`StartFSB`:** the highest FSB on the manual screen (default 166).
- **`MaxVCO` / `MinVCO`:** the PLL range allowed (default 800-1050). FSB steps with no setting in that range aren't offered. With the stock multiplier, 1050 tops out at FSB 175.
- **`QuickSeconds` / `FullMinutes`:** test lengths.
- **`GpuMin` / `GpuMax`:** the GPU range on the manual screen. Nothing tests the GPU, so check GPU clocks in games.
- **`AutoContinueSec`:** how long the "Done" screen waits before going to your dashboard (default 20).
- **`CerbiosIni`:** where `cerbios.ini` is.
- **`RealDash`:** your dashboard (filled in automatically).
- **`Enabled = 0`:** sends it straight to your dashboard and gives `DashPath` back.

## Files it writes (next to `default.xbe`)
- `state.ini`: saved clocks and timings, the last good clocks, and the results history. Delete it to start over.
- `log.txt`: every start, measurement, save and result.
- `pfb_dump.txt`: the NV2A memory-controller registers as of the latest start (read only).

## How it works
- **Clocks:** CPU/RAM come from the PLL at PCI 0:0.3 register 0x6C (FSB = 16.667 × N / M MHz; CPU = FSB × 5.5). ClockTuner writes `CPUMPLLCoeff`, `NVPLLCoeff` and `Overclocking` in `cerbios.ini`, and Cerbios applies them at power-on.
- **Measuring the real CPU speed:** on an overclocked console, Cerbios (`AdvCPUSupport`) patches RDTSC instructions in every program as it loads. So ClockTuner times the CPU with an RDTSC built at run time, against the CMOS clock. It also puts back the bytes Cerbios patched in its own code. Before it did this, the controller stopped responding on overclocked starts.
- **Tests:** a CRC-checked RAM pattern test across free memory, plus a CPU arithmetic check against a known result.

## Notes for tinkerers: CAS latency
CAS latency can't be changed from Cerbios, but it can be changed live: resending the RAM mode register while running works. However:
- The stock Samsung chips (K4D263238F) are rated for CL3 only. They do respond to the CL4 command, but reads then come back wrong.
- The NV2A has one read-timing field: `0x100228` bits 24-27 (8 at power-on). One step either way breaks CL3 reads.
- With CL4, no value of that field from 5 to 10 gave correct reads. Values further out can hang the memory controller.

So on stock chips, a higher RAM clock means faster-rated CL3 chips, not CL4.

## Building
Needs [nxdk](https://github.com/XboxDev/nxdk).
```
make NXDK_DIR=/path/to/nxdk
python3 tools/check_rdtsc_pattern.py bin/default.xbe
```
The check fails if any `0F 31` byte pair ended up in a data section, where Cerbios's load-time RDTSC patching would corrupt it.

## Credits
- [nxdk](https://github.com/XboxDev/nxdk) and the XboxDev community.
- Cerbios, which does the actual clock setting at power-on.
- [envytools](https://github.com/envytools/envytools), for the NVIDIA register documentation.

## License
MIT. See [LICENSE](LICENSE).
