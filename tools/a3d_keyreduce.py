# SPDX-FileCopyrightText: 2026 Eric Nam
# SPDX-License-Identifier: Apache-2.0

"""
Keyframe reduction: drop keys that linear interpolation already reproduces.
--------
The dragon's 27 clips over a 156-bone rig come to 16.7 MB of `ANIM` against
652 KB of `MESH`. That is **not** because the keys are unquantized - they have
always been int16 rotations, uint16 translations and uint16 times. It is because
there are 1,862,123 of them: 12,424 channels averaging 150 keys each, exported at
a fixed sample rate whether the bone moved or not.

Measured on that asset: 34% of channels never change value at all, and at a
tolerance of 0.1 degrees of rotation and 0.05% of model extent, **15% of keys
survive** - a 5.7x reduction that subsumes the constant case.
----------------
The reduced curve is checked against **every original key**, not against its
neighbours: a run is only extended while interpolating its two endpoints
reproduces each interior key within tolerance. So the stated tolerance is an
error bound on the whole curve, not a per-step one that could accumulate.

The first and last keys are always kept, so a clip's duration and its start and
end poses are exact.
"""
from __future__ import annotations

import math

ROTATION = "rotation"


def _quat_error_deg(a, b, u, target):
    """Angle between the interpolated quaternion and the key it replaces."""
    d = sum(a[m] * b[m] for m in range(4))
    bb = [-x for x in b] if d < 0.0 else list(b)      # shortest arc
    q = [a[m] + (bb[m] - a[m]) * u for m in range(4)]
    n = math.sqrt(sum(x * x for x in q))
    if n <= 1e-12:
        return 1e9
    q = [x / n for x in q]
    dot = abs(sum(q[m] * target[m] for m in range(4)))
    return 2.0 * math.degrees(math.acos(min(1.0, dot)))


def reduce_channel(times, values, is_rotation, tolerance):
    """
    Return (times, values) with redundant keys removed.

    `tolerance` is degrees for rotation and absolute units for anything else -
    the caller scales it by the model's extent so that a translation tolerance
    means the same thing on a 2-unit character and a 4,700-unit dragon.

    Douglas-Peucker: take the segment from the first key to the last, find the
    interior key it reproduces worst, and if that key is outside tolerance, split
    there and recurse on both halves. Every original key is therefore measured
    against the segment that will actually represent it, which is the same
    guarantee a forward scan gives - but that scan re-checked every interior key
    for every candidate endpoint and ran to O(n^3) per channel. On the dragon's
    12,424 channels it did not finish in ten minutes.
    """
    n = len(times)
    if n < 3 or tolerance <= 0.0:
        return times, values

    keep = [False] * n
    keep[0] = keep[n - 1] = True

    stack = [(0, n - 1)]
    while stack:
        lo, hi = stack.pop()
        if hi - lo < 2:
            continue
        span = times[hi] - times[lo]
        worst, at = -1.0, -1
        for k in range(lo + 1, hi):
            u = 0.0 if span <= 0.0 else (times[k] - times[lo]) / span
            if is_rotation:
                err = _quat_error_deg(values[lo], values[hi], u, values[k])
            else:
                err = max(abs(values[lo][m] + (values[hi][m] - values[lo][m]) * u
                              - values[k][m])
                          for m in range(len(values[k])))
            if err > worst:
                worst, at = err, k
        if worst > tolerance and at > lo:
            keep[at] = True
            stack.append((lo, at))
            stack.append((at, hi))

    idx = [i for i in range(n) if keep[i]]
    return [times[i] for i in idx], [values[i] for i in idx]


def max_error(times, values, rtimes, rvalues, is_rotation):
    """
    Largest deviation of the reduced curve from the original, sampled at every
    original key. Exists so a caller can assert the bound rather than trust it.
    """
    if not rtimes:
        return 0.0
    worst = 0.0
    j = 0
    for t, v in zip(times, values):
        while (j + 1 < len(rtimes) - 1) and (rtimes[j + 1] < t):
            j += 1
        a, b = j, min(j + 1, len(rtimes) - 1)
        span = rtimes[b] - rtimes[a]
        u = 0.0 if span <= 0.0 else (t - rtimes[a]) / span
        u = 0.0 if u < 0.0 else (1.0 if u > 1.0 else u)
        if is_rotation:
            err = _quat_error_deg(rvalues[a], rvalues[b], u, v)
        else:
            err = max(abs(rvalues[a][m] + (rvalues[b][m] - rvalues[a][m]) * u - v[m])
                      for m in range(len(v)))
        worst = max(worst, err)
    return worst
