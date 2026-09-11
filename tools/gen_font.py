#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Eric Nam
# SPDX-License-Identifier: Apache-2.0

"""
Rasterize a TTF into a fixed-cell 1-bit ASCII font header for a3d.

a3d ships one hand-authored 5x7 font, which is a caption on a 240x320 panel
and half a millimetre of type on an 800x1280 one. This turns any monospace
TTF you have the right to redistribute into a header the software renderer
can draw directly.

    python3 tools/gen_font.py --size 16 --preview "Hg0@"      # look at it first
    python3 tools/gen_font.py --size 16 --output a3d_font_10x17.h
    python3 tools/gen_font.py --shipped src/backends/soft         # the three in the tree

--size is the nominal pixel size and NOT the cell: the cell is the face's own
advance by its ascent plus descent, so DejaVu Sans Mono at 16 comes out 10x17.
Preview first, then name the file after what it says.

ASCII 32..126 ONLY. There is no Unicode path and adding one is not a matter of
a flag: a CJK face is two to three megabytes, which belongs in a flash resource
with a loader rather than in a header every translation unit includes.

WHY THE SOURCE FONT MATTERS
---------------------------
Bitmaps rasterized from a TTF are a derivative of its outlines, so the output
carries the source font's licence into whatever links it. The default is DejaVu
Sans Mono, whose licence permits redistribution and derivative works. Fonts
under the operating system's own directories are refused by name - see
`--allow-nonfree` - because "it was already on my machine" is not a licence, and
a bitmap is exactly as derived as the outline it came from.

THE OUTPUT
----------
A header defining one `a3d::Font` and its bitmap:

    row-major, `stride` bytes a row, MSB = leftmost pixel, `h` rows a glyph,
    glyph `c` at bits[(c - first) * h * stride].

which is the layout `a3d::drawText()` in `src/backends/soft/a3d_font.h` reads.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

FIRST, LAST = 32, 126          # printable ASCII, and that is the whole range

# Where a redistributable DejaVu Sans Mono tends to be. matplotlib ships one
# with its licence beside it, which is why it is first: a machine that has ever
# drawn a graph in Python already has the font this tool wants.
DEJAVU_CANDIDATES = (
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
    "/usr/local/share/fonts/DejaVuSansMono.ttf",
    "~/Library/Fonts/DejaVuSansMono.ttf",
    "~/.local/share/fonts/DejaVuSansMono.ttf",
)

# Directories whose contents are licensed to the machine, not to the reader of
# a git repository. Rasterizing from these is refused unless the caller says
# they have the rights.
NONFREE_DIRS = (
    "/System/Library/Fonts",
    "/Library/Fonts",
    "C:\\Windows\\Fonts",
    "C:/Windows/Fonts",
)

# The fonts the tree carries, and the cell each one MUST come out as.
#
# The cell is asserted rather than merely reported: FreeType's metrics are what
# decide it, so a different Pillow or a different DejaVu release could quietly
# produce a 9x17 font under a file called 8x16. An assertion turns that into a
# failure at generation time instead of a layout that is subtly wrong on a
# board three weeks later.
#
#   (header, C++ name, face, px size, expected (w, h), threshold)
#
# Three sizes and not more: body text, a heading, and a title big enough to
# read across a room on an 800x1280 panel. The cells are what the face's own
# metrics produce after the trim pass - they are not round numbers, and naming
# the file after the real cell is better than choosing a px size that flatters
# the file name.
SHIPPED = (
    ("a3d_font_8x14.h",  "kFont8x14",  "DejaVuSansMono.ttf",      13, (8, 14),  128),
    ("a3d_font_12x21.h", "kFont12x21", "DejaVuSansMono-Bold.ttf", 20, (12, 21), 128),
    ("a3d_font_20x35.h", "kFont20x35", "DejaVuSansMono-Bold.ttf", 33, (20, 35), 128),
)


# --- finding a font ---------------------------------------------------------

def _matplotlib_font_dir() -> Path | None:
    """matplotlib's bundled DejaVu, if matplotlib is installed."""
    try:
        import importlib.util
        spec = importlib.util.find_spec("matplotlib")
        if spec is None or not spec.submodule_search_locations:
            return None
        d = Path(list(spec.submodule_search_locations)[0]) / "mpl-data" / "fonts" / "ttf"
        return d if d.is_dir() else None
    except Exception:
        return None


def find_font(name: str = "DejaVuSansMono.ttf") -> Path | None:
    """Locate a redistributable face by file name, or None."""
    d = _matplotlib_font_dir()
    if d is not None and (d / name).is_file():
        return d / name
    for c in DEJAVU_CANDIDATES:
        p = Path(c).expanduser()
        if p.name == name and p.is_file():
            return p
        cand = p.parent / name
        if cand.is_file():
            return cand
    return None


def check_redistributable(path: Path, allow: bool) -> None:
    s = str(path.resolve())
    for d in NONFREE_DIRS:
        if s.startswith(d):
            if allow:
                print(f"gen_font: WARNING: {s} is a system font; you are asserting "
                      f"you may redistribute bitmaps derived from it.", file=sys.stderr)
                return
            sys.exit(
                f"gen_font: refusing to rasterize {s}\n"
                f"  Bitmaps from a TTF are a derivative of its outlines, and fonts under\n"
                f"  {d} are licensed to this machine, not to whoever clones the result.\n"
                f"  Use DejaVu Sans Mono (the default; `pip install matplotlib` ships one\n"
                f"  with its licence), or pass --allow-nonfree if you hold the rights.")


# --- rasterizing ------------------------------------------------------------

def rasterize(ttf: Path, px: int, threshold: int, pad: int,
              allow_clip: bool, name: str, trim: bool = True):
    """Render ASCII 32..126 into a fixed cell. Returns a dict of the font."""
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        sys.exit("gen_font: needs Pillow.  python3 -m pip install pillow")

    font = ImageFont.truetype(str(ttf), px)

    # A monospace cell: the advance for the width, the face's own ascent plus
    # descent for the height. Every glyph is stored in that cell whether it
    # fills it or not, which is what makes the indexing a multiply.
    advance = int(round(font.getlength("M")))
    ascent, descent = font.getmetrics()
    w, h = advance + pad, ascent + descent
    stride = (w + 7) // 8

    # Render into a canvas with room around the cell, so ink that falls OUTSIDE
    # the cell can be counted rather than silently clipped. A glyph quietly
    # losing its left stem is the kind of thing that looks like a bad font
    # rather than like a bad cell.
    margin = px
    clipped = {}
    glyphs = []
    for code in range(FIRST, LAST + 1):
        img = Image.new("L", (w + 2 * margin, h + 2 * margin), 0)
        ImageDraw.Draw(img).text((margin, margin), chr(code), font=font, fill=255)
        px_ = img.load()

        outside = 0
        for y in range(img.height):
            for x in range(img.width):
                if px_[x, y] < threshold:
                    continue
                if not (margin <= x < margin + w and margin <= y < margin + h):
                    outside += 1
        if outside:
            clipped[chr(code)] = outside

        rows = []
        for y in range(h):
            row = bytearray()
            for b in range(stride):
                v = 0
                for bit in range(8):
                    x = b * 8 + bit
                    if x < w and px_[margin + x, margin + y] >= threshold:
                        v |= 0x80 >> bit
                row.append(v)
            rows.append(row)
        glyphs.append(rows)

    # A face's metrics reserve room for accents and descenders whether or not
    # ASCII uses them: DejaVu Sans Mono at 13px is a 17-row cell whose top four
    # rows are empty in all 95 glyphs. Dropping rows that no glyph lights is
    # lossless by construction, and it is worth doing - those rows cost bytes in
    # .rodata, and they cost the caller vertical space on a 320-row panel.
    top_trim = bot_trim = 0
    if trim:
        used = [any(any(g[y]) for g in glyphs) for y in range(h)]
        if any(used):
            top_trim = used.index(True)
            bot_trim = used[::-1].index(True)
            glyphs = [g[top_trim:h - bot_trim] for g in glyphs]
            h -= top_trim + bot_trim
            ascent -= top_trim

    data = bytearray()
    for g in glyphs:
        for row in g:
            data += row

    if clipped and not allow_clip:
        worst = sorted(clipped.items(), key=lambda kv: -kv[1])[:8]
        detail = ", ".join(f"{c!r}:{n}" for c, n in worst)
        sys.exit(
            f"gen_font: {len(clipped)} glyph(s) have ink outside the {w}x{h} cell "
            f"({detail}).\n"
            f"  The cell comes from the face's own advance and metrics, so this is\n"
            f"  usually a face that is not monospace. Widen it with --pad N, pick a\n"
            f"  monospace face, or pass --allow-clip if the loss is deliberate.")

    return {
        "name": name, "w": w, "h": h, "advance": advance, "ascent": ascent,
        "first": FIRST, "last": LAST, "stride": stride, "bits": bytes(data),
        "source": str(ttf), "px": px, "threshold": threshold, "pad": pad,
        "clipped": clipped, "trimmed": (top_trim, bot_trim),
    }


# --- emitting ---------------------------------------------------------------

def emit_header(f: dict, argv: str) -> str:
    n = f["name"]
    guard = f"A3D_FONT_{f['w']}X{f['h']}_H_"
    per = f["h"] * f["stride"]
    out = []
    out.append("// SPDX-FileCopyrightText: 2026 Eric Nam")
    out.append("// SPDX-License-Identifier: Apache-2.0")
    out.append("")
    out.append("/**")
    out.append(f" * @file a3d_font_{f['w']}x{f['h']}.h")
    out.append(f" * @brief {f['w']}x{f['h']} fixed-cell ASCII font, GENERATED - do not edit by hand.")
    out.append(" *")
    out.append(f" * Produced by: tools/gen_font.py {argv}")
    out.append(f" * Source face: {Path(f['source']).name} at {f['px']}px, threshold {f['threshold']}")
    out.append(" *")
    out.append(" * The bitmaps are a derivative of that face's outlines and carry its")
    out.append(" * licence. Regenerate with the command above rather than editing; see")
    out.append(" * docs/FONTS.md.")
    out.append(" *")
    out.append(f" * ASCII {f['first']}..{f['last']}, {per} bytes a glyph, "
               f"{per * (f['last'] - f['first'] + 1)} bytes of .rodata.")
    out.append(" */")
    out.append("")
    out.append(f"#ifndef {guard}")
    out.append(f"#define {guard}")
    out.append("")
    out.append('#include "backends/soft/a3d_font.h"')
    out.append("")
    out.append("namespace a3d {")
    out.append("")
    out.append(f"inline constexpr uint8_t {n}Bits[] = {{")
    bits = f["bits"]
    for g in range(f["last"] - f["first"] + 1):
        ch = chr(f["first"] + g)
        label = "space" if ch == " " else ("'\\''" if ch == "'" else f"'{ch}'")
        if ch == "\\":
            label = "'\\\\'"
        glyph = bits[g * per:(g + 1) * per]
        for i in range(0, per, 16):
            row = ", ".join(f"0x{b:02x}" for b in glyph[i:i + 16])
            tail = f"   // {label}" if i == 0 else ""
            out.append(f"    {row},{tail}")
    out.append("    };")
    out.append("")
    out.append("// Field order matches a3d::Font. Each value is on its own line with its")
    out.append("// name, because an aggregate initialiser is silently wrong when two")
    out.append("// adjacent fields of the same type are swapped.")
    out.append(f"inline constexpr Font {n} = {{")
    for field in ("w", "h", "advance", "ascent", "first", "last", "stride"):
        out.append(f"    {f[field]},{' ' * max(1, 6 - len(str(f[field])))}// {field}")
    out.append(f"    {n}Bits,")
    out.append("    };")
    out.append("")
    out.append("} // namespace a3d")
    out.append("")
    out.append(f"#endif // {guard}")
    out.append("")
    return "\n".join(out)


# --- reading one back -------------------------------------------------------

_ARRAY_RE = re.compile(r"inline constexpr uint8_t (\w+)Bits\[\] = \{(.*?)\n    \};", re.S)
_FONT_RE = re.compile(r"inline constexpr Font (\w+) = \{(.*?)\n    \};", re.S)


def parse_header(text: str) -> dict:
    """Read a generated header back into a dict.

    This exists so the preview and the tests work from the FILE rather than
    from the state that produced it. A generator that checks its own variables
    proves nothing about what it wrote.
    """
    am = _ARRAY_RE.search(text)
    fm = _FONT_RE.search(text)
    if am is None or fm is None:
        raise ValueError("not a generated a3d font header")
    bits = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", am.group(2)))
    nums = [int(v) for v in re.findall(r"^\s*(\d+),", fm.group(2), re.M)]
    if len(nums) != 7:
        raise ValueError(f"expected 7 scalar fields, found {len(nums)}")
    w, h, advance, ascent, first, last, stride = nums
    return {"name": fm.group(1), "w": w, "h": h, "advance": advance,
            "ascent": ascent, "first": first, "last": last, "stride": stride,
            "bits": bits}


def glyph_rows(f: dict, ch: str) -> list[str]:
    """One glyph as `w` characters a row, '#' for ink."""
    c = ord(ch)
    if not (f["first"] <= c <= f["last"]):
        c = ord("?")
    per = f["h"] * f["stride"]
    base = (c - f["first"]) * per
    rows = []
    for y in range(f["h"]):
        row = ""
        for x in range(f["w"]):
            b = f["bits"][base + y * f["stride"] + (x >> 3)]
            row += "#" if (b >> (7 - (x & 7))) & 1 else "."
        rows.append(row)
    return rows


def preview(f: dict, s: str, out=sys.stdout) -> None:
    """Print a string as ASCII art, one line of cells."""
    art = [glyph_rows(f, ch) for ch in s]
    print(f"{f['name']}  {f['w']}x{f['h']} cell, advance {f['advance']}, "
          f"ascent {f['ascent']}", file=out)
    for y in range(f["h"]):
        line = " ".join(g[y] for g in art)
        mark = " <- baseline" if y == f["ascent"] - 1 else ""
        print(line + mark, file=out)


# --- main -------------------------------------------------------------------

def generate_shipped(outdir: Path, allow_nonfree: bool) -> int:
    rc = 0
    for header, name, face, px, cell, thr in SHIPPED:
        ttf = find_font(face)
        if ttf is None:
            sys.exit(f"gen_font: cannot find {face}.  See docs/FONTS.md; "
                     f"`python3 -m pip install matplotlib` ships one.")
        check_redistributable(ttf, allow_nonfree)
        f = rasterize(ttf, px, thr, 0, False, name)
        if (f["w"], f["h"]) != cell:
            sys.exit(f"gen_font: {face} at {px}px came out {f['w']}x{f['h']}, "
                     f"not the {cell[0]}x{cell[1]} this tree ships as {header}.\n"
                     f"  The face or Pillow differs from the one the table was written\n"
                     f"  against. Do not rename the file around it - work out which.")
        argv = f"--ttf <{face}> --size {px} --name {name} --output {header}"
        (outdir / header).write_text(emit_header(f, argv))
        print(f"gen_font: {outdir / header}  {f['w']}x{f['h']}  "
              f"{len(f['bits'])} bytes from {ttf}")
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(
        description="rasterize a TTF into a fixed-cell 1-bit ASCII font header",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""examples:
  # the three fonts this tree carries, from DejaVu Sans Mono
  python3 tools/gen_font.py --shipped src/backends/soft

  # look at what you are about to commit, before you commit it. --size is not
  # the cell: this one prints `kFont10x17  10x17 cell, advance 10, ascent 13`
  python3 tools/gen_font.py --size 16 --preview "Hamburg 0@"

  # then write it, named after the cell the line above reported
  python3 tools/gen_font.py --size 16 --output src/backends/soft/a3d_font_10x17.h

  # a face of your own
  python3 tools/gen_font.py --ttf ~/fonts/Inconsolata.ttf --size 24 \\
                            --name kFontMenu --output main/font_menu.h
""")
    ap.add_argument("--ttf", metavar="FILE",
                    help="source face (default: DejaVu Sans Mono, found on this machine)")
    ap.add_argument("--size", type=int, default=16, metavar="PX",
                    help="nominal pixel size; the CELL that comes out is the face's "
                         "advance by its ascent+descent, which is usually bigger (default 16)")
    ap.add_argument("--threshold", type=int, default=128, metavar="N",
                    help="grey level that counts as ink, 1..255 (default 128). "
                         "The output is 1-bit: there is no anti-aliasing")
    ap.add_argument("--no-trim", dest="trim", action="store_false",
                    help="keep rows the face reserves but no ASCII glyph lights "
                         "(they are dropped by default, which is lossless)")
    ap.add_argument("--pad", type=int, default=0, metavar="N",
                    help="widen the cell by N columns (default 0)")
    ap.add_argument("--name", default=None, metavar="IDENT",
                    help="C++ identifier for the font (default kFont<W>x<H>)")
    ap.add_argument("--output", metavar="FILE", help="write the header here (default: stdout)")
    ap.add_argument("--preview", metavar="TEXT", nargs="?", const="Hamburg 0@",
                    help="print TEXT as ASCII art from the generated bytes and exit")
    ap.add_argument("--shipped", metavar="DIR",
                    help="regenerate the fonts this tree carries, into DIR")
    ap.add_argument("--allow-clip", action="store_true",
                    help="keep going when ink falls outside the cell")
    ap.add_argument("--allow-nonfree", action="store_true",
                    help="rasterize a system font anyway; you are asserting the rights")
    ap.add_argument("--list", action="store_true",
                    help="print the faces this tool can find and exit")
    args = ap.parse_args()

    if args.list:
        d = _matplotlib_font_dir()
        print(f"matplotlib fonts: {d if d else '(matplotlib not installed)'}")
        for face in sorted({s[2] for s in SHIPPED}):
            print(f"  {face:28s} {find_font(face) or '(not found)'}")
        return 0

    if args.shipped:
        return generate_shipped(Path(args.shipped), args.allow_nonfree)

    ttf = Path(args.ttf).expanduser() if args.ttf else find_font()
    if ttf is None:
        sys.exit("gen_font: no DejaVu Sans Mono on this machine. Pass --ttf FILE, or\n"
                 "  `python3 -m pip install matplotlib`, which ships one. --list shows\n"
                 "  where it looked.")
    if not ttf.is_file():
        sys.exit(f"gen_font: no such file: {ttf}")
    check_redistributable(ttf, args.allow_nonfree)

    f = rasterize(ttf, args.size, args.threshold, args.pad, args.allow_clip,
                  args.name or "", args.trim)
    if not f["name"]:
        f["name"] = f"kFont{f['w']}x{f['h']}"

    argv = " ".join(sys.argv[1:])
    text = emit_header(f, argv)

    if args.preview is not None:
        # Read back what was EMITTED, not what is in memory: the point of a
        # preview is to prove the file, and a bad emitter is exactly the fault
        # a preview off the in-memory table cannot see.
        preview(parse_header(text), args.preview)
        return 0

    if args.output:
        Path(args.output).write_text(text)
        print(f"gen_font: {args.output}  {f['w']}x{f['h']}  "
              f"{len(f['bits'])} bytes from {ttf}", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
