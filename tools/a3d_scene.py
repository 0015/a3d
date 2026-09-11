# SPDX-FileCopyrightText: 2026 Eric Nam
# SPDX-License-Identifier: Apache-2.0

"""
Renderer-agnostic scene model, and the chunk emitters that turn it into an
a3d container.

Quantization happens here, at write time, using exactly the formulas documented
in `a3d/docs/ASSET_FORMAT.md`. Exporter and loader must agree bit for bit, so
those formulas exist in precisely two places: that document and this file.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

from a3d_container import (
    ChunkBuilder, ContainerWriter, StringTable, NONE8, NONE16, NONE32,
    CHUNK_STRT, CHUNK_NODE, CHUNK_SKEL, CHUNK_MESH, CHUNK_MSHC, CHUNK_SKIN,
    CHUNK_MATL, CHUNK_TEXR, CHUNK_ANIM,
    F_NODE, F_BONE, F_MESH, F_COOKED, F_SKINVTX, F_SKIN, F_MATERIAL,
    F_TEXTURE, F_CHANNEL, F_CLIP, MAX_BONES,
)

TEXEL_RGB565, TEXEL_RGB24, TEXEL_RGB32 = 0, 1, 2
TEXFLAG_POW2 = 1 << 0
ANIM_TRANSLATION, ANIM_ROTATION, ANIM_SCALE = 0, 1, 2
ANIM_STEP, ANIM_LINEAR = 0, 1
CLIP_LOOP = 1 << 0


# --------------------------------------------------------------------------
# Scene model
# -----------

@dataclass
class Material:
    name: str = ""
    color: tuple = (1.0, 1.0, 1.0)
    ambient: float = 0.2
    diffuse: float = 0.7
    specular: float = 0.5
    specular_exponent: int = 16
    texture_index: int = NONE16


@dataclass
class Texture:
    name: str = ""
    width: int = 0
    height: int = 0
    fmt: int = TEXEL_RGB565
    pixels: bytes = b""

    def flags(self) -> int:
        def pow2(n):
            return n > 0 and (n & (n - 1)) == 0
        return TEXFLAG_POW2 if (pow2(self.width) and pow2(self.height)) else 0


@dataclass
class Node:
    name: str = ""
    parent: int = NONE16
    flags: int = 0
    mesh_index: int = NONE16
    skin_index: int = NONE16
    translation: tuple = (0.0, 0.0, 0.0)
    rotation: tuple = (0.0, 0.0, 0.0, 1.0)      # xyzw
    scale: tuple = (1.0, 1.0, 1.0)


@dataclass
class Bone:
    name: str = ""
    node_index: int = 0
    parent: int = NONE8
    inv_bind: tuple = tuple([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])  # column-major


@dataclass
class Skeleton:
    bones: list = field(default_factory=list)


@dataclass
class Mesh:
    name: str = ""
    positions: list = field(default_factory=list)   # [(x,y,z), ...]
    normals: list | None = None                     # [(x,y,z), ...] unit length
    texcoords: list | None = None                   # [(u,v), ...]
    indices: list = field(default_factory=list)     # flat, 3 per triangle
    material_index: int = NONE16
    flags: int = 0


@dataclass
class SkinVertex:
    bone0: int = 0
    bone1: int = 0
    weight0: int = 255      # weight1 is implicitly 255 - weight0


@dataclass
class Skin:
    mesh_index: int = 0
    skeleton_index: int = 0
    bindings: list = field(default_factory=list)    # [SkinVertex] * mesh.vertex_count


@dataclass
class Channel:
    target_node: int = 0
    path: int = ANIM_ROTATION
    interp: int = ANIM_LINEAR
    times: list = field(default_factory=list)       # seconds, ascending
    values: list = field(default_factory=list)      # per key: 4-tuple (rot) or 3-tuple


@dataclass
class Clip:
    name: str = ""
    duration_ms: int = 0
    loop: bool = True
    channels: list = field(default_factory=list)


@dataclass
class CookedMesh:
    backend_id: int = 0
    mesh_index: int = NONE32
    blob: bytes = b""


@dataclass
class Scene:
    nodes: list = field(default_factory=list)
    skeletons: list = field(default_factory=list)
    meshes: list = field(default_factory=list)
    skins: list = field(default_factory=list)
    materials: list = field(default_factory=list)
    textures: list = field(default_factory=list)
    clips: list = field(default_factory=list)
    cooked: list = field(default_factory=list)


# --------------------------------------------------------------------------
# Quantization - must match docs/ASSET_FORMAT.md exactly
# ------------------------------------------------------

def _clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def _qi16(v: float) -> int:
    return int(_clamp(round(v), -32768, 32767))


def _qu16(v: float) -> int:
    return int(_clamp(round(v), 0, 65535))


def decompose_matrix(m):
    """
    Split a glTF column-major 4x4 node matrix into (translation, rotation, scale).

    glTF allows a node to state its transform either as TRS or as a matrix, and
    requires that matrix to be decomposable - no shear, no projection. The
    importer used to warn and substitute identity TRS, which silently moved
    every affected node to the origin with unit scale. The Fox has no matrix
    nodes and so could never show it; CesiumMan has two, and the model came out
    torn apart, because a bind pose that no longer matches the inverse bind
    matrices does not fail, it just deforms.

    Returns (t, r, s, residual) where residual is the largest element-wise
    difference between the input and the matrix rebuilt from the result - a
    number the caller can actually test, rather than a promise.
    """
    m = [float(v) for v in m]
    t = (m[12], m[13], m[14])

    cols = [(m[0], m[1], m[2]), (m[4], m[5], m[6]), (m[8], m[9], m[10])]
    s = [math.sqrt(sum(c * c for c in col)) for col in cols]

    # A negative determinant means the basis is mirrored. The mirror has to go
    # somewhere; glTF puts it in scale, and picking the x axis matches what
    # every other importer does, so assets round-trip the same way.
    cx, cy, cz = cols
    det = (cx[0] * (cy[1] * cz[2] - cy[2] * cz[1])
           - cy[0] * (cx[1] * cz[2] - cx[2] * cz[1])
           + cz[0] * (cx[1] * cy[2] - cx[2] * cy[1]))
    if det < 0.0:
        s[0] = -s[0]

    r = [[0.0] * 3 for _ in range(3)]
    for c in range(3):
        inv = 0.0 if s[c] == 0.0 else 1.0 / s[c]
        for row in range(3):
            r[row][c] = cols[c][row] * inv

    trace = r[0][0] + r[1][1] + r[2][2]
    if trace > 0.0:
        k = math.sqrt(trace + 1.0) * 2.0
        q = ((r[2][1] - r[1][2]) / k, (r[0][2] - r[2][0]) / k,
             (r[1][0] - r[0][1]) / k, 0.25 * k)
    elif (r[0][0] > r[1][1]) and (r[0][0] > r[2][2]):
        k = math.sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2.0
        q = (0.25 * k, (r[0][1] + r[1][0]) / k, (r[0][2] + r[2][0]) / k,
             (r[2][1] - r[1][2]) / k)
    elif r[1][1] > r[2][2]:
        k = math.sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2.0
        q = ((r[0][1] + r[1][0]) / k, 0.25 * k, (r[1][2] + r[2][1]) / k,
             (r[0][2] - r[2][0]) / k)
    else:
        k = math.sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2.0
        q = ((r[0][2] + r[2][0]) / k, (r[1][2] + r[2][1]) / k, 0.25 * k,
             (r[1][0] - r[0][1]) / k)
    n = math.sqrt(sum(v * v for v in q))
    q = (0.0, 0.0, 0.0, 1.0) if n == 0.0 else tuple(v / n for v in q)

    # Rebuild and measure, so a matrix that was NOT decomposable is reported
    # instead of quietly deforming the model.
    x, y, z, w = q
    rot = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
           [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
           [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    residual = 0.0
    for c in range(3):
        for row in range(3):
            residual = max(residual, abs(rot[row][c] * s[c] - m[c * 4 + row]))
    return t, q, tuple(s), residual


def generate_normals(positions, indices):
    """
    Area-weighted vertex normals from the geometry itself.

    glTF makes NORMAL optional and expects the renderer to fall back to flat
    face normals when it is missing - the Khronos Fox sample has no normals at
    all. Without this the runtime sees a mesh with no normals, downgrades to
    unlit, and the model renders with no lighting whatsoever. Computing them at
    import time is the only place the information is still available cheaply.

    Faces are assumed counter-clockwise, as glTF specifies, so `cross(b-a, c-a)`
    points outward. Face contributions are not normalized before accumulation, so
    large triangles carry proportionally more weight - the usual behaviour, and
    the one that looks right on uneven tessellation.

    A vertex touched by no triangle, or by degenerate ones only, gets +Y rather
    than a zero vector: a zero normal would make the shader produce black.
    """
    import math as _m
    acc = [[0.0, 0.0, 0.0] for _ in positions]
    for t in range(0, len(indices) - 2, 3):
        i0, i1, i2 = indices[t], indices[t + 1], indices[t + 2]
        if (i0 >= len(positions)) or (i1 >= len(positions)) or (i2 >= len(positions)):
            continue
        a, b, c = positions[i0], positions[i1], positions[i2]
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        fx = uy * vz - uz * vy
        fy = uz * vx - ux * vz
        fz = ux * vy - uy * vx
        for i in (i0, i1, i2):
            acc[i][0] += fx
            acc[i][1] += fy
            acc[i][2] += fz

    out = []
    for v in acc:
        L = _m.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
        out.append((v[0] / L, v[1] / L, v[2] / L) if L > 1e-12 else (0.0, 1.0, 0.0))
    return out


def mesh_bbox(positions):
    if not positions:
        return (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)
    xs = [p[0] for p in positions]
    ys = [p[1] for p in positions]
    zs = [p[2] for p in positions]
    return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


def mesh_quant_params(bb_min, bb_max):
    """center and uniform scale, per the format spec."""
    center = tuple(0.5 * (bb_min[i] + bb_max[i]) for i in range(3))
    extent = max(bb_max[i] - bb_min[i] for i in range(3))
    if extent <= 0.0:
        extent = 1.0
    return center, extent / 32767.0


# --------------------------------------------------------------------------
# Chunk emitters
# --------------

def emit_strt(strings: StringTable) -> ChunkBuilder:
    c = ChunkBuilder(CHUNK_STRT)
    c.raw(bytes(strings.buf))
    return c


def emit_node(scene: Scene, st: StringTable) -> ChunkBuilder:
    # Parents must precede children so world transforms need one forward pass.
    for i, n in enumerate(scene.nodes):
        if n.parent != NONE16 and n.parent >= i:
            raise ValueError(
                f"node {i} ({n.name!r}) has parent {n.parent} at or after it; "
                "parents must be ordered before children")
    c = ChunkBuilder(CHUNK_NODE)
    c.pack("<I", len(scene.nodes))
    for n in scene.nodes:
        c.pack(F_NODE, st.add(n.name), n.parent, n.flags, n.mesh_index, n.skin_index,
               *n.translation, *n.rotation, *n.scale)
    return c


def emit_skel(scene: Scene, st: StringTable) -> ChunkBuilder:
    c = ChunkBuilder(CHUNK_SKEL)
    if len(scene.skeletons) > 1:
        raise ValueError("v0.1 supports at most one skeleton chunk")
    bones = scene.skeletons[0].bones if scene.skeletons else []
    if len(bones) > MAX_BONES:
        raise ValueError(f"{len(bones)} bones exceeds the {MAX_BONES} cap (uint8 indices)")
    c.pack("<II", len(bones), 0)
    for b in bones:
        c.pack(F_BONE, st.add(b.name), b.node_index, b.parent, 0, *b.inv_bind)
    return c


def emit_mesh(scene: Scene, st: StringTable) -> ChunkBuilder:
    c = ChunkBuilder(CHUNK_MESH)
    c.pack("<I", len(scene.meshes))

    entry_refs = []
    for m in scene.meshes:
        nv = len(m.positions)
        nt = len(m.indices) // 3
        bb_min, bb_max = mesh_bbox(m.positions)
        name_off = st.add(m.name)
        c.pack("<III", name_off, nv, nt)
        r_pos = c.reserve_ref()
        r_nrm = c.reserve_ref()
        r_uv = c.reserve_ref()
        r_idx = c.reserve_ref()
        c.pack("<HH", m.material_index, m.flags)
        c.pack("<3f3f", *bb_min, *bb_max)
        entry_refs.append((m, bb_min, bb_max, r_pos, r_nrm, r_uv, r_idx))

    for (m, bb_min, bb_max, r_pos, r_nrm, r_uv, r_idx) in entry_refs:
        center, pscale = mesh_quant_params(bb_min, bb_max)
        inv = 1.0 / pscale

        qp = bytearray()
        for p in m.positions:
            for k in range(3):
                qp += struct.pack("<h", _qi16((p[k] - center[k]) * inv))
        c.set_ref(r_pos, c.blob(bytes(qp)))

        if m.normals:
            qn = bytearray()
            for n in m.normals:
                for k in range(3):
                    qn += struct.pack("<h", _qi16(n[k] * 32767.0))
            c.set_ref(r_nrm, c.blob(bytes(qn)))
        else:
            c.set_none(r_nrm)

        if m.texcoords:
            qt = bytearray()
            for t in m.texcoords:
                for k in range(2):
                    qt += struct.pack("<h", _qi16(t[k] * (32767.0 / 4.0)))
            c.set_ref(r_uv, c.blob(bytes(qt)))
        else:
            c.set_none(r_uv)

        c.set_ref(r_idx, c.blob(struct.pack(f"<{len(m.indices)}H", *m.indices)))

    return c


def emit_mshc(scene: Scene) -> ChunkBuilder:
    c = ChunkBuilder(CHUNK_MSHC)
    c.pack("<I", len(scene.cooked))
    refs = []
    for ck in scene.cooked:
        c.pack("<II", ck.backend_id, ck.mesh_index)
        r_blob = c.reserve_ref()
        c.pack("<I", len(ck.blob))
        refs.append((ck, r_blob))
    for ck, r_blob in refs:
        c.set_ref(r_blob, c.blob(ck.blob))
    return c


def emit_skin(scene: Scene) -> ChunkBuilder:
    c = ChunkBuilder(CHUNK_SKIN)
    c.pack("<I", len(scene.skins))
    refs = []
    for s in scene.skins:
        c.pack("<II", s.mesh_index, s.skeleton_index)
        r = c.reserve_ref()
        c.pack("<I", 0)
        refs.append((s, r))
    for s, r in refs:
        payload = bytearray()
        for sv in s.bindings:
            payload += struct.pack(F_SKINVTX, sv.bone0, sv.bone1, sv.weight0, 0)
        c.set_ref(r, c.blob(bytes(payload)))
    return c


def emit_matl(scene: Scene, st: StringTable) -> ChunkBuilder:
    c = ChunkBuilder(CHUNK_MATL)
    c.pack("<I", len(scene.materials))
    for m in scene.materials:
        c.pack(F_MATERIAL, st.add(m.name), *m.color,
               m.ambient, m.diffuse, m.specular,
               m.specular_exponent, m.texture_index)
    return c


def emit_texr(scene: Scene, st: StringTable) -> ChunkBuilder:
    c = ChunkBuilder(CHUNK_TEXR)
    c.pack("<I", len(scene.textures))
    refs = []
    for t in scene.textures:
        c.pack("<I", st.add(t.name))
        c.pack("<HHHH", t.width, t.height, t.fmt, t.flags())
        r = c.reserve_ref()
        c.pack("<I", len(t.pixels))
        refs.append((t, r))
    for t, r in refs:
        c.set_ref(r, c.blob(t.pixels))
    return c


def emit_anim(scene: Scene, st: StringTable) -> ChunkBuilder:
    c = ChunkBuilder(CHUNK_ANIM)
    c.pack("<I", len(scene.clips))
    clip_refs = []
    for cl in scene.clips:
        c.pack("<III", st.add(cl.name), cl.duration_ms, len(cl.channels))
        r = c.reserve_ref()
        c.pack("<HHI", CLIP_LOOP if cl.loop else 0, 0, 0)
        clip_refs.append((cl, r))

    for cl, r_channels in clip_refs:
        c.pad_to_align(4)
        channels_at = len(c.buf)
        c.set_ref(r_channels, channels_at)

        per_channel = []
        for ch in cl.channels:
            n = len(ch.times)
            if ch.path == ANIM_ROTATION:
                vmin = vmax = (0.0, 0.0, 0.0)
            else:
                comps = list(zip(*ch.values)) if ch.values else [(0.0,)] * 3
                vmin = tuple(min(comps[k]) for k in range(3))
                vmax = tuple(max(comps[k]) for k in range(3))
            c.pack("<HBBHH", ch.target_node, ch.path, ch.interp, n, 0)
            r_times = c.reserve_ref()
            r_values = c.reserve_ref()
            c.pack("<3f3f", *vmin, *vmax)
            c.pack("<II", 0, 0)
            per_channel.append((ch, vmin, vmax, r_times, r_values))

        dur = max(cl.duration_ms, 1) / 1000.0
        for (ch, vmin, vmax, r_times, r_values) in per_channel:
            times = bytearray()
            for t in ch.times:
                times += struct.pack("<H", _qu16(t / dur * 65535.0))
            c.set_ref(r_times, c.blob(bytes(times)))

            vals = bytearray()
            if ch.path == ANIM_ROTATION:
                for q in ch.values:
                    for k in range(4):
                        vals += struct.pack("<h", _qi16(q[k] * 32767.0))
            else:
                span = [(vmax[k] - vmin[k]) for k in range(3)]
                for v in ch.values:
                    for k in range(3):
                        if span[k] <= 0.0:
                            vals += struct.pack("<H", 0)
                        else:
                            vals += struct.pack(
                                "<H", _qu16((v[k] - vmin[k]) / span[k] * 65535.0))
            c.set_ref(r_values, c.blob(bytes(vals)))

    return c


def build_container(scene: Scene) -> bytes:
    """Emit the whole scene. Empty chunks are omitted."""
    st = StringTable()

    # Names must be interned before STRT is emitted, so build the chunks that
    # reference strings first and append STRT last.
    later = []
    if scene.nodes:     later.append(emit_node(scene, st))
    if scene.skeletons: later.append(emit_skel(scene, st))
    if scene.meshes:    later.append(emit_mesh(scene, st))
    if scene.cooked:    later.append(emit_mshc(scene))
    if scene.skins:     later.append(emit_skin(scene))
    if scene.materials: later.append(emit_matl(scene, st))
    if scene.textures:  later.append(emit_texr(scene, st))
    if scene.clips:     later.append(emit_anim(scene, st))

    w = ContainerWriter()
    w.add(emit_strt(st))
    for c in later:
        w.add(c)
    return w.build()
