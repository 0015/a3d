# The shared half of the a3d demo


> **This is the demo copy.** The benchmark has been removed from
> `a3d_demo/a3d_demo_app.cpp`, and `bench/` and `bench_table.py` are not here.
> Everything below that describes measuring, logs or tables belongs to the
> original at `examples/common/`, which is unchanged.

`a3d_demo/` is an ESP-IDF component that three example projects register:

| Project | Board | What it adds |
|---|---|---|
| `ESP32-S3-LCD2` | Waveshare ESP32-S3-Touch-LCD-2 | ST7789 240x320 over SPI, CST816S touch |
| `Waveshare_ESP32-C6-Touch-AMOLED-2.16` | Waveshare ESP32-C6-Touch-AMOLED-2.16 | SH8601 480x480 over QSPI, CST9217 touch, AXP2101 |
| `ESP32-Bench-Headless` | any | nothing; the benchmark with no panel |

A board supplies a `Display`: its size, how to push a tile, how to read a
finger. Everything above that — loading the containers, skinning, binning,
rasterizing, timing, the self-checks, the HUD — lives in the component.

**This is why the comparison table means anything.** A benchmark copy-pasted
between two projects diverges within a session, and the two columns then report
two different programs without saying so.

## The benchmark

Runs first, before anything is shown, and ends with a pass/fail summary.

- **Off-screen rows fix the viewport at 240x320 on every board** and never send
  a pixel to a panel. The panels differ in resolution, interface and bandwidth;
  folding the blit into the render figure would compare an ST7789 against a
  MIPI DSI rather than one CPU against another.
- **Frame counts are chosen from a probe frame** to hit a time budget, because
  a part without an FPU is more than ten times slower and a fixed count is four
  seconds on one board and four minutes on another. The count actually used is
  printed as `frames=`.
- **Self-checks.** A benchmark that drew nothing passes every timing check
  there is, and an animated one whose pose never moved passes them twice. Both
  are checked, per configuration.
- **`present=1` rows use the panel's own resolution** and include the blit.
  They are not comparable across boards, and the `view=` field says so.

## Turning logs into the README table

```bash
idf.py -p <port> monitor | tee bench/esp32s3.log
python3 bench_table.py bench/*.log
python3 bench_table.py --into ../../dist/README.md bench/*.log
```

Every number the published README carries comes out of this script. The logs it
was given are in `bench/`.
