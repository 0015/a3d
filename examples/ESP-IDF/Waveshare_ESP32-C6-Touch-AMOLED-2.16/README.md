# a3d on a Waveshare ESP32-C6-Touch-AMOLED-2.16


> **Demo build — no benchmark.** This is the copy under `examples/ESP-IDF/`:
> it boots straight into the viewer. The measuring version, which runs the full
> sweep first and prints the rows the published tables are made from, is the
> original at `examples/Waveshare_ESP32-C6-Touch-AMOLED-2.16`. Nothing else differs.

Two skinned, textured, animated models on a 480x480 AMOLED. Drag to orbit;
the buttons along the bottom change model, animation clip and distance.

The benchmark runs first and prints its results over USB; then the viewer
starts. See [`../common/README.md`](../common/README.md) for what the benchmark
measures.

## Hardware

| | |
|---|---|
| MCU | ESP32-C6, **one** RISC-V core at 160 MHz, RV32IMAC |
| FPU | **None.** See below |
| PSRAM | None |
| Display | SH8601, 480x480, QSPI at 40 MHz — SCLK 0, D0-D3 on 1-4, CS 5 |
| Touch | CST9217 on I2C0 — SDA 8, SCL 7, INT 15, RST 11 |
| Power | AXP2101; the panel rail is ALDO3, so the PMIC comes up first |

## What this board is here to show

**The ESP32-C6 has no floating-point unit.** a3d's rasterizer is float
throughout, so every multiply, add and compare in the inner loop becomes a
libgcc call. It renders correctly and roughly an order of magnitude slower than
a part with an FPU. No compiler flag recovers this: `-O2` is already on, and
there is no instruction to emit.

The point of measuring it is to replace an impression with a number. The
skinning stage — pure float math with no memory traffic to hide behind — is
where the gap is widest, and the benchmark reports it separately.

There is also no PSRAM, so the benchmark's PSRAM placement runs skip themselves
and the whole working set has to fit in 512 KB of SRAM. That is what
`tileSlotCount()` is for: on one core a3d allocates one tile framebuffer, not
two.

## Build

```bash
idf.py set-target esp32c6
idf.py -p <port> flash monitor
```
