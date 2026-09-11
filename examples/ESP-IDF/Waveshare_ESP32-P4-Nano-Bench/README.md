# a3d benchmark — Waveshare ESP32-P4-Nano, 10.1" DSI


> **Demo build — no benchmark.** This is the copy under `examples/ESP-IDF/`:
> it boots straight into the viewer. The measuring version, which runs the full
> sweep first and prints the rows the published tables are made from, is the
> original at `examples/Waveshare_ESP32-P4-Nano-Bench`. Nothing else differs.

The shared a3d benchmark on the P4's 800x1280 MIPI-DSI panel, then the viewer.
Flash it, capture the log, and hand the log to `../common/bench_table.py`.

## Why this project exists

The published table has an ESP32-P4 column whose Panel cell reads
**`none (headless run)`**. That is accurate rather than missing: the P4 sweep
was taken with `examples/ESP32-Bench-Headless`, which sends nothing to a
display, so the P4 is the one measured part with no row in *On the panel*.

This fills that gap and nothing else. It is the same `examples/common/a3d_demo`
component the S3 and C6 projects register, so everything above the panel is the
same object code — the only reason a comparison between the columns means
anything.

Neither the board component nor the shared demo is copied in. `CMakeLists.txt`
points `EXTRA_COMPONENT_DIRS` at `../common` and at the sibling STL example's
`components/`, so there is one `p4_nano_board` in this tree and one benchmark.

## Build and run

```sh
idf.py set-target esp32p4
idf.py -p /dev/cu.wchusbserial<...> flash monitor
```

**Either ESP-IDF 5.x or 6.x builds this**, and `main/idf_component.yml`
deliberately does not pin the panel driver to one of them. Waveshare ship
`esp_lcd_jd9365` 1.0.6 for IDF 5.x and 2.0.0 for 6.x; 1.0.6 reads
`panel_dev_config->color_space`, a field 5.5.4 still carries as a deprecated
union member and 6.0 deleted, so pinning it makes this project fail to compile
on 6.0 with

```
esp_lcd_jd9365.c:85: 'esp_lcd_panel_dev_config_t' has no member named 'color_space'
```

before a line of a3d is reached. Verified both ways: IDF 5.5.4 and IDF 6.0.2
each build it to a flashable binary, and each resolves jd9365 2.0.0.

**But do not mix IDF versions inside one table.** Every log in
`../common/bench/` was captured on v5.5.4. The benchmark prints its toolchain
(`[A3D] build ... idf=`), `bench_table.py` puts it in a column, and a table
built from logs that disagree carries a warning saying the columns compare
compilers as well as parts. If you capture this run on 6.0.2, that warning is
telling you something real about the off-screen rows.

**Flash over the WCH bridge, not the native USB port.** This board enumerates
two serial ports, and the built-in USB-Serial-JTAG one fails here — with the
stub, `Failed to write to target RAM (result was 0107: Checksum error)`; with
`--no-stub`, it erases first and *then* fails, which can leave the board with
no valid bootloader. The `cu.wchusbserial*` port writes first try.

## What to expect in the log

The board half comes up first:

```
I (2410) board: JD9365 800x1280, 2 lanes at 1500 Mbps
I (2411) GT911: TouchPad_ID:0x39,0x32,0x37
I (xxxx) a3d_p4_bench: JD9365 800x1280, benchmark then viewer
```

Then the demo prints `[A3D] ...` lines — the only ones `bench_table.py` reads.
The sweep runs off screen at a fixed 240x320 first (`present=0`), which is the
like-for-like comparison against every other board, and then at the panel's own
800x1280 (`present=1`), which is the row this project exists to produce.

**Three things worth reading before believing the numbers.**

- **`swap_us=0.0` on every row.** The DSI path takes a3d's native-endian
  RGB565 straight through, so unlike the CO5300 board there is no software byte
  swap to pay for. If instead the background comes out **pink**, `swapBytes` in
  `main.cpp` is wrong: 0x1082 byte-swapped is 0x8210, equal red and blue with
  the green pulled down, and it looks nothing like "the colours are a bit off".
- **Internal RAM.** A tile is 800 x 40 x 2 bytes of colour and the same again
  of depth, so two workers want **256,000 bytes** of internal SRAM before
  anything else is allocated. `no internal RAM for slot N` in the log is that
  failing; lower `TILE_H` in `../common/a3d_demo/a3d_demo_app.cpp` if it does —
  but note that changes the shared constant, so re-measure the other boards
  rather than publishing a table with two tile heights in it.
- **The tail, not the first windows.** Frame time falls for roughly five
  one-second windows after boot. The demo already discards a warm-up budget;
  this is only worth knowing if you read the raw lines.

## What it measured, 2026-09-09

Flashed and captured on the board, IDF 6.0.2, 46 checks and 0 failures. The
rows this project exists for, animated on two workers at the panel's own
800x1280:

| Model | Frame | FPS | Raster | Clear | Blit | Swap |
|---|---|---|---|---|---|---|
| FOX | 50.5 ms | 19.8 | 77.0 ms | 12.5 ms | 7.2 ms | 0.0 ms |
| CesiumMan | 70.1 ms | 14.3 | 105.8 ms | 12.6 ms | 7.2 ms | 0.0 ms |

`raster` is core time summed over both workers, so it exceeds the wall clock.
Two things are worth reading twice: `swap` is **0.0**, as the DSI path should
be, and **clear costs 12.5 ms against the blit's 7.2** - painting a million
pixels once is nearly twice the price of sending them to the panel.

**The spread of this binary is 0.19%.** It was flashed once and run twice, and
the worst row moved 0.19% while most moved under 0.1%. Quote three digits from
this benchmark; do not quote them from an application's frame counter.

**Its off-screen rows do not match the headless P4 column**, by -13.3% to
+8.9%. Do not read that as a toolchain result: this build differs from the
headless one in three ways at once - IDF 6.0.2 against 5.5.4, an L2 cache of
256 KB with 128-byte lines (inherited from the DSI example) against the IDF
default, and 16 MB QIO flash against 4 MB at 80 MHz. To untangle it, change one
at a time; the L2 cache lines are the first thing to try, since dropping them
from `sdkconfig.defaults` and rebuilding on 5.5.4 should reproduce the headless
column if that is what it was.

## Touch

Once the benchmark has finished, the viewer takes over and the panel is live.

| gesture | effect |
|---|---|
| one finger, drag | orbit - horizontal turns, vertical tilts |
| two fingers, pinch | zoom in and out |

**Both axes are inverted in `main.cpp`.** The GT911 reports in a frame turned
180 degrees from the one this panel is scanned in, so dragging right moved the
model left. `touchRead2()` maps `x -> (W-1) - x` and `y -> (H-1) - y`: a half
turn, not a transpose, which is why both axes flip and the width and height are
not exchanged. If your unit disagrees, that function is the one place to change.

**Pinch is a ratio, not an accumulated delta.** The zoom is the current finger
separation divided by the separation at the moment the second finger landed,
applied to the camera distance as it was at that moment. Returning the fingers
to where they started returns the zoom to where it started, however far it
wandered in between - which an accumulating implementation does not do.

The gesture lives in `examples/common/a3d_demo`, so every board gets it; the
board supplies only the fingers. `a3d_demo::Display::touchRead2` returns the
CONTACT COUNT rather than a bool, and the demo re-anchors whenever that count
changes - without which a second finger landing reads as one enormous drag and
the model jumps. It is optional: a board that sets only `touchRead` behaves
exactly as it did before pinch existed, which is why the S3 and C6 examples
needed no change.

## Turning the log into the table

```sh
cd ../common
python3 bench_table.py bench/*.log            # after saving this run into bench/
```

Save the captured output as `../common/bench/esp32p4_dsi.log`. The generator
will then have two ESP32-P4 rows with different panels, and it disambiguates a
duplicated board name by the panel it drove — the headless one keeps its
`none (headless run)` cell and this one gets `jd9365_dsi 800x1280`.

Nothing in the README tables is typed by hand, so regenerating is the whole
edit.

## What this does NOT change

The off-screen rows should come out the same as the headless P4 column, because
they are the same code at the same viewport on the same silicon. **If they do
not, that is the finding** — it would mean something in this build differs from
the headless one, and it is worth chasing before publishing the panel row.
