<!--
SPDX-FileCopyrightText: 2026 Eric Nam
SPDX-License-Identifier: Apache-2.0
-->

# Fonts

a3d draws text with a fixed-cell 1-bit bitmap font. One 5x7 is built in, three
larger sizes are generated and checked in, and `tools/gen_font.py` makes more
from any monospace TTF you have the right to redistribute.

**ASCII 32..126 only.** There is no Unicode path. A CJK face is two to three
megabytes, which belongs in a flash resource with a loader rather than in a
header that every translation unit includes — so if you need one, that is a
different design, not a flag on this tool.

---

## What is already in the tree

| include | cell | advance | `.rodata` | for |
|---|---|---|---|---|
| `src/backends/soft/a3d_soft_text.h` | 5x7 | 6 | 475 B | captions, HUD rows, anything on 240x320 |
| `src/backends/soft/a3d_font_8x14.h` | 8x14 | 8 | 1,330 B | body text |
| `src/backends/soft/a3d_font_12x21.h` | 12x21 | 12 | 3,990 B | headings, readouts (bold) |
| `src/backends/soft/a3d_font_20x35.h` | 20x35 | 20 | 9,975 B | titles on an 800x1280 panel (bold) |

A header per size, and you pay only for the ones you include. The three
generated sizes come from DejaVu Sans Mono; the 5x7 was authored by hand.

`src/backends/soft/a3d_font.h` has the `Font` struct and the blitter that reads all
four, including the 5x7 — `a3d::font5x7()` hands the built-in table back as a
`Font`, transposed to row-major at compile time, so switching sizes is changing
one reference rather than one function name.

---

## Drawing with them

```cpp
#include "backends/soft/a3d_font.h"        // Font, drawText, font5x7()
#include "backends/soft/a3d_font_12x21.h"  // kFont12x21

// fb is 16-bit RGB565, `stride` words a row, the target is w x h.
// `y` is the BASELINE, not the top of the cell.
a3d::drawText(fb, stride, w, h, /*x*/ 8, /*y*/ 40, "READY", 0xFFFF, a3d::kFont12x21);

// Centre it: fontTextWidth() includes the advance after the last glyph.
const int tw = a3d::fontTextWidth(a3d::kFont12x21, "READY");
a3d::drawText(fb, stride, w, h, (w - tw) / 2, 72, "READY", 0xFFFF, a3d::kFont12x21);

// The built-in 5x7 through the same call, at double size.
a3d::drawText(fb, stride, w, h, 4, 96, "fps 30", 0x07E0, a3d::font5x7(), /*scale*/ 2);
```

(`a3d::rgb565(r, g, b)` packs a colour if you would rather not write hex; it
lives in `src/backends/soft/a3d_canvas.h`. This block is compiled verbatim by the
development repository's font test, so it cannot rot.)

Clipping is per pixel, so a string may run off any edge without taking the rest
of the line with it. `drawText5x7()` still exists and is unchanged; drawing the
5x7 through `drawText()` produces pixel-identical output, which `test_font.cpp`
asserts.

Inside a tile, pass the tile's own buffer and its row count — the same as any
other 2D work in this tree — and subtract the tile's top row from `y`.

---

## Generating another size

### What you need

* **Pillow.** `python3 -m pip install pillow`
* **A monospace TTF you may redistribute.** The default is DejaVu Sans Mono;
  `python3 -m pip install matplotlib` ships one with its licence beside it, and
  the tool finds it there without being told. `--list` shows where it looked.

### The commands

```bash
# What can this machine find?
python3 tools/gen_font.py --list

# Look at a size BEFORE committing it. Reads back the bytes it just emitted.
python3 tools/gen_font.py --size 24 --preview "Hamburg 0@"

# Write one where the renderer can include it.
python3 tools/gen_font.py --size 24 --output src/backends/soft/a3d_font_14x25.h

# Your own face, your own identifier, into your own project.
python3 tools/gen_font.py --ttf ~/fonts/Inconsolata-Regular.ttf --size 28 \
                          --name kFontMenu --output main/font_menu.h

# Regenerate the three this tree carries.
python3 tools/gen_font.py --shipped src/backends/soft
```

`--preview` prints the glyphs as ASCII art with the baseline marked:

```
$ python3 tools/gen_font.py --size 13 --preview "Hg"
kFont8x14  8x14 cell, advance 8, ascent 11
........ ........
........ ........
.#....#. ........
.#....#. ........
.#....#. ..#####.
.#....#. .##..##.
.######. .#...##.
.#....#. .#...##.
.#....#. .#...##.
.#....#. .##..##.
.#....#. ..#####. <- baseline
........ .....#..
........ .....#..
........ ..###...
```

Then include the header and name the font — the identifier is `kFont<W>x<H>`
unless you passed `--name`.

### Every flag

| flag | what it does |
|---|---|
| `--ttf FILE` | source face. Default: DejaVu Sans Mono, found on this machine |
| `--size PX` | nominal pixel size. **Not** the cell — see below |
| `--name IDENT` | C++ identifier. Default `kFont<W>x<H>` |
| `--output FILE` | write the header here. Default: stdout |
| `--preview [TEXT]` | print TEXT as ASCII art from the emitted bytes, and exit |
| `--shipped DIR` | regenerate the three fonts this tree carries |
| `--threshold N` | grey level that counts as ink, 1..255. Default 128 |
| `--no-trim` | keep rows the face reserves but no ASCII glyph lights |
| `--pad N` | widen the cell by N columns |
| `--allow-clip` | keep going when ink falls outside the cell |
| `--allow-nonfree` | rasterize a system font anyway — you are asserting the rights |
| `--list` | print the faces the tool can find, and exit |

### `--size` is not the cell size

The cell is the face's own advance by its ascent plus descent, which is
generally taller than the pixel size you asked for. DejaVu Sans Mono at
`--size 13` is an 8x14 cell. The tool prints the cell it produced and names the
file and the identifier after it — pick the size you want by looking at what
comes out, not by computing it.

Rows that no ASCII glyph lights are dropped, which is lossless: a face reserves
space for accents that this font does not contain, and at 13px that was four
blank rows on every one of the 95 glyphs. `--no-trim` keeps them if you need
the cell to match the face's metrics exactly.

---

## The licence, which is the part that matters

**Bitmaps rasterized from a TTF are a derivative of its outlines.** The header
this tool writes carries the source face's licence into whatever links it, so
the face has to be one you may redistribute.

Fonts under `/System/Library/Fonts`, `/Library/Fonts` and `C:\Windows\Fonts`
are **refused by name**. Those are licensed to the machine, not to whoever
clones your repository, and "it was already installed" is not a licence.
`--allow-nonfree` overrides it, and doing so is you asserting you hold the
rights.

Safe choices: DejaVu Sans Mono (Bitstream Vera licence — redistribution and
derivative works are explicitly permitted), or any face under the SIL Open Font
License such as Inconsolata, JetBrains Mono or Fira Code.

Each generated header records the face, the pixel size, the threshold and the
command that produced it, so the provenance travels with the bytes.

---

## Checking one

The development repository's `tests/test_font.py` and `tests/test_font.cpp` run
under `ctest`. They are not part of this distribution, but what they check is
worth knowing before you add a font of your own, because it is what goes wrong:

* **A table of exactly the right length holding the wrong bits.** Every size
  check passes; the font is unreadable. The checks are therefore about content
  — ink per glyph, 95 distinct glyphs, clean padding bits past the last column.
* **A rasterization that produced nothing.** 95 glyphs of all zeros has the
  right length and the right count. Space must be blank and every other glyph
  must not be.
* **A wrong `ascent`.** Invisible in the bitmap, and it moves every line of
  text on the panel. `'H'` has no descender, so its lowest lit row must be the
  baseline.
* **Drift.** The checked-in headers are compared against what the generator
  emits today, so a hand-edited generated file is a failure rather than a
  surprise months later. That check needs Pillow and the face, and skips with a
  message without them.

Eight deliberate mutations of the blitter and the tables were used to falsify
the C++ side; all eight were caught. If you generate a font, the cheapest
version of all of this is `--preview`: look at the glyphs before you commit
them.

---

## What this deliberately does not do

* **Unicode, CJK, or any codepoint above 126.** See the top of this file.
* **Anti-aliasing.** The output is 1-bit. A grey edge is worse than no edge on
  a dithered or reflective panel, and supporting both doubles every table.
* **Proportional metrics.** The cell is fixed. `kFontAdvance`-style arithmetic
  is load-bearing in the demos' layout code, and a variable advance would make
  centring and truncation a per-string measurement.
