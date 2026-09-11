# ESP-IDF demos

Four ESP-IDF projects that boot straight into the viewer. They are **copies** of
the four at `examples/Waveshare_*`, with the start-up benchmark removed.

| Project | Board | Panel |
|---|---|---|
| [Waveshare_ESP32-S3-Touch-LCD-2](Waveshare_ESP32-S3-Touch-LCD-2) | ESP32-S3 | 240x320 ST7789 SPI |
| [Waveshare_ESP32-S3-Touch-AMOLED-1.75C](Waveshare_ESP32-S3-Touch-AMOLED-1.75C) | ESP32-S3 | 466x466 CO5300 QSPI, round |
| [Waveshare_ESP32-C6-Touch-AMOLED-2.16](Waveshare_ESP32-C6-Touch-AMOLED-2.16) | ESP32-C6 | 480x480 SH8601 QSPI, round |
| [Waveshare_ESP32-P4-Nano-Bench](Waveshare_ESP32-P4-Nano-Bench) | ESP32-P4 | 800x1280 JD9365 DSI |

```bash
cd Waveshare_ESP32-S3-Touch-AMOLED-1.75C
idf.py set-target esp32s3 && idf.py -p <port> flash monitor
```

## ESP-IDF version

**5.5.4 is the toolchain these were built, flashed and measured on**, and it is
what every number in the top-level README describes. Use it if you can.

They also build and run on **6.x** - that is checked before each release, on
both majors - but a3d measured about **9% slower off screen on the ESP32-P4
under 6.0.2** than under 5.5.4, confirmed twice on two different applications.
The S3 and C6 were never checked, so nothing is claimed for them either way.
Each project prints a warning at configure time when it sees 6.x.

Getting these to build on 6.0 at all took four unrelated fixes, all of them in
the board glue rather than in a3d, and all of them C++-only:

| what 6.0 changed | symptom |
|---|---|
| `cs_gpio_num` / `dc_gpio_num` / `reset_gpio_num` became `gpio_num_t` | `invalid conversion from 'int' to 'gpio_num_t'` |
| `-Werror=missing-field-initializers` is on | a partial designated-initialiser list is now an error - including inside vendor config macros |
| the umbrella `driver` component stopped pulling in the split ones | `driver/ledc.h: No such file or directory` |
| `ledc_channel_config_t` gained a member | the same missing-initializer error |

The fixes are casts and field-by-field struct filling, which are correct on 5.x
too - so there is one source for both, not a version guard.

## Why these exist next to the originals

The originals run a full sweep before they draw anything: every model, at one
and two workers, animated and static, in flash and in PSRAM, each with a warm-up
budget. That takes minutes on an S3 and considerably longer on a C6, and it is
the right behaviour for the thing the published tables are made from. It is the
wrong first impression for somebody who flashed a board to see a3d draw.

So these four skip it. **Nothing else differs** - same panel bring-up, same
viewer, same models, same rasterizer.

**The originals are unchanged and remain the measuring versions.** If a number
is going into a table it comes from `examples/Waveshare_*`, never from here.

## What was removed

In `common/a3d_demo/a3d_demo_app.cpp`: `runBenchmark()` and the call to it,
`benchOne()`, `median()`, `budgetFrames()`, `progress()`, and the `check()`
self-test counters that only the sweep used - 194 lines. `report()` STAYS, so
the board still prints its target, cores, clock, FPU, PSRAM, panel and free
memory at boot; that is how you tell which build is running, and it costs one
line of output rather than three minutes.

`common/bench/` and `common/bench_table.py` are not copied either: device logs
and the script that turns them into tables belong with the version that
produces them.

Measured on ESP-IDF 5.5.4, both projects built from a deleted `sdkconfig` so
each regenerated from its own `sdkconfig.defaults`: the S3 AMOLED binary goes
from **744,352 to 738,912 bytes**. 5,440 bytes, which is the benchmark being
gone rather than merely unreachable.

The first version of that sentence said 766,928 -> 738,256, and it was wrong in
the way this project's own rule #5 warns about: the original was built with a
`sdkconfig` that had been sitting in that directory and had drifted from
`sdkconfig.defaults`, while the fresh copy had none. Two builds differing by
more than the change. `sdkconfig` is gitignored here precisely because it is
per-checkout state, so a clean comparison has to delete it on both sides.

## A path that is easy to get wrong here

These sit one directory deeper than the originals, so every `A3D_ROOT` is
`../../../..` rather than `../../..`. Get it wrong and the failure is a
configure-time `FATAL_ERROR` about a missing `fox.a3d`, which points at the
assets rather than at the path that went looking for them.

`Waveshare_ESP32-P4-Nano-Bench` also names its board component **outside** this
folder, at `../../Waveshare_ESP32-P4-Nano/components`, because that project was
not copied here and a second copy of a panel bring-up is a second thing to keep
in step.

## Verified

All four configure and build to a flashable binary on ESP-IDF **5.5.4**.
**None has been flashed in this form** - the originals they were copied from
were run on the boards they name.
