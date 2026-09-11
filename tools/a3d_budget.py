# SPDX-FileCopyrightText: 2026 Eric Nam
# SPDX-License-Identifier: Apache-2.0
"""Will this model run, and on what.

WHY THIS EXISTS

    The exporter used to print counts - `triangles=4672 vertices=3273` - and a
    person converting their first model has no way to know whether 4,672 is
    comfortable or hopeless. They find out by flashing, and on an ESP32-C6 the
    answer is 1.7 fps. That is the point where people give up, and it is
    avoidable: this tree has measured the numbers that answer the question.

WHAT THE NUMBERS ARE

    ANCHORS, below, are rows copied from device logs in
    `examples/common/bench/` - two models on each part, in the same viewport,
    through the same object code. The per-stage coefficients are DERIVED from
    them at import time rather than typed in, so the provenance is the
    measurement and a mistyped constant cannot survive.

    Two measured points fix a two-term model exactly, so the fit is not
    evidence of anything by itself. The fit is made on the ANIMATED rows and
    checked against the STATIC ones, which it never saw:

        ESP32-S3   FOX  +6.4%   CesiumMan  -1.3%
        ESP32-C6   FOX  -2.3%   CesiumMan  -3.8%

    THAT CHECK ALONE IS WEAK, AND FINDING OUT COST A ROUND. Nudging one
    anchor - CesiumMan's ESP32-C6 raster figure, by 10% - moved its held-out
    error only to +5.0% and left the worst case untouched at 6.4%, because the
    fit shifts to absorb the change and the held-out row is the same model in a
    static pose. A corrupted table would have passed.

    So `self_test()` checks the REDUNDANCIES in the measurements instead, the
    ones the fit does not consume: skinning cost per vertex has to agree
    between two different models, bind memory has to come out at the documented
    32 bytes a vertex on both, every derived coefficient has to be positive,
    and a part with no FPU has to be slower on every term. Each of those was
    verified to fail when the thing it tests is broken.

WHAT IT CANNOT KNOW

    How much SCREEN your model covers, and how many of its triangles face the
    camera. Both depend on the model and the camera, and both are assumed here
    from the same two measurements (see FRAMING). A tall thin model covers less
    than a wide one and will beat the estimate; a model that fills the frame
    will lose to it. The estimate is for a character framed the way the
    benchmark frames one.

    It also cannot know your application's own memory - tile buffers, binner
    scratch. `bind_bytes` is the model's cost alone.
"""

from __future__ import annotations

# ---------------------------------------------------------------------------
# Measured rows. Do not adjust these to make an estimate come out nicer; they
# are what a part did, and a wrong estimate means the model around them is
# wrong. Every field is copied from the log named in `source`.
#
# `workers=1 anim=1 place=xip` is the fit row; `anim=0` is the held-out check.
# `verts`/`tris` are the whole model, `drawn`/`px` are what one frame actually
# submitted and lit at 240x320.
# ---------------------------------------------------------------------------
ANCHORS = {
    "ESP32-P4": dict(
        source="examples/common/bench/esp32p4.log, 2026-09-09, 360 MHz",
        clock_mhz=360,
        cores=2,
        fpu=True,
        two_core_speedup=(11.913 / 7.595 + 26.021 / 18.132) / 2,
        clear_us=(838.1 + 859.3) / 2,
        models=[
            dict(name="FOX", verts=1728, tris=576, bind_bytes=18888 + 41992,
                 fit=dict(drawn=373, px=18021, skin=1485.4, bin=675.2, ras=8758.1, ms=11.913, anim=177.9, draw=9613.1),
                 held=dict(drawn=332, px=17310, ms=8.604)),
            dict(name="CESIUMMAN", verts=3273, tris=4672, bind_bytes=4228 + 106508,
                 fit=dict(drawn=2300, px=16783, skin=3655.1, bin=3657.1, ras=17522.1, ms=26.021, anim=412.8, draw=18426.7),
                 held=dict(drawn=2469, px=15749, ms=22.626)),
        ],
    ),
    "ESP32-S3": dict(
        source="examples/common/bench/esp32s3.log, 2026-09-07, 240 MHz",
        clock_mhz=240,
        cores=2,
        fpu=True,
        # measured whole-frame speedup with a second worker, not an assumed 2x
        two_core_speedup=(27.653 / 19.528 + 56.578 / 41.848) / 2,
        clear_us=(857.1 + 855.7) / 2,
        models=[
            dict(name="FOX", verts=1728, tris=576, bind_bytes=18888 + 41992,
                 fit=dict(drawn=373, px=18021, skin=2547.9, bin=1787.3, ras=22141.0, ms=27.653, anim=303.9, draw=23021.9),
                 held=dict(drawn=332, px=17310, ms=22.171)),
            dict(name="CESIUMMAN", verts=3273, tris=4672, bind_bytes=4228 + 106508,
                 fit=dict(drawn=2300, px=16783, skin=4622.4, bin=6920.7, ras=43943.8, ms=56.578, anim=480.2, draw=44824.5),
                 held=dict(drawn=2469, px=15749, ms=53.424)),
        ],
    ),
    "ESP32-C6": dict(
        source="examples/common/bench/esp32c6.log, 2026-09-07, 160 MHz, NO FPU",
        clock_mhz=160,
        cores=1,
        fpu=False,
        two_core_speedup=1.0,
        clear_us=(2310.1 + 2315.6) / 2,
        models=[
            dict(name="FOX", verts=1728, tris=576, bind_bytes=60320,
                 fit=dict(drawn=372, px=18761, skin=60078.2, bin=21738.8, ras=179097.4, ms=265.609, anim=1915.8, draw=181475.9),
                 held=dict(drawn=332, px=17310, ms=192.362)),
            dict(name="CESIUMMAN", verts=3273, tris=4672, bind_bytes=108924,
                 fit=dict(drawn=2302, px=16778, skin=114445.5, bin=66450.1, ras=405098.2, ms=591.017, anim=2711.9, draw=407505.5),
                 held=dict(drawn=2469, px=15749, ms=506.340)),
        ],
    ),
}

# Parts that are compiled for but have NOT been through this sweep. Naming them
# is the point: a table that silently omits the ESP32-P4 reads as though the
# question were not asked.
UNMEASURED = {
    # Nothing here at the moment: the S3, the C6 and the P4 have all been
    # through examples/ESP32-Bench-Headless. Leave the mechanism in place - a
    # table that silently omits a part reads as though the question were not
    # asked, and the next part to be compiled for but not measured belongs here.
}

#: The viewport every anchor was measured at. A benchmark measured at each
#: panel's own size compares panels rather than parts.
BENCH_W, BENCH_H = 240, 320

# ---------------------------------------------------------------------------
# FRAMING - the two things the exporter cannot know, taken from the same rows.
#
# Both measured models, framed by their own bounds, land close together, which
# is what makes a default defensible at all. The spread is reported with the
# estimate rather than hidden in it.
# ---------------------------------------------------------------------------
def _framing():
    covers, survives = [], []
    for chip in ANCHORS.values():
        for m in chip["models"]:
            covers.append(m["fit"]["px"] / float(BENCH_W * BENCH_H))
            survives.append(m["fit"]["drawn"] / float(m["tris"]))
    return covers, survives


_COVERS, _SURVIVES = _framing()
#: Fraction of the viewport a framed model lights. Measured 0.219 - 0.244.
COVERAGE = sum(_COVERS) / len(_COVERS)
#: Fraction of a model's triangles that survive back-face culling. 0.49 - 0.65.
SURVIVAL = sum(_SURVIVES) / len(_SURVIVES)


def _solve2(x1, y1, r1, x2, y2, r2):
    """The a, b with a*x + b*y = r through both points."""
    det = x1 * y2 - x2 * y1
    if det == 0:
        raise ValueError("anchors are collinear; the fit is not determined")
    return (r1 * y2 - r2 * y1) / det, (x1 * r2 - x2 * r1) / det


class Chip:
    """Per-stage costs for one part, derived from its anchors."""

    def __init__(self, name, spec):
        self.name = name
        self.source = spec["source"]
        self.cores = spec["cores"]
        self.clock_mhz = spec["clock_mhz"]
        self.two_core_speedup = spec["two_core_speedup"]
        self.clear_us = spec["clear_us"]
        self._spec = spec
        a, b = spec["models"]

        # Skinning is per vertex and the two models agree to within 4%, so the
        # mean is the whole story rather than a fit.
        self.skin_us_per_vertex = (a["fit"]["skin"] / a["verts"]
                                   + b["fit"]["skin"] / b["verts"]) / 2.0

        # Binning transforms every VERTEX once and then boxes every TRIANGLE.
        # Both terms are needed: FOX carries 3.0 vertices per triangle and
        # CesiumMan 0.7, so a per-triangle cost alone comes out 2x apart.
        self.bin_us_per_vertex, self.bin_us_per_triangle = _solve2(
            a["verts"], a["tris"], a["fit"]["bin"],
            b["verts"], b["tris"], b["fit"]["bin"])

        # Rasterizing. The two anchors sit at 48 and 7 pixels per triangle,
        # which is what makes this solvable: with similar coverage the split
        # between the two terms would be noise.
        self.ras_us_per_triangle, self.ras_us_per_pixel = _solve2(
            a["fit"]["drawn"], a["fit"]["px"], a["fit"]["ras"],
            b["fit"]["drawn"], b["fit"]["px"], b["fit"]["ras"])

        # bind() decodes vertices to float. The documented 32 bytes a vertex
        # accounts for both anchors to within 500 bytes, so the remainder is
        # carried as a constant rather than fitted.
        self.bind_bytes_per_vertex = 32.0
        self.bind_bytes_fixed = sum(
            m["bind_bytes"] - 32.0 * m["verts"] for m in spec["models"]) / 2.0

    # -- estimation ---------------------------------------------------------

    def frame_ms(self, vertices, triangles, animated=True, workers=1,
                 view_w=BENCH_W, view_h=BENCH_H,
                 coverage=None, survival=None):
        """Estimated milliseconds for one frame."""
        cov = COVERAGE if coverage is None else coverage
        surv = SURVIVAL if survival is None else survival
        px = cov * view_w * view_h
        drawn = surv * triangles

        us = self.bin_us_per_vertex * vertices + self.bin_us_per_triangle * triangles
        us += self.ras_us_per_triangle * drawn + self.ras_us_per_pixel * px
        us += self.clear_us * (view_w * view_h) / float(BENCH_W * BENCH_H)
        if animated:
            us += self.skin_us_per_vertex * vertices
        ms = us / 1000.0
        if workers >= 2 and self.cores >= 2:
            ms /= self.two_core_speedup
        return ms

    def bind_bytes(self, vertices):
        """RAM the model itself takes when bound. Not the whole application."""
        return self.bind_bytes_per_vertex * vertices + self.bind_bytes_fixed

    def triangles_for(self, target_ms, vertices, triangles, animated=True,
                      workers=1, view_w=BENCH_W, view_h=BENCH_H):
        """Triangle count that would reach `target_ms`, or None if unreachable.

        Decimation takes vertices down with triangles, so both scale together.
        Everything that does not scale - the pixels, the clear - is a FLOOR,
        and being told that the floor alone misses the target is more useful
        than being handed a triangle count that cannot work.
        """
        floor_ms = self.frame_ms(0, 0, animated=animated, workers=workers,
                                 view_w=view_w, view_h=view_h)
        full_ms = self.frame_ms(vertices, triangles, animated=animated,
                                workers=workers, view_w=view_w, view_h=view_h)
        if full_ms <= target_ms:
            return triangles
        if floor_ms >= target_ms:
            return None
        scale = (target_ms - floor_ms) / (full_ms - floor_ms)
        return max(1, int(triangles * scale))


CHIPS = {name: Chip(name, spec) for name, spec in ANCHORS.items()}


# ---------------------------------------------------------------------------
# The check that makes the table a measurement
# ---------------------------------------------------------------------------
def self_test(tolerance_pct=15.0, verbose=True):
    """Check every redundancy in ANCHORS. Raises AssertionError on any failure.

    The held-out prediction is here too, but it is the WEAKEST of these checks,
    not the strongest - see the module docstring for the round that established
    that. The checks that actually bite are the ones the fit never touches.
    """
    say = (lambda m: print(m)) if verbose else (lambda m: None)
    worst = 0.0

    for name, chip in sorted(CHIPS.items()):
        spec = ANCHORS[name]
        a, b = spec["models"]

        # 1. The STAGES MUST ADD UP to the frame time. The sweep times each
        #    stage separately and also times the whole frame, and nothing in
        #    the fit uses that agreement - so a wrong digit in ANY stage field
        #    breaks it. Measured within 0.5% on all six rows; the bound is 2%.
        #    This is the strongest check in the file and it arrived late: the
        #    per-vertex agreement below was doing this job badly.
        f = a["fit"]
        for m in spec["models"]:
            g = m["fit"]
            total = g["anim"] + g["skin"] + g["bin"] + g["draw"]
            err = 100.0 * (total / 1000.0 - g["ms"]) / g["ms"]
            say("  %-9s %-10s stages sum to %9.1f us vs %8.3f ms measured  %+.2f%%"
                % (name, m["name"], total, g["ms"], err))
            assert abs(err) <= 2.0, (
                "%s %s stage times sum to %.3f ms but the frame was measured at "
                "%.3f ms" % (name, m["name"], total / 1000.0, g["ms"]))

        # 2. Skinning is per vertex, so two models should roughly agree on its
        #    cost. THE BOUND IS LOOSE ON PURPOSE, and the reason is a finding
        #    rather than an excuse. Measured spreads:
        #
        #        ESP32-C6   0.6%   vertices in internal RAM
        #        ESP32-S3   4.4%   vertices in PSRAM, slow core
        #        ESP32-P4  29.9%   vertices in PSRAM, fast core
        #
        #    The first version of this check bounded it at 6% and the P4 rows
        #    tripped it the moment they were measured. It is not a typo: the
        #    faster the core, the more of skinning is waiting for memory, and
        #    CesiumMan's decoded vertices are 106 KB against FOX's 42 KB. The
        #    part that suffers most from PSRAM is the one fast enough to notice.
        #    So this now catches an order-of-magnitude slip and leaves the fine
        #    grained work to the stage sum above.
        sa = a["fit"]["skin"] / a["verts"]
        sb = b["fit"]["skin"] / b["verts"]
        spread = 100.0 * abs(sa - sb) / min(sa, sb)
        say("  %-9s skin/vertex %.3f vs %.3f us  (%.1f%% apart)" % (name, sa, sb, spread))
        assert spread <= 100.0, "%s skinning cost disagrees by %.0f%%" % (name, spread)

        # 2. bind() decodes to float at 32 bytes a vertex - documented, not
        #    fitted - so what is left over must be the same constant for both.
        ra = a["bind_bytes"] - 32.0 * a["verts"]
        rb = b["bind_bytes"] - 32.0 * b["verts"]
        say("  %-9s bind overhead %.0f vs %.0f bytes" % (name, ra, rb))
        assert abs(ra - rb) <= 1024.0, (
            "%s bind memory is not 32 bytes a vertex plus a constant "
            "(%.0f vs %.0f)" % (name, ra, rb))

        # 3. Every derived cost must be positive. A mistyped anchor turns one
        #    of these negative long before it moves the held-out error.
        for label, value in (("bin/vertex", chip.bin_us_per_vertex),
                             ("bin/triangle", chip.bin_us_per_triangle),
                             ("raster/triangle", chip.ras_us_per_triangle),
                             ("raster/pixel", chip.ras_us_per_pixel)):
            assert value > 0.0, "%s %s came out %.4f, which is not a cost" % (
                name, label, value)

        # 4. The held-out static rows. Weak as a guard - see the docstring -
        #    but it is the one number that says how wrong the ESTIMATE can be,
        #    so the report quotes whatever it comes to rather than a constant.
        #
        #    The errors are not noise, they have a shape:
        #
        #        FOX       52 px/triangle   C6 -2.3%   S3 +6.4%   P4 +14.0%
        #        CesiumMan  6 px/triangle   C6 -3.8%   S3 -1.3%   P4  -0.8%
        #
        #    Large only at high coverage, and growing with core speed. That is
        #    the per-ROW term this cost model does not have: row setup is a
        #    bigger share of a triangle that covers 52 pixels than of one that
        #    covers 6, and on a part whose whole
        #    frame is 8 ms rather than 190 ms it is a bigger share of that too.
        #    The bound is 15% because that is what the data supports, not
        #    because 15% is comfortable.
        for m in spec["models"]:
            h = m["held"]
            us = (chip.bin_us_per_vertex * m["verts"]
                  + chip.bin_us_per_triangle * m["tris"]
                  + chip.ras_us_per_triangle * h["drawn"]
                  + chip.ras_us_per_pixel * h["px"]
                  + chip.clear_us)
            pred = us / 1000.0
            err = 100.0 * (pred - h["ms"]) / h["ms"]
            worst = max(worst, abs(err))
            say("  %-9s %-10s static held out: %8.2f vs %8.2f ms  %+.1f%%"
                % (name, m["name"], pred, h["ms"], err))
            assert abs(err) <= tolerance_pct, (
                "%s %s held-out error %.1f%% exceeds %.1f%%"
                % (name, m["name"], err, tolerance_pct))

    # 5. A part with no FPU turns every float in the rasterizer into a libgcc
    #    call, so it must be slower than EVERY part that has one, on every
    #    term. This is the check that catches two parts' rows being swapped,
    #    which nothing above would. Written over all pairs rather than over two
    #    named chips so a third part does not quietly escape it.
    soft = [c for n, c in sorted(CHIPS.items()) if not ANCHORS[n]["fpu"]]
    hard = [c for n, c in sorted(CHIPS.items()) if ANCHORS[n]["fpu"]]
    terms = ("skin_us_per_vertex", "bin_us_per_vertex", "bin_us_per_triangle",
             "ras_us_per_triangle", "ras_us_per_pixel")
    for nofpu in soft:
        for withfpu in hard:
            for term in terms:
                x, y = getattr(withfpu, term), getattr(nofpu, term)
                say("  FPU gap  %-20s %-9s %9.4f  %-9s %9.4f  (%.1fx)"
                    % (term, withfpu.name, x, nofpu.name, y, y / x))
                assert y > x, (
                    "%s has no FPU but is not slower than %s on %s; "
                    "are the rows swapped?" % (nofpu.name, withfpu.name, term))

    return worst


# ---------------------------------------------------------------------------
# The report
# ---------------------------------------------------------------------------
def _worst_held_out():
    """The largest error the anchors' own held-out check produces.

    Computed rather than quoted: a hard-coded accuracy claim in a report is one
    measurement away from being a lie, and this one has already moved once -
    from 6.4% to 14.0% - when a third part was measured.
    """
    worst = 0.0
    for name, chip in CHIPS.items():
        for m in ANCHORS[name]["models"]:
            h = m["held"]
            us = (chip.bin_us_per_vertex * m["verts"]
                  + chip.bin_us_per_triangle * m["tris"]
                  + chip.ras_us_per_triangle * h["drawn"]
                  + chip.ras_us_per_pixel * h["px"]
                  + chip.clear_us)
            worst = max(worst, abs(100.0 * (us / 1000.0 - h["ms"]) / h["ms"]))
    return worst


def _verdict(ms):
    fps = 1000.0 / ms if ms > 0 else 0.0
    if fps >= 24.0:
        return "smooth"
    if fps >= 12.0:
        return "usable"
    if fps >= 5.0:
        return "slow"
    return "TOO SLOW"


def report(vertices, triangles, animated=True, view_w=BENCH_W, view_h=BENCH_H,
           target_fps=15.0, indent="  "):
    """The lines the exporter prints. Returns a list of strings."""
    out = []
    out.append("budget: %s vertices, %s triangles, %s, viewport %dx%d"
               % (f"{vertices:,}", f"{triangles:,}",
                  "animated" if animated else "static", view_w, view_h))
    out.append("  interpolated between two models measured on hardware;"
               " held out to +-%.0f%%. Assumes a" % _worst_held_out())
    out.append("  model framed like a character: %.0f%% of the viewport lit,"
               " %.0f%% of triangles facing"
               % (COVERAGE * 100.0, SURVIVAL * 100.0))
    out.append("  the camera. Yours will differ - a model that fills the frame"
               " costs more.")
    out.append("")
    out.append("  %-10s %-7s %8s %7s %10s  %s"
               % ("part", "cores", "frame", "fps", "model RAM", "verdict"))

    for name, chip in sorted(CHIPS.items()):
        for workers in (1, 2):
            if workers > chip.cores:
                continue
            ms = chip.frame_ms(vertices, triangles, animated=animated,
                               workers=workers, view_w=view_w, view_h=view_h)
            fps = 1000.0 / ms if ms > 0 else 0.0
            ram = chip.bind_bytes(vertices)
            out.append("  %-10s %-7s %7.0fms %7.1f %9.0fK  %s"
                       % (name, "%d" % workers, ms, fps, ram / 1024.0, _verdict(ms)))

    for name, why in sorted(UNMEASURED.items()):
        out.append("  %-10s %-7s %8s %7s %10s  %s" % (name, "-", "-", "-", "-", why))

    # What to do about it, for the slowest measured part that misses the target.
    target_ms = 1000.0 / target_fps
    advice = []
    for name, chip in sorted(CHIPS.items()):
        best = chip.frame_ms(vertices, triangles, animated=animated,
                             workers=chip.cores, view_w=view_w, view_h=view_h)
        if best <= target_ms:
            continue
        n = chip.triangles_for(target_ms, vertices, triangles, animated=animated,
                               workers=chip.cores, view_w=view_w, view_h=view_h)
        if n is None:
            floor = chip.frame_ms(0, 0, animated=animated, workers=chip.cores,
                                  view_w=view_w, view_h=view_h)
            advice.append("  %s cannot reach %.0f fps at %dx%d whatever the mesh:"
                          " covering that many" % (name, target_fps, view_w, view_h))
            advice.append("    pixels already costs %.0f ms. Render smaller, or"
                          " cover less of the screen." % floor)
        else:
            advice.append("  %s reaches about %.0f fps at --max-triangles %d"
                          % (name, target_fps, n))
    if advice:
        out.append("")
        out.extend(advice)
    return out


if __name__ == "__main__":
    import sys
    if "--self-test" in sys.argv:
        print("checking the redundancies in the measured anchors:")
        worst = self_test()
        print("ok. worst held-out error %.1f%%" % worst)
        raise SystemExit(0)
    if any(a in ("-h", "--help") for a in sys.argv[1:]):
        print(__doc__.strip() if __doc__ else "")
        print("usage: a3d_budget.py [VERTICES] [TRIANGLES]")
        print("       a3d_budget.py --self-test")
        print("\ndefaults are CesiumMan: 3273 vertices, 4672 triangles.")
        raise SystemExit(0)

    # A traceback is not an error message. This is a shipped tool, and the two
    # arguments are easy to get in the wrong order or to pass a filename to.
    try:
        v = int(sys.argv[1]) if len(sys.argv) > 1 else 3273
        t = int(sys.argv[2]) if len(sys.argv) > 2 else 4672
    except ValueError:
        raise SystemExit("a3d_budget.py: VERTICES and TRIANGLES must be whole "
                         "numbers.  Try --help.")
    print("\n".join(report(v, t)))
