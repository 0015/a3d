# SPDX-FileCopyrightText: 2026 Eric Nam
# SPDX-License-Identifier: Apache-2.0

"""
glTF 2.0 / GLB importer -> a3d scene model.

Dependency-free for geometry and animation; Pillow is used only to decode
texture images, and its absence degrades to "no textures" with a warning rather
than a failure.

Two conversions deserve attention because they lose information on purpose:

  * **Skinning is reduced to two influences per vertex.** glTF exports four.
    The two largest weights are kept and renormalized, which is the decision
    recorded in the plan: it halves both the skinning cost and the per-vertex
    data at a quality difference visible only at joints.

  * **Node order is rewritten so parents always precede children**, which the
    format requires so world transforms need one forward pass and no stack.
    Indices in skins and animation channels are remapped accordingly.
"""
from __future__ import annotations

import base64
import json
import struct
import sys
from pathlib import Path

import a3d_container as C
import a3d_decimate as D
import a3d_keyreduce as K
import a3d_scene as S

# glTF component types -> struct char and byte size
_COMPONENT = {
    5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
    5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4),
}
_NUM_COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
                   "MAT2": 4, "MAT3": 9, "MAT4": 16}


class GltfError(RuntimeError):
    pass


def _load_glb(data: bytes):
    magic, version, _length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        raise GltfError("not a GLB file")
    if version != 2:
        raise GltfError(f"unsupported GLB version {version}")
    off = 12
    js, bin_chunk = None, None
    while off + 8 <= len(data):
        clen, ctype = struct.unpack_from("<II", data, off)
        body = data[off + 8: off + 8 + clen]
        if ctype == 0x4E4F534A:
            js = json.loads(body.decode("utf-8"))
        elif ctype == 0x004E4942:
            bin_chunk = body
        off += 8 + clen + ((-clen) % 4)
    if js is None:
        raise GltfError("GLB has no JSON chunk")
    return js, bin_chunk


class Gltf:
    def __init__(self, path: Path):
        self.path = Path(path)
        raw = self.path.read_bytes()
        if raw[:4] == b"glTF":
            self.js, self._glb_bin = _load_glb(raw)
        else:
            self.js = json.loads(raw.decode("utf-8"))
            self._glb_bin = None
        self._buffers: list[bytes | None] = [None] * len(self.js.get("buffers", []))

    # -- raw data access ---------------------------------------------------

    def buffer(self, i: int) -> bytes:
        if self._buffers[i] is not None:
            return self._buffers[i]
        b = self.js["buffers"][i]
        uri = b.get("uri")
        if uri is None:
            if self._glb_bin is None:
                raise GltfError("buffer has no uri and no GLB binary chunk")
            data = self._glb_bin
        elif uri.startswith("data:"):
            data = base64.b64decode(uri.split(",", 1)[1])
        else:
            data = (self.path.parent / uri).read_bytes()
        self._buffers[i] = data
        return data

    def buffer_view_bytes(self, i: int) -> bytes:
        bv = self.js["bufferViews"][i]
        data = self.buffer(bv.get("buffer", 0))
        off = bv.get("byteOffset", 0)
        return data[off: off + bv["byteLength"]]

    def accessor(self, i: int) -> list:
        """Return accessor data as a list of tuples (or scalars for SCALAR)."""
        a = self.js["accessors"][i]
        n = a["count"]
        ncomp = _NUM_COMPONENTS[a["type"]]
        ch, csize = _COMPONENT[a["componentType"]]
        elem = ncomp * csize

        if "bufferView" not in a:
            zero = 0.0 if ch == "f" else 0
            return [(zero,) * ncomp if ncomp > 1 else zero for _ in range(n)]

        bv = self.js["bufferViews"][a["bufferView"]]
        data = self.buffer(bv.get("buffer", 0))
        base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        stride = bv.get("byteStride", elem) or elem

        out = []
        fmt = "<" + ch * ncomp
        for k in range(n):
            vals = struct.unpack_from(fmt, data, base + k * stride)
            out.append(vals[0] if ncomp == 1 else vals)
        # Normalized integer attributes (weights, joints) per the glTF spec.
        if a.get("normalized") and ch in ("b", "B", "h", "H"):
            denom = {"b": 127.0, "B": 255.0, "h": 32767.0, "H": 65535.0}[ch]
            if ncomp == 1:
                out = [max(v / denom, -1.0) for v in out]
            else:
                out = [tuple(max(v / denom, -1.0) for v in t) for t in out]
        return out


# --------------------------------------------------------------------------
# Conversion
# ----------

def _topo_order_nodes(js) -> tuple[list[int], list[int]]:
    """
    Order nodes so every parent precedes its children, as the format requires.
    Returns (order, remap) where remap[old_index] = new_index.
    """
    nodes = js.get("nodes", [])
    parent = [C.NONE16] * len(nodes)
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = i

    order, emitted = [], [False] * len(nodes)

    def emit(i):
        if emitted[i]:
            return
        p = parent[i]
        if p != C.NONE16 and not emitted[p]:
            emit(p)
        emitted[i] = True
        order.append(i)

    for i in range(len(nodes)):
        emit(i)

    remap = [0] * len(nodes)
    for new_i, old_i in enumerate(order):
        remap[old_i] = new_i
    return order, remap, parent


def _rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def _decode_image(g: Gltf, image_index: int, max_size: int):
    try:
        from PIL import Image
    except ImportError:
        return None, "Pillow not installed"
    import io as _io

    img_js = g.js["images"][image_index]
    if "bufferView" in img_js:
        raw = g.buffer_view_bytes(img_js["bufferView"])
    elif "uri" in img_js:
        uri = img_js["uri"]
        raw = (base64.b64decode(uri.split(",", 1)[1]) if uri.startswith("data:")
               else (g.path.parent / uri).read_bytes())
    else:
        return None, "image has neither bufferView nor uri"

    im = Image.open(_io.BytesIO(raw)).convert("RGB")
    if max(im.size) > max_size:
        scale = max_size / max(im.size)
        im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))))

    # The fast wrap path needs power-of-two dimensions; snap down to the
    # nearest one so the flag can be set and the slower clamp path avoided.
    def floor_pow2(n):
        p = 1
        while p * 2 <= n:
            p *= 2
        return p
    w, h = floor_pow2(im.width), floor_pow2(im.height)
    if (w, h) != im.size:
        im = im.resize((w, h))

    px = bytearray()
    for (r, gg, b) in im.getdata():
        px += struct.pack("<H", _rgb565(r, gg, b))
    return S.Texture(name=img_js.get("name", f"image{image_index}"),
                     width=w, height=h, fmt=S.TEXEL_RGB565,
                     pixels=bytes(px)), None


DRACO = "KHR_draco_mesh_compression"


def non_opaque_materials(js) -> dict:
    """
    Material index -> (name, alphaMode), for every material a3d cannot honour.

    a3d's rasterizer writes every covered pixel opaquely: no alpha blend, no
    alpha test. This is a module-level function rather than a few lines inside
    convert() because the browser front end has to warn about the same thing
    and must not carry a second opinion about what counts.
    """
    out = {}
    for i, m in enumerate(js.get("materials", [])):
        mode = m.get("alphaMode", "OPAQUE")
        if mode != "OPAQUE":
            out[i] = (m.get("name", ""), mode)
    return out


def _refuse_compressed_geometry(js) -> None:
    """
    Refuse a file whose geometry this importer cannot read, LOUDLY.

    A Draco primitive keeps its accessors - with the right `count`, `min` and
    `max` - but moves the data into a compressed bufferView and drops
    `bufferView` from every accessor. The zero-fill in `Gltf.accessor` then
    hands back the right NUMBER of vertices, all of them at the origin, and the
    conversion runs to completion: correct triangle and vertex counts, correct
    materials, real textures, and a model collapsed to a point. Nothing
    downstream can tell that apart from a legitimately small mesh, so it has to
    be caught here.

    """
    bad = []
    for mi, m in enumerate(js.get("meshes", [])):
        for pi, prim in enumerate(m.get("primitives", [])):
            if DRACO not in prim.get("extensions", {}):
                continue
            pos = prim.get("attributes", {}).get("POSITION")
            if pos is None:
                continue
            if "bufferView" not in js["accessors"][pos]:
                bad.append(f"mesh {mi} primitive {pi}")
    if bad:
        raise GltfError(
            f"{DRACO}: {len(bad)} primitive(s) store their geometry compressed "
            f"({', '.join(bad[:3])}{', ...' if len(bad) > 3 else ''}) and this "
            "importer has no Draco decoder. Decompress first, e.g.\n"
            "    npx @gltf-transform/cli copy in.glb out.glb\n"
            "and import the result. Importing as-is would produce a file with "
            "the right vertex count and every vertex at the origin.")


def convert(path: Path, *, max_texture: int = 256, verbose: bool = True, clips=None,
            max_triangles=0, anim_tolerance=1.0, drop_blend: bool = False) -> S.Scene:
    g = Gltf(path)
    js = g.js
    _refuse_compressed_geometry(js)
    sc = S.Scene()
    warn = (lambda m: print(f"  warning: {m}", file=sys.stderr)) if verbose else (lambda m: None)

    # --- textures ---------------------------------------------------------
    image_to_texture: dict[int, int] = {}
    for i in range(len(js.get("images", []))):
        tex, err = _decode_image(g, i, max_texture)
        if tex is None:
            warn(f"image {i} skipped: {err}")
            continue
        image_to_texture[i] = len(sc.textures)
        sc.textures.append(tex)

    SPEC_GLOSS = "KHR_materials_pbrSpecularGlossiness"

    def base_colour_of(material_js):
        """
        The material's diffuse colour, texture reference and roughness.

        glTF 2.0's core material is pbrMetallicRoughness, but a great many real
        assets - anything exported through the older Sketchfab and game-engine
        pipelines - carry their diffuse in KHR_materials_pbrSpecularGlossiness
        instead, with **no pbrMetallicRoughness block at all**. Reading only the
        core block leaves such a model textureless and flat white, drawn without
        complaint. The dragon fixture is exactly this shape.

        A third shape shows up in asset sets built for unlit rendering: the base
        colour is factor-only and BLACK, and the artwork hangs off
        `emissiveTexture` with an `emissiveFactor` of white. This renderer has no
        emissive term, so reading only the base colour draws such a material as
        pure black - which is not a missing texture that anyone would notice as
        one, it is a model that renders as a silhouette. Falling back to the
        emissive map is an approximation, but it is the one that shows what the
        asset is of.
        (Pokemon #795 is exactly this and drew black; #605 and #726 have one such
        material each among four normal ones.)

        """
        pbr = material_js.get("pbrMetallicRoughness")
        if pbr is not None and (("baseColorTexture" in pbr) or
                                ("baseColorFactor" in pbr) or
                                ("roughnessFactor" in pbr)):
            base = pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0])
            tex = pbr.get("baseColorTexture")
            rough = float(pbr.get("roughnessFactor", 1.0))
            if tex is None and "emissiveTexture" in material_js:
                # Emissive carries the artwork; its factor carries the tint.
                em = material_js.get("emissiveFactor", [1.0, 1.0, 1.0])
                return ([em[0], em[1], em[2], base[3] if len(base) > 3 else 1.0],
                        material_js["emissiveTexture"], rough)
            return (base, tex, rough)

        sg = material_js.get("extensions", {}).get(SPEC_GLOSS)
        if sg is not None:
            # Glossiness is the inverse of roughness, which is what the Phong
            # exponent below is derived from.
            return (sg.get("diffuseFactor", [1.0, 1.0, 1.0, 1.0]),
                    sg.get("diffuseTexture"),
                    1.0 - float(sg.get("glossinessFactor", 0.0)))

        if "emissiveTexture" in material_js:
            em = material_js.get("emissiveFactor", [1.0, 1.0, 1.0])
            return ([em[0], em[1], em[2], 1.0], material_js["emissiveTexture"], 1.0)

        return ([1.0, 1.0, 1.0, 1.0], None, 1.0)

    def texture_index_for(material_js) -> int:
        _, tex_ref, _ = base_colour_of(material_js)
        if tex_ref is None:
            return C.NONE16
        t = js.get("textures", [])[tex_ref["index"]]
        # A texture may name its image through an extension instead of `source`.
        # EXT_texture_webp is the common one, and Pillow decodes WebP, so the
        # only thing missing was looking in the right place. A texture whose
        # source is only reachable through an extension reads as no texture at
        # all, and the model renders untextured without complaint.
        src = t.get("source")
        if src is None:
            for ext in t.get("extensions", {}).values():
                if isinstance(ext, dict) and "source" in ext:
                    src = ext["source"]
                    break
        if src is None or src not in image_to_texture:
            return C.NONE16
        return image_to_texture[src]

    # --- materials --------------------------------------------------------
    # a3d's rasterizer writes every covered pixel opaquely: there is no alpha
    # blend and no alpha test. A BLEND material therefore does not come out
    # faint, it comes out SOLID - and the shapes authored for it are usually
    # the ones that are meant to be nearly invisible (a propeller blur disc, a
    # soft shadow quad, a glow card), so they are large and they sit in front
    # of the model. That is silent twice over: the mesh draws, the counts are
    # right, and the offending quad also inflates worldBounds() and pushes the
    # camera back, which reads as "the model imported small" rather than as a
    # transparency problem. Say so, and offer the one fix there is.
    _non_opaque = non_opaque_materials(js)
    blend_materials: set[int] = set(_non_opaque)
    for _mi, (_name, _mode) in _non_opaque.items():
        warn(f"material {_name!r} is alphaMode {_mode}; a3d has no "
             "alpha blending and will draw it SOLID"
             + (" - dropped (--drop-blend)" if drop_blend
                else ". Pass --drop-blend to leave those primitives out"))
    for m in js.get("materials", []):
        base, _, rough = base_colour_of(m)
        if ("pbrMetallicRoughness" not in m) and (SPEC_GLOSS in m.get("extensions", {})):
            warn(f"material {m.get('name','')!r} has no pbrMetallicRoughness; "
                 f"its diffuse was read from {SPEC_GLOSS}")
        pbr_m = m.get("pbrMetallicRoughness", {})
        if ("emissiveTexture" in m) and ("baseColorTexture" not in pbr_m) and \
           (SPEC_GLOSS not in m.get("extensions", {})):
            warn(f"material {m.get('name','')!r} has no base colour texture; "
                 "its emissiveTexture was used as the diffuse map")
        # Map roughness onto a Phong exponent: smooth -> tight highlight.
        exponent = int(max(2, min(64, round(2.0 + 62.0 * (1.0 - rough) ** 2))))
        sc.materials.append(S.Material(
            name=m.get("name", ""),
            color=(float(base[0]), float(base[1]), float(base[2])),
            ambient=0.2, diffuse=0.7,
            specular=float(1.0 - rough) * 0.6,
            specular_exponent=exponent,
            texture_index=texture_index_for(m)))
    if not sc.materials:
        sc.materials.append(S.Material(name="default"))

    # --- meshes: one a3d mesh per glTF primitive --------------------------
    prim_lookup: dict[tuple[int, int], int] = {}
    prim_indices: dict[int, list[int]] = {}
    prim_joint_data: dict[int, tuple[list, list]] = {}
    for mi, m in enumerate(js.get("meshes", [])):
        for pi, prim in enumerate(m.get("primitives", [])):
            if prim.get("mode", 4) != 4:
                warn(f"mesh {mi} primitive {pi} is not TRIANGLES; skipped")
                continue
            if drop_blend and prim.get("material") in blend_materials:
                continue
            attrs = prim.get("attributes", {})
            if "POSITION" not in attrs:
                warn(f"mesh {mi} primitive {pi} has no POSITION; skipped")
                continue

            pos = [tuple(float(c) for c in v) for v in g.accessor(attrs["POSITION"])]
            nrm = ([tuple(float(c) for c in v) for v in g.accessor(attrs["NORMAL"])]
                   if "NORMAL" in attrs else None)
            uv = ([tuple(float(c) for c in v) for v in g.accessor(attrs["TEXCOORD_0"])]
                  if "TEXCOORD_0" in attrs else None)

            if "indices" in prim:
                idx = [int(v) for v in g.accessor(prim["indices"])]
            else:
                idx = list(range(len(pos)))
            if nrm is None:
                nrm = S.generate_normals(pos, idx)
                warn(f"mesh {mi} primitive {pi} has no NORMAL; generated "
                     f"{len(nrm)} vertex normals from the geometry")

            name = m.get("name", f"mesh{mi}")
            if len(m.get("primitives", [])) > 1:
                name = f"{name}.{pi}"

            joints_attr = weights_attr = None
            if "JOINTS_0" in attrs and "WEIGHTS_0" in attrs:
                joints_attr = [tuple(int(c) for c in v) for v in g.accessor(attrs["JOINTS_0"])]
                weights_attr = [tuple(float(c) for c in v) for v in g.accessor(attrs["WEIGHTS_0"])]

            if max_triangles and (len(idx) // 3 > max_triangles):
                # Decimate BEFORE quantization and before the skin binding is
                # reduced to two bones, so the collapse sees the geometry the
                # artist authored and carries the full binding across.
                extras = tuple(a for a in (joints_attr, weights_attr) if a is not None)
                pos, nrm, uv, idx, out_extras, st = D.decimate(
                    pos, nrm, uv, idx, max_triangles,
                    extra_per_vertex=extras, warn=warn)
                if joints_attr is not None:
                    joints_attr, weights_attr = out_extras[0], out_extras[1]
                if verbose:
                    print(f"  mesh {mi} primitive {pi}: {st['triangles_in']} -> "
                          f"{st['triangles_out']} triangles, {st['vertices_in']} -> "
                          f"{st['vertices_out']} vertices "
                          f"({st['locked']} vertex group(s) locked)")

            # The uint16 cap is checked AFTER decimation, not before: an asset
            # that only exceeds it in its authored form is exactly the one
            # --max-triangles exists for, and checking first turned every such
            # model into a valid container with no geometry in it - a file that
            # loads, reports success and draws nothing.
            if len(pos) > 65535:
                warn(f"mesh {mi} primitive {pi} has {len(pos)} vertices; "
                     "the format's uint16 indices cap this at 65535. Skipped."
                     + ("" if max_triangles else
                        " Pass --max-triangles to decimate it instead."))
                continue

            new_index = len(sc.meshes)
            sc.meshes.append(S.Mesh(
                name=name, positions=pos, normals=nrm, texcoords=uv, indices=idx,
                material_index=prim.get("material", C.NONE16)
                if prim.get("material") is not None else C.NONE16))
            prim_lookup[(mi, pi)] = new_index
            prim_indices.setdefault(mi, []).append(new_index)

            if joints_attr is not None:
                prim_joint_data[new_index] = (joints_attr, weights_attr)

    # --- nodes, reordered so parents precede children ---------------------
    order, remap, parent_of = _topo_order_nodes(js)
    gltf_nodes = js.get("nodes", [])
    # (a3d node, glTF node, a3d mesh) for every node that ends up carrying a
    # mesh, the synthesized ones included. The skin pass is driven from this.
    node_prims: list[tuple[int, int, int]] = []
    extra_prims: list[tuple[int, int, int, str]] = []
    for old_i in order:
        n = gltf_nodes[old_i]
        t = n.get("translation", [0.0, 0.0, 0.0])
        r = n.get("rotation", [0.0, 0.0, 0.0, 1.0])
        s = n.get("scale", [1.0, 1.0, 1.0])
        if "matrix" in n:
            t, r, s, residual = S.decompose_matrix(n["matrix"])
            # glTF requires a node matrix to be decomposable into TRS. If this
            # one is not, say so with the number, because the model will be
            # wrong in a way that renders without complaint.
            if residual > 1e-4:
                warn(f"node {old_i}: matrix does not decompose into TRS "
                     f"(largest element off by {residual:.6f}); shear or "
                     "projection is lost")
        prims = prim_indices.get(n["mesh"], []) if "mesh" in n else []
        p = parent_of[old_i]
        sc.nodes.append(S.Node(
            name=n.get("name", ""),
            parent=C.NONE16 if p == C.NONE16 else remap[p],
            mesh_index=prims[0] if prims else C.NONE16,
            skin_index=C.NONE16,
            translation=tuple(float(v) for v in t),
            rotation=tuple(float(v) for v in r),
            scale=tuple(float(v) for v in s)))
        if prims:
            node_prims.append((remap[old_i], old_i, prims[0]))
        # A glTF mesh may hold many primitives - a robot with one material per
        # part is a single mesh of 59 - but an a3d node carries exactly one.
        # The extras become identity children so they inherit the parent's
        # world transform and animate with it, which costs no format change.
        # Without this they stayed in the file and were never reachable.
        for extra in prims[1:]:
            extra_prims.append((remap[old_i], old_i, extra, n.get("name", "")))

    # Appended after the topological pass, so parents still precede children.
    for parent_node, old_i, mesh_index, base in extra_prims:
        node_prims.append((len(sc.nodes), old_i, mesh_index))
        sc.nodes.append(S.Node(
            name=f"{base}.{mesh_index}", parent=parent_node,
            mesh_index=mesh_index, skin_index=C.NONE16,
            translation=(0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0, 1.0),
            scale=(1.0, 1.0, 1.0)))

    # --- skins ------------------------------------------------------------
    # Every glTF skin folds into ONE skeleton, concatenated.
    #
    # The format has a single SKEL chunk, and the exporter used to refuse a file
    # with more than one skin - which ruled out a great many real assets: a Mario
    # rig with a separate one-joint skin per accessory has ten, Waluigi three,
    # the Khronos alien four, a Yeti six. None of them needed a format change.
    #
    # Concatenated, not merged by node: two skins may name the same joint node
    # with DIFFERENT inverse bind matrices, and deduplicating would silently pick
    # one of them. Concatenation keeps each skin's own binding, at the cost of
    # bones that would otherwise be shared.
    all_bones = []
    skin_base = {}
    for si, sk in enumerate(js.get("skins", [])):
        skin_base[si] = len(all_bones)
        all_bones.extend([None] * len(sk.get("joints", [])))
    if len(all_bones) > C.MAX_BONES:
        warn(f"{len(all_bones)} bones across {len(js.get('skins', []))} skins "
             f"exceeds the {C.MAX_BONES} cap (uint8 indices); skins beyond the "
             "cap are skipped")
    all_bones = []

    for si, sk in enumerate(js.get("skins", [])):
        joints = sk.get("joints", [])
        base = len(all_bones)
        if base + len(joints) > C.MAX_BONES:
            warn(f"skin {si} would push the skeleton past {C.MAX_BONES} bones "
                 "(uint8 indices). Skipped.")
            continue
        skin_base[si] = base
        ibm = (g.accessor(sk["inverseBindMatrices"])
               if "inverseBindMatrices" in sk else
               [tuple([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])] * len(joints))

        joint_to_bone = {jn: bi for bi, jn in enumerate(joints)}
        for bi, jn in enumerate(joints):
            p = parent_of[jn]
            # A parent outside this skin's own joint list has no bone here, so
            # the bone becomes a root and inherits placement through its node
            # instead - which is what the runtime reads anyway.
            local_parent = joint_to_bone.get(p, None) if p != C.NONE16 else None
            all_bones.append(S.Bone(
                name=gltf_nodes[jn].get("name", f"skin{si}bone{bi}"),
                node_index=remap[jn],
                parent=(base + local_parent) if local_parent is not None else C.NONE8,
                inv_bind=tuple(float(v) for v in ibm[bi])))
        skeleton_index = 0

        for a3d_node, old_i, mesh_index in node_prims:
            if gltf_nodes[old_i].get("skin") != si:
                continue
            if mesh_index not in prim_joint_data:
                continue
            joints_attr, weights_attr = prim_joint_data[mesh_index]

            bindings = []
            for vi in range(len(sc.meshes[mesh_index].positions)):
                j = joints_attr[vi] if vi < len(joints_attr) else (0, 0, 0, 0)
                w = weights_attr[vi] if vi < len(weights_attr) else (1.0, 0.0, 0.0, 0.0)
                # Keep the two heaviest influences and renormalize.
                pairs = sorted(zip(w, j), key=lambda p: -p[0])[:2]
                while len(pairs) < 2:
                    pairs.append((0.0, pairs[0][1]))
                (w0, j0), (w1, j1) = pairs
                total = w0 + w1
                q0 = 255 if total <= 0.0 else int(round(w0 / total * 255.0))
                # Local joint indices, shifted into the concatenated skeleton.
                bindings.append(S.SkinVertex(
                    bone0=min(base + j0, C.MAX_BONES - 1),
                    bone1=min(base + j1, C.MAX_BONES - 1),
                    weight0=max(0, min(255, q0))))

            sc.skins.append(S.Skin(mesh_index=mesh_index,
                                   skeleton_index=skeleton_index,
                                   bindings=bindings))
            sc.nodes[a3d_node].skin_index = len(sc.skins) - 1

    if all_bones:
        sc.skeletons.append(S.Skeleton(bones=all_bones))

    # --- animations -------------------------------------------------------
    path_map = {"translation": S.ANIM_TRANSLATION,
                "rotation": S.ANIM_ROTATION,
                "scale": S.ANIM_SCALE}
    # A rig with many bones makes animation the bulk of the container: the
    # dragon's 156 bones over 27 clips come to 16.7 MB of ANIM against 652 KB of
    # MESH. Not because the keys are unquantized - they are int16 rotations,
    # uint16 translations and uint16 times - but because there are 1.86 million
    # of them, exported at a fixed rate whether the bone moved or not. 34% of its
    # channels never change value at all.
    wanted = None
    if clips:
        # Names or indices, so a clip called Qishilong_skill10_dz can be picked
        # by number; a bare substring matches too.
        wanted = set()
        all_names = [a.get("name", "") for a in js.get("animations", [])]
        for token in clips:
            token = token.strip()
            if token.isdigit():
                wanted.add(int(token))
                continue
            hits = [i for i, n in enumerate(all_names) if n == token]
            if not hits:
                hits = [i for i, n in enumerate(all_names) if token.lower() in n.lower()]
            if not hits:
                warn(f"--clips: nothing matches {token!r}; available: "
                     + ", ".join(all_names))
            wanted.update(hits)

    # A translation tolerance has to mean something relative to the model, and
    # the mesh bounds are the only scale available here.
    scene_extent = 1.0
    if sc.meshes:
        lo = [min(min(v[k] for v in m.positions) for m in sc.meshes) for k in range(3)]
        hi = [max(max(v[k] for v in m.positions) for m in sc.meshes) for k in range(3)]
        scene_extent = max(hi[k] - lo[k] for k in range(3)) or 1.0
    anim_keys_in = anim_keys_out = 0

    for ai, anim in enumerate(js.get("animations", [])):
        if (wanted is not None) and (ai not in wanted):
            continue
        channels, duration = [], 0.0
        for ch in anim.get("channels", []):
            target = ch.get("target", {})
            p = path_map.get(target.get("path"))
            if p is None:
                warn(f"animation {ai}: unsupported target path "
                     f"{target.get('path')!r} (weights need the MRPH chunk); skipped")
                continue
            if "node" not in target:
                continue
            samp = anim["samplers"][ch["sampler"]]
            times = [float(t) for t in g.accessor(samp["input"])]
            vals = g.accessor(samp["output"])
            if not times:
                continue
            interp = S.ANIM_STEP if samp.get("interpolation") == "STEP" else S.ANIM_LINEAR
            if samp.get("interpolation") == "CUBICSPLINE":
                # Cubic keys store in/value/out triplets; take the values only.
                vals = vals[1::3]
                warn(f"animation {ai}: CUBICSPLINE resampled to LINEAR keys")
            duration = max(duration, times[-1])
            values = [tuple(float(c) for c in v) for v in vals]

            # Drop keys linear interpolation already reproduces. The tolerance
            # for a translation is scaled by the model's extent, so 0.05% means
            # the same thing on a 2-unit character and a 4,700-unit dragon;
            # rotation is in degrees and needs no scaling. STEP curves are left
            # alone - their whole content is where the steps are.
            if anim_tolerance > 0.0 and interp == S.ANIM_LINEAR:
                if p == S.ANIM_ROTATION:
                    tol = 0.1 * anim_tolerance
                elif p == S.ANIM_TRANSLATION:
                    tol = 0.0005 * anim_tolerance * scene_extent
                else:
                    tol = 0.0005 * anim_tolerance
                before = len(times)
                times, values = K.reduce_channel(times, values,
                                                 p == S.ANIM_ROTATION, tol)
                anim_keys_in += before
                anim_keys_out += len(times)

            channels.append(S.Channel(
                target_node=remap[target["node"]], path=p, interp=interp,
                times=times, values=values))
        if channels:
            sc.clips.append(S.Clip(name=anim.get("name", f"clip{ai}"),
                                   duration_ms=int(round(duration * 1000.0)) or 1,
                                   loop=True, channels=channels))

    if verbose and anim_keys_in:
        print(f"  animation keys {anim_keys_in} -> {anim_keys_out} "
              f"({anim_keys_out / anim_keys_in * 100:.0f}%) at tolerance "
              f"x{anim_tolerance:g}")

    # --- drop textures no material references -----------------------------
    # A glTF commonly ships an image set that its materials do not all use -
    # metallic-roughness and occlusion maps this renderer has no term for, or
    # variants left in the file by the authoring tool. They were being decoded,
    # converted to RGB565 and written into TEXR anyway, and a texture is stored
    # raw: at the default 256 px cap one costs 131,072 bytes whether or not
    # anything samples it. On esp32.glb that was two of three textures and 61%
    # of the container.
    #
    # It matters most for assets loaded from an SD card, where the whole file
    # has to fit in PSRAM before anything is drawn.
    # NONE16 is "no texture", not texture 65535. Reading it as an index put
    # 65535 into `used`, which both inflated the count this test compares
    # against - masking the drop on a scene with exactly one spare texture - and
    # then raised KeyError off the remap below. It needed a material with no
    # texture AND a texture nothing references in the same file, which is 8 of
    # the 974 Pokemon models.
    used = {m.texture_index for m in sc.materials
            if m.texture_index is not None and m.texture_index != C.NONE16}
    if len(used) < len(sc.textures):
        keep = [i for i in range(len(sc.textures)) if i in used]
        remap = {old_i: new_i for new_i, old_i in enumerate(keep)}
        dropped = len(sc.textures) - len(keep)
        freed = sum(len(sc.textures[i].pixels)
                    for i in range(len(sc.textures)) if i not in used)
        sc.textures = [sc.textures[i] for i in keep]
        for m in sc.materials:
            if m.texture_index is not None and m.texture_index != C.NONE16:
                m.texture_index = remap[m.texture_index]
        if verbose:
            print(f"  dropped {dropped} unreferenced texture(s), {freed} bytes")

    return sc
