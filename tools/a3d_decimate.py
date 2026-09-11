# SPDX-FileCopyrightText: 2026 Eric Nam
# SPDX-License-Identifier: Apache-2.0

"""
Mesh decimation by quadric-error half-edge collapse.

Why this exists
---------------
BrainStem paints 10% of a 720x720 panel - 51,840 pixels - with 61,666 triangles:
0.84 pixels per triangle. Measured frame time on an ESP32-P4 is
`3.68 us x triangles + 27 ms`, so that model costs 244 ms whatever the renderer
does with it. Tile size, vertex residency and parallel binning each moved the
frame by single-digit percentages because the cost is proportional to a triangle
count an order of magnitude larger than the screen can show.

What it does NOT do
-------------------
**No attribute interpolation.** A collapse moves one vertex onto another and the
survivor keeps its own normal, uv and skin binding unchanged. Bone indices cannot
be averaged - the mean of joint 7 and joint 22 is not a joint - and blending
weights across different bone pairs silently deforms the mesh. Interpolating uvs
across a seam swims the texture.

That is bought by locking two kinds of vertex:

* **Seams.** Vertices sharing a position but not their attributes - a uv seam, a
  hard normal edge - are locked. Collapsing one side would leave the other
  behind and tear the surface.
* **Boundaries.** An edge with one adjacent triangle is a border. Meshes are
  decimated one at a time, so a part's border is where it meets its neighbours;
  moving it opens a crack between parts that no amount of triangle budget hides.

Locking costs reduction: a mesh that is mostly seam cannot shrink much, and the
caller is told what it actually got rather than what it asked for.
"""
from __future__ import annotations

import heapq
import math


def _face_quadric(p0, p1, p2):
    """Plane (a,b,c,d) of a triangle as the 10 upper-triangle terms of K = pp^T."""
    ux, uy, uz = p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
    vx, vy, vz = p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]
    a = uy * vz - uz * vy
    b = uz * vx - ux * vz
    c = ux * vy - uy * vx
    n = math.sqrt(a * a + b * b + c * c)
    if n <= 1e-20:
        return None, 0.0
    # Area weighting: a big triangle's plane should matter more than a sliver's.
    area = 0.5 * n
    a, b, c = a / n, b / n, c / n
    d = -(a * p0[0] + b * p0[1] + c * p0[2])
    w = area
    return (a * a * w, a * b * w, a * c * w, a * d * w,
            b * b * w, b * c * w, b * d * w,
            c * c * w, c * d * w,
            d * d * w), area


def _quadric_add(q, r):
    return tuple(x + y for x, y in zip(q, r))


def _quadric_error(q, p):
    x, y, z = p
    (q0, q1, q2, q3, q4, q5, q6, q7, q8, q9) = q
    return (q0 * x * x + 2 * q1 * x * y + 2 * q2 * x * z + 2 * q3 * x
            + q4 * y * y + 2 * q5 * y * z + 2 * q6 * y
            + q7 * z * z + 2 * q8 * z
            + q9)


def decimate(positions, normals, texcoords, indices, target_triangles,
             extra_per_vertex=(), warn=None, uv_span=3.0):
    """
    Reduce `indices` towards `target_triangles` by collapsing interior vertices.

    `extra_per_vertex` is any number of parallel per-vertex sequences (skin
    joints, skin weights) that must survive the same remapping. They are indexed,
    never blended.

    `uv_span` refuses a collapse that would move a vertex more than that many
    MEDIAN uv edge lengths away in uv space. Geometry error alone does not see
    texture: a face is a smooth surface, so quadric error happily collapses
    across it and drags the sunglasses over the cheek.

    Relative, not absolute: uv spacing is a property of how the mesh was
    unwrapped, and a fixed threshold either blocks every collapse on a coarsely
    unwrapped mesh or permits every one on a dense face. The first version used
    0.03 uv units and stopped a flat test grid from decimating at all.

    Returns (positions, normals, texcoords, indices, extras, stats).
    Vertices that no triangle references are dropped and everything is compacted.
    """
    nv = len(positions)
    nt = len(indices) // 3
    stats = {"triangles_in": nt, "vertices_in": nv, "locked": 0,
             "triangles_out": nt, "vertices_out": nv, "collapses": 0,
             "uv_refused": 0}
    if target_triangles >= nt or nt < 4 or nv < 4:
        return positions, normals, texcoords, indices, list(extra_per_vertex), stats

    # --- weld by exact position -------------------------------------------
    # Exact, not tolerant: glTF exporters split vertices at seams by duplicating
    # the position bit for bit, so equality finds precisely those duplicates and
    # invents no others.
    group_of = [0] * nv
    group_pos = []
    seen = {}
    for i, p in enumerate(positions):
        key = (p[0], p[1], p[2])
        g = seen.get(key)
        if g is None:
            g = len(group_pos)
            seen[key] = g
            group_pos.append(key)
        group_of[i] = g
    ng = len(group_pos)

    # A group holding more than one vertex is a seam: its members disagree about
    # normal or uv, and only one of them can survive a collapse.
    members = [0] * ng
    for g in group_of:
        members[g] += 1
    locked = [m > 1 for m in members]

    # --- faces and adjacency in group space --------------------------------
    faces = []
    for t in range(nt):
        a = group_of[indices[t * 3 + 0]]
        b = group_of[indices[t * 3 + 1]]
        c = group_of[indices[t * 3 + 2]]
        if a == b or b == c or a == c:
            continue                     # already degenerate in the source
        faces.append([a, b, c])
    nf = len(faces)

    vfaces = [[] for _ in range(ng)]
    for f, (a, b, c) in enumerate(faces):
        vfaces[a].append(f)
        vfaces[b].append(f)
        vfaces[c].append(f)

    edge_faces = {}
    for f, (a, b, c) in enumerate(faces):
        for u, v in ((a, b), (b, c), (c, a)):
            edge_faces.setdefault((u, v) if u < v else (v, u), []).append(f)
    for (u, v), fs in edge_faces.items():
        if len(fs) != 2:                 # border, or non-manifold
            locked[u] = True
            locked[v] = True
    stats["locked"] = sum(1 for x in locked if x)

    uv_of = None
    uv_limit = 0.0
    if texcoords and uv_span > 0.0:
        uv_of = [None] * ng
        for i in range(nv):
            g = group_of[i]
            if uv_of[g] is None:
                uv_of[g] = texcoords[i]
        lens = []
        for (u, v) in edge_faces:
            a_uv, b_uv = uv_of[u], uv_of[v]
            if (a_uv is not None) and (b_uv is not None):
                lens.append(math.hypot(a_uv[0] - b_uv[0], a_uv[1] - b_uv[1]))
        lens.sort()
        median = lens[len(lens) // 2] if lens else 0.0
        uv_limit = median * uv_span
        if uv_limit <= 0.0:
            uv_of = None

    # --- quadrics -----------------------------------------------------------
    Q = [(0.0,) * 10 for _ in range(ng)]
    for a, b, c in faces:
        q, _ = _face_quadric(group_pos[a], group_pos[b], group_pos[c])
        if q is None:
            continue
        Q[a] = _quadric_add(Q[a], q)
        Q[b] = _quadric_add(Q[b], q)
        Q[c] = _quadric_add(Q[c], q)

    alive = [True] * ng
    version = [0] * ng
    collapsed_to = list(range(ng))

    heap = []
    for (u, v) in edge_faces:
        for a, b in ((u, v), (v, u)):
            if locked[a]:
                continue
            # Cost of moving `a` onto `b`, measured against the planes that met
            # at `a`. The survivor is `b`, so `b` keeps every attribute it had.
            heapq.heappush(heap, (_quadric_error(Q[a], group_pos[b]),
                                  a, b, version[a]))

    # One attribute vertex per unlocked group (a group with more than one is a
    # seam and is locked), so a group's uv is well defined.
    def _uv_far(a, b):
        if uv_of is None:
            return False
        ua, ub = uv_of[a], uv_of[b]
        if (ua is None) or (ub is None):
            return False
        return ((ua[0] - ub[0]) ** 2 + (ua[1] - ub[1]) ** 2) > (uv_limit * uv_limit)

    def _resolve(g):
        while collapsed_to[g] != g:
            g = collapsed_to[g]
        return g

    def _would_flip(a, b):
        """Does replacing `a` with `b` turn any of a's triangles inside out?"""
        for f in vfaces[a]:
            tri = faces[f]
            if tri is None:
                continue
            i0, i1, i2 = (_resolve(tri[0]), _resolve(tri[1]), _resolve(tri[2]))
            if b in (i0, i1, i2):
                continue                 # this face disappears in the collapse
            before, area0 = _face_quadric(group_pos[i0], group_pos[i1], group_pos[i2])
            n0 = _normal(group_pos[i0], group_pos[i1], group_pos[i2])
            j = [b if x == a else x for x in (i0, i1, i2)]
            n1 = _normal(group_pos[j[0]], group_pos[j[1]], group_pos[j[2]])
            if n0 is None or n1 is None:
                return True
            if n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] < 0.2:
                return True
        return False

    def _normal(p0, p1, p2):
        ux, uy, uz = p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
        vx, vy, vz = p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]
        a = uy * vz - uz * vy
        b = uz * vx - ux * vz
        c = ux * vy - uy * vx
        n = math.sqrt(a * a + b * b + c * c)
        return None if n <= 1e-20 else (a / n, b / n, c / n)

    live_faces = nf
    while heap and live_faces > target_triangles:
        cost, a, b, ver = heapq.heappop(heap)
        if not alive[a] or ver != version[a]:
            continue                     # stale entry
        b = _resolve(b)
        if b == a or not alive[b] or locked[a]:
            continue
        if _uv_far(a, b):
            stats["uv_refused"] += 1
            continue
        if _would_flip(a, b):
            continue

        collapsed_to[a] = b
        alive[a] = False
        Q[b] = _quadric_add(Q[b], Q[a])
        version[b] += 1
        stats["collapses"] += 1

        moved = vfaces[a]
        vfaces[b] = vfaces[b] + moved
        for f in moved:
            tri = faces[f]
            if tri is None:
                continue
            i0, i1, i2 = (_resolve(tri[0]), _resolve(tri[1]), _resolve(tri[2]))
            if i0 == i1 or i1 == i2 or i0 == i2:
                faces[f] = None
                live_faces -= 1

        for f in vfaces[b]:
            tri = faces[f]
            if tri is None:
                continue
            for g in (_resolve(tri[0]), _resolve(tri[1]), _resolve(tri[2])):
                if g != b and alive[g] and not locked[g]:
                    heapq.heappush(heap, (_quadric_error(Q[g], group_pos[b]),
                                          g, b, version[g]))

    # --- rebuild ------------------------------------------------------------
    # One surviving vertex per surviving group. A seam group keeps every member
    # it had, because it was never collapsed.
    rep = {}
    for i in range(nv):
        g = _resolve(group_of[i])
        rep.setdefault(g, {})[group_of[i]] = i
    vert_for = {}
    for i in range(nv):
        g = _resolve(group_of[i])
        if group_of[i] == g:
            vert_for.setdefault(g, i)
    for g, m in rep.items():
        if g not in vert_for:
            vert_for[g] = next(iter(m.values()))

    out_idx = []
    for t in range(nt):
        src = [indices[t * 3 + k] for k in range(3)]
        dst = []
        for s in src:
            g = _resolve(group_of[s])
            dst.append(s if group_of[s] == g else vert_for[g])
        if dst[0] == dst[1] or dst[1] == dst[2] or dst[0] == dst[2]:
            continue
        out_idx.extend(dst)

    used = sorted(set(out_idx))
    remap = {old: new for new, old in enumerate(used)}
    out_pos = [positions[i] for i in used]
    out_nrm = [normals[i] for i in used] if normals else None
    out_uv = [texcoords[i] for i in used] if texcoords else None
    out_extra = [[arr[i] for i in used] for arr in extra_per_vertex]
    out_idx = [remap[i] for i in out_idx]

    stats["triangles_out"] = len(out_idx) // 3
    stats["vertices_out"] = len(out_pos)
    if warn and stats["triangles_out"] > target_triangles * 1.15:
        warn(f"decimation stopped at {stats['triangles_out']} triangles, not "
             f"{target_triangles}: {stats['locked']} of {ng} vertex groups are "
             "seams or borders and cannot move")
    return out_pos, out_nrm, out_uv, out_idx, out_extra, stats
