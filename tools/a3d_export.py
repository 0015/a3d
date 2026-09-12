#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Eric Nam
# SPDX-License-Identifier: Apache-2.0

"""
a3d asset exporter.

    python3 a3d_export.py --self-test out.bin      write the verification fixture
    python3 a3d_export.py --inspect  file.bin      dump a container's structure

The fixture is not a toy: it is the file the C++ side loads in the host test
suite, so it deliberately exercises every chunk type, every optional field and
both the "present" and "absent" branches of each reference.
"""
from __future__ import annotations

import argparse
import math
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import a3d_container as C
import a3d_scene as S


def build_self_test_scene() -> S.Scene:
    sc = S.Scene()

    # --- texture: 8x8 RGB565, power of two so the wrap flag must be set -----
    px = bytearray()
    for y in range(8):
        for x in range(8):
            px += struct.pack("<H", 0xF800 if ((x ^ y) & 1) else 0x001F)
    sc.textures.append(S.Texture(name="checker", width=8, height=8,
                                 fmt=S.TEXEL_RGB565, pixels=bytes(px)))

    # --- materials: one textured, one not (exercises NONE16) ----------------
    sc.materials.append(S.Material(name="skin_mat", color=(0.9, 0.7, 0.6),
                                   ambient=0.25, diffuse=0.65, specular=0.4,
                                   specular_exponent=24, texture_index=0))
    sc.materials.append(S.Material(name="plain", color=(0.2, 0.4, 0.8),
                                   specular_exponent=8,
                                   texture_index=C.NONE16))

    # --- nodes: root, mesh node, then three bone nodes ----------------------
    #     Parents always precede children.
    sc.nodes.append(S.Node(name="root", parent=C.NONE16,
                           translation=(0.0, 0.0, 0.0)))
    sc.nodes.append(S.Node(name="body", parent=0, mesh_index=0, skin_index=0,
                           translation=(0.0, 1.0, 0.0)))
    sc.nodes.append(S.Node(name="bone_root", parent=0))
    sc.nodes.append(S.Node(name="bone_mid", parent=2, translation=(0.0, 0.5, 0.0)))
    sc.nodes.append(S.Node(name="bone_tip", parent=3, translation=(0.0, 0.5, 0.0)))

    # --- skeleton: 3 bones referencing nodes 2,3,4 --------------------------
    ident = tuple([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
    shifted = tuple([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0, -0.5, 0.0, 1])
    sc.skeletons.append(S.Skeleton(bones=[
        S.Bone(name="b_root", node_index=2, parent=C.NONE8, inv_bind=ident),
        S.Bone(name="b_mid",  node_index=3, parent=0,       inv_bind=shifted),
        S.Bone(name="b_tip",  node_index=4, parent=1,       inv_bind=shifted),
    ]))

    # --- mesh: a unit quad, 4 vertices / 2 triangles -------------------------
    sc.meshes.append(S.Mesh(
        name="quad",
        positions=[(-1.0, 0.0, -1.0), (1.0, 0.0, -1.0),
                   (1.0, 0.0,  1.0), (-1.0, 0.0,  1.0)],
        normals=[(0.0, 1.0, 0.0)] * 4,
        texcoords=[(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)],
        indices=[0, 1, 2, 0, 2, 3],
        material_index=0))

    # --- skin: each vertex blended between two bones -------------------------
    sc.skins.append(S.Skin(mesh_index=0, skeleton_index=0, bindings=[
        S.SkinVertex(0, 1, 255),    # fully bone 0
        S.SkinVertex(0, 1, 128),    # roughly half and half
        S.SkinVertex(1, 2, 64),
        S.SkinVertex(1, 2, 0),      # fully bone 2
    ]))

    # --- animation: rotation on a bone node, translation on the root ---------
    rot_keys = []
    for i in range(5):
        a = (i / 4.0) * (math.pi / 2.0)
        rot_keys.append((0.0, math.sin(a / 2), 0.0, math.cos(a / 2)))   # xyzw
    sc.clips.append(S.Clip(name="wave", duration_ms=2000, loop=True, channels=[
        S.Channel(target_node=3, path=S.ANIM_ROTATION, interp=S.ANIM_LINEAR,
                  times=[0.0, 0.5, 1.0, 1.5, 2.0], values=rot_keys),
        S.Channel(target_node=0, path=S.ANIM_TRANSLATION, interp=S.ANIM_LINEAR,
                  times=[0.0, 1.0, 2.0],
                  values=[(0.0, 0.0, 0.0), (0.0, 2.0, 0.0), (0.0, 0.0, 0.0)]),
    ]))

    # --- cooked blob: opaque to the core, tagged with a backend id ----------
    sc.cooked.append(S.CookedMesh(backend_id=C.BACKEND_COOKED_TEST,
                                  mesh_index=0,
                                  blob=bytes(range(64))))
    return sc


def build_grid_scene(cells: int) -> S.Scene:
    """
    A textured, subdivided grid: enough triangles that on-device timings mean
    something, unlike the 2-triangle self-test fixture.
    """
    sc = S.Scene()

    size = 64
    px = bytearray()
    for y in range(size):
        for x in range(size):
            v = ((x >> 3) ^ (y >> 3)) & 1
            px += struct.pack("<H", 0xFFE0 if v else 0x18E3)
    sc.textures.append(S.Texture(name="grid_tex", width=size, height=size,
                                 fmt=S.TEXEL_RGB565, pixels=bytes(px)))
    sc.materials.append(S.Material(name="grid_mat", color=(0.85, 0.85, 0.9),
                                   ambient=0.25, diffuse=0.7, specular=0.3,
                                   specular_exponent=16, texture_index=0))

    pos, nrm, uv, idx = [], [], [], []
    for gy in range(cells + 1):
        for gx in range(cells + 1):
            u = gx / cells
            v = gy / cells
            x = (u - 0.5) * 2.0
            z = (v - 0.5) * 2.0
            # A gentle dome so lighting and the depth buffer both do real work.
            y = 0.35 * math.cos(u * math.pi) * math.cos(v * math.pi)
            pos.append((x, y, z))
            d = math.sqrt(x * x + 1.0 + z * z)
            nrm.append((-x / d, 1.0 / d, -z / d))
            uv.append((u, v))
    for gy in range(cells):
        for gx in range(cells):
            a = gy * (cells + 1) + gx
            b = a + 1
            c = a + (cells + 1)
            d = c + 1
            # Counter-clockwise front faces, matching the glTF convention.
            # Emitting the other winding would mean generated fixtures and
            # imported models needed opposite culling settings.
            idx += [a, c, b, b, c, d]

    if len(pos) > 65535:
        raise SystemExit(f"grid {cells} would need {len(pos)} vertices; "
                         "the format caps meshes at 65535")

    sc.meshes.append(S.Mesh(name="dome", positions=pos, normals=nrm,
                            texcoords=uv, indices=idx, material_index=0))
    sc.nodes.append(S.Node(name="root"))
    sc.nodes.append(S.Node(name="dome_node", parent=0, mesh_index=0))
    return sc


def build_skinned_scene(rings: int, segments: int, nb_bones: int) -> S.Scene:
    """
    A skinned tube on a bone chain, with a whip animation.

    Sized like a character limb rather than a unit test: the point is to measure
    the real runtime skinning path on hardware, which the 6-vertex strip fixture
    cannot do. Weights blend between the two nearest bones, which is exactly the
    2-influence case the format stores.
    """
    import math as _m
    sc = S.Scene()

    seg_len = 0.5
    height = seg_len * (nb_bones - 1)
    radius = 0.35

    size = 64
    px = bytearray()
    for y in range(size):
        for x in range(size):
            v = ((x >> 3) ^ (y >> 3)) & 1
            px += struct.pack("<H", 0x07FF if v else 0x780F)
    sc.textures.append(S.Texture(name="tube_tex", width=size, height=size,
                                 fmt=S.TEXEL_RGB565, pixels=bytes(px)))
    sc.materials.append(S.Material(name="tube_mat", color=(0.8, 0.8, 0.85),
                                   ambient=0.25, diffuse=0.7, specular=0.3,
                                   specular_exponent=16, texture_index=0))

    # --- nodes: root, then a bone chain, then the mesh node ----------------
    # Parents always precede children, as the format requires.
    sc.nodes.append(S.Node(name="root"))
    for b in range(nb_bones):
        sc.nodes.append(S.Node(
            name=f"bone{b}",
            parent=0 if b == 0 else b,          # bone b lives at node b+1
            translation=(0.0, 0.0 if b == 0 else seg_len, 0.0)))
    mesh_node = len(sc.nodes)
    sc.nodes.append(S.Node(name="tube", parent=0, mesh_index=0, skin_index=0))

    # --- skeleton ----------------------------------------------------------
    bones = []
    for b in range(nb_bones):
        y = seg_len * b
        inv = (1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0.0, -y, 0.0, 1)
        bones.append(S.Bone(name=f"bone{b}", node_index=b + 1,
                            parent=C.NONE8 if b == 0 else b - 1, inv_bind=inv))
    sc.skeletons.append(S.Skeleton(bones=bones))

    # --- geometry ----------------------------------------------------------
    pos, nrm, uv, idx, bindings = [], [], [], [], []
    for r in range(rings):
        t = r / (rings - 1)
        y = t * height
        for sgi in range(segments):
            a = (sgi / segments) * 2.0 * _m.pi
            cx, cz = _m.cos(a), _m.sin(a)
            pos.append((radius * cx, y, radius * cz))
            nrm.append((cx, 0.0, cz))
            uv.append((sgi / segments, t))

            bf = y / seg_len
            b0 = int(bf)
            if b0 > nb_bones - 1:
                b0 = nb_bones - 1
            b1 = min(b0 + 1, nb_bones - 1)
            w0 = 1.0 - (bf - b0)
            bindings.append(S.SkinVertex(bone0=b0, bone1=b1,
                                         weight0=max(0, min(255, int(round(w0 * 255))))))
    for r in range(rings - 1):
        for sgi in range(segments):
            a = r * segments + sgi
            b = r * segments + (sgi + 1) % segments
            c = a + segments
            d = b + segments
            # Counter-clockwise front faces, matching the glTF convention.
            # Emitting the other winding would mean generated fixtures and
            # imported models needed opposite culling settings.
            idx += [a, c, b, b, c, d]

    if len(pos) > 65535:
        raise SystemExit(f"skinned grid would need {len(pos)} vertices; cap is 65535")

    sc.meshes.append(S.Mesh(name="tube", positions=pos, normals=nrm,
                            texcoords=uv, indices=idx, material_index=0))
    sc.skins.append(S.Skin(mesh_index=0, skeleton_index=0, bindings=bindings))

    # --- animation: a whip, amplitude rising along the chain ---------------
    channels = []
    nkeys = 9
    for b in range(1, nb_bones):
        amp = 0.30 * (b / (nb_bones - 1))
        keys = []
        for k in range(nkeys):
            phase = (k / (nkeys - 1)) * 2.0 * _m.pi
            ang = amp * _m.sin(phase)
            keys.append((0.0, 0.0, _m.sin(ang * 0.5), _m.cos(ang * 0.5)))
        channels.append(S.Channel(
            target_node=b + 1, path=S.ANIM_ROTATION, interp=S.ANIM_LINEAR,
            times=[(k / (nkeys - 1)) * 2.0 for k in range(nkeys)],
            values=keys))
    sc.clips.append(S.Clip(name="whip", duration_ms=2000, loop=True, channels=channels))
    return sc


def inspect(path: Path) -> int:
    data = path.read_bytes()
    info = C.read_header(data)
    print(f"{path}: {info['file_size']} bytes, "
          f"v{info['version'][0]}.{info['version'][1]}, "
          f"{len(info['chunks'])} chunks")
    for ch in info["chunks"]:
        print(f"  {C.fourcc_str(ch['type'])}  off={ch['offset']:<8} size={ch['size']:<8}"
              f" (4-aligned: {ch['offset'] % 4 == 0})")
    return 0


def parse_viewport(text, ap):
    try:
        w, h = text.lower().split("x")
        return int(w), int(h)
    except Exception:
        ap.error(f"--viewport wants WxH, e.g. 240x320 (got {text!r})")


def print_budget(vertices, triangles, args, animated=True):
    """Will this run, and on what. See tools/a3d_budget.py for the numbers."""
    import a3d_budget
    w, h = parse_viewport(args.viewport, argparse.ArgumentParser())
    if getattr(args, "static", False):
        animated = False
    for line in a3d_budget.report(vertices, triangles, animated=animated,
                                  view_w=w, view_h=h):
        print(line)


def find_viewer():
    """The built host previewer, or None.

    Looked for rather than required: a3d is header-only and somebody checking
    their first model has not necessarily built anything. Not finding it is a
    missing preview, not a failed check.
    """
    root = Path(__file__).resolve().parent.parent
    for rel in ("build/tests/a3d_view", "build/a3d_view", "a3d_view",
                "build/Release/a3d_view.exe", "build/a3d_view.exe"):
        q = root / rel
        if q.is_file():
            return q
    return None


def check(args, ap) -> int:
    """Import a model, say whether it will run, and draw it. Nothing is flashed.

    This exists because the three things somebody needs before committing to a
    model - does it import, will it run, does it look right - were three
    separate commands, one of which wrote a format the desktop cannot open. A
    person trying a3d for the first time did all three, or far more often none
    of them, and found out by flashing.
    """
    import a3d_gltf
    src = Path(args.check)
    if not src.is_file():
        ap.error(f"no such file: {src}")

    out = Path(args.out) if args.out else src.with_suffix(".a3d")
    print(f"{src}")

    scene = a3d_gltf.convert(src, max_texture=args.max_texture,
                             clips=args.clips.split(",") if args.clips else None,
                             max_triangles=args.max_triangles,
                             anim_tolerance=args.anim_tolerance,
                             drop_blend=args.drop_blend)
    data = S.build_container(scene)
    C.read_header(data)                 # never claim success without re-reading
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)

    tri = sum(len(m.indices) // 3 for m in scene.meshes)
    vtx = sum(len(m.positions) for m in scene.meshes)
    bones = len(scene.skeletons[0].bones) if scene.skeletons else 0
    print(f"  -> {out} ({len(data):,} bytes)")
    print(f"  {vtx:,} vertices, {tri:,} triangles, {bones} bones, "
          f"{len(scene.clips)} clip(s), {len(scene.textures)} texture(s)")
    for i, clip in enumerate(scene.clips):
        print(f"    clip {i}: {clip.name!r}")
    if not scene.clips:
        print("    no animation in this file - it will be drawn as a static pose")

    print()
    print_budget(vtx, tri, args, animated=bool(scene.clips))

    print()
    viewer = find_viewer()
    if viewer is None:
        print("preview: skipped, the host previewer is not built. To get one:")
        print("  cmake -S . -B build -DA3D_BUILD_TOOLS=ON && cmake --build build")
        print("then re-run this command and it will write a PNG of the model.")
        return 0

    png = Path(args.preview) if args.preview else out.with_name(out.stem + "_check.png")
    frames = 8 if scene.clips else 1
    cmd = [str(viewer), str(out), str(png), "--clip", "0",
           "--frames", str(frames), "--size", "240"]
    print("preview: " + " ".join(cmd))
    # The previewer writes straight to the terminal while this process's own
    # output sits in a buffer, so without this its lines come out ABOVE
    # everything printed here and the report reads back to front.
    sys.stdout.flush()
    rc = subprocess.call(cmd)
    if rc != 0:
        print(f"  the previewer exited {rc}; the container was still written")
        return 0
    print(f"  open {png} - {frames} frame(s) left to right. Every frame identical")
    print("  means the animation did not move; a missing model means the import")
    print("  lost it, and the warnings above say where.")
    return 0


def project(args, ap) -> int:
    """Write a buildable ESP-IDF project around a model.

    Takes either a .glb to convert or an .a3d already made. The point is that
    what comes out builds and runs BEFORE the panel is wired up, so the first
    thing the person sees is their own model's frame time on their own part.
    """
    import a3d_project as P
    import a3d_panel as P_PANEL

    dest = Path(args.project)
    src = args.check or args.gltf
    if src is None:
        ap.error("--project needs a model: --gltf model.glb (or --check model.glb)")
    src = Path(src)
    if not src.is_file():
        ap.error(f"no such file: {src}")

    w, h = parse_viewport(args.viewport, ap)
    root = Path(__file__).resolve().parent.parent

    dest.mkdir(parents=True, exist_ok=True)
    if src.suffix.lower() == ".a3d":
        model = src
        scene = None
    else:
        import a3d_gltf
        scene = a3d_gltf.convert(src, max_texture=args.max_texture,
                                 clips=args.clips.split(",") if args.clips else None,
                                 max_triangles=args.max_triangles,
                                 anim_tolerance=args.anim_tolerance,
                                 drop_blend=args.drop_blend)
        data = S.build_container(scene)
        C.read_header(data)             # never claim success without re-reading
        model = dest / (Path(args.out).name if args.out else src.stem + ".a3d")
        model.write_bytes(data)

    try:
        info = P.generate(dest, model, args.target, root, w, h,
                          args.tile_rows, args.flash_size, str(src),
                          panel=args.panel, touch=args.touch,
                          psram=args.psram, a3d_source=args.a3d_source)
    except ValueError as e:
        ap.error(str(e))

    if scene is not None:
        model.unlink()                  # generate() copied it into main/
        tri = sum(len(m.indices) // 3 for m in scene.meshes)
        vtx = sum(len(m.positions) for m in scene.meshes)
        print()
        print_budget(vtx, tri, args, animated=bool(scene.clips))

    print()
    print(f"wrote {dest}/  for {P.TARGETS[args.target]['name']}")
    print(f"  model      main/{model.name}  ({info['model_bytes']:,} bytes, embedded)")
    print(f"  symbol     {info['sym']}_start")
    print(f"  partition  {info['app_bytes'] // 1024} KB app in {info['flash_mb']} MB flash")
    print(f"  render     {info['panel'][0]}x{info['panel'][1]} in {info['tiles']} tiles"
          f" of {info['tile_h']} rows, {info['workers']} worker(s)"
          + (", PSRAM on" if info["psram"] else ""))
    print()
    print("next:")
    print(f"  cd {dest} && idf.py set-target {args.target} && idf.py -p <port> flash monitor")
    print(f"  PSRAM      {'on' if info['psram'] else 'off'} ({info['psram_mode']})")
    print(f"  a3d        " + ("fetched from " + info["a3d_git"] +
                              " @ " + info["a3d_ref"]
                              if info["a3d_source"] == "managed"
                              else "included by path"))
    print(f"  panel      {P_PANEL.PANELS[info['panel_id']]['label']}")
    print(f"  touch      {P_PANEL.TOUCH[info['touch_id']]['label']}")
    for name, ver in sorted(info["components"].items()):
        print(f"  component  {name} {ver}")
    print()
    print("  " + "-" * 68)
    for line in P.DISCLAIMER_TEXT.split(". "):
        print("  " + line.strip().rstrip(".") + ".")
    print("  " + "-" * 68)
    print()
    if info["panel_id"] == "offscreen":
        print("It renders off screen and logs the frame time, so it builds and runs")
        print("before you have written a display driver. Then regenerate with")
        print("--panel, or fill in main/panel.c yourself.")
    elif P_PANEL.PANELS[info["panel_id"]]["family"] == "custom":
        print("ONE FUNCTION IS YOURS TO WRITE: panel_bring_up() in main/panel.c.")
        print("Paste in the bring-up that already works on your board - a vendor")
        print("example, a BSP, an LVGL port - and set *out_panel and *out_io. The")
        print("rest of panel.c is written: the completion wait that keeps the tile")
        print("buffer safe, the bus mutex, fill and touch. Check PANEL_WAIT at the")
        print("top of that file matches your bus, and that PANEL_W/PANEL_H match")
        print("your glass. As generated it builds and flashes, and says it is empty.")
    else:
        print("THE PINS IN main/panel.c ARE NOT YOUR BOARD'S until you have checked")
        print(f"them - they are {P_PANEL.PANELS[info['panel_id']]['source']}'s.")
        print("Nothing here can know your wiring, and a plausible default driving")
        print("the wrong GPIO looks exactly like a dead panel.")
    return 0


def c_identifier(stem: str) -> str:
    """A C identifier from a file stem, the way a linker would need one.

    `2fast.a3d` cannot become `2fast_a3d` - an identifier may not start with a
    digit - so a leading underscore goes on. That is the opposite of the rule
    for ESP-IDF's EMBED_FILES symbols, where CMake runs MAKE_C_IDENTIFIER over
    the whole `_binary_<name>_start` and the `_binary_` prefix has already made
    it legal. Getting that backwards cost this tree a round; see CLAUDE.md.
    """
    out = "".join(c if (c.isalnum() or c == "_") else "_" for c in stem)
    if not out or out[0].isdigit():
        out = "_" + out
    return out


def to_header(src: Path, out: Path) -> int:
    """Write a .a3d as a C array, because Arduino has no EMBED_FILES.

    WHY THIS EXISTS

        On ESP-IDF a container goes into the firmware with EMBED_FILES and is
        read in place out of memory-mapped flash. The Arduino IDE has no such
        thing, so the only two routes onto a board are a file on SD/LittleFS -
        which `Viewer::openAssetFile()` already handles, since the loader is
        plain fopen and the Arduino cores register VFS mounts - or a byte array
        compiled into the sketch. This writes the second.

    THE ALIGNMENT IS NOT DECORATION

        `Reader::open()` casts the base pointer straight to `FileHeader*` and
        rejects any chunk whose offset is not a multiple of 4. EMBED_FILES
        gives 4-byte alignment for free; a plain `const uint8_t[]` in a sketch
        does not have to, and an unaligned container is a load exception or a
        silent refusal on the part, not a compile error. So the array carries
        the attribute, and tests/test_arduino.py checks that it still does.
    """
    data = src.read_bytes()
    C.read_header(data)                 # never write a header for a bad file
    name = c_identifier(src.stem + "_a3d")

    rows = []
    for i in range(0, len(data), 16):
        rows.append("    " + " ".join(f"0x{b:02x}," for b in data[i:i + 16]))

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        "// Generated by tools/a3d_export.py --header. Do not edit.\n"
        f"// Source: {src.name} ({len(data):,} bytes)\n"
        "//\n"
        "// Arduino has no EMBED_FILES, so the container is a byte array. On an\n"
        "// ESP32 a const array lives in .rodata, which is memory-mapped flash,\n"
        "// and a3d reads the container in place - so this costs flash, not RAM.\n"
        "//\n"
        "// The alignment is required, not cosmetic: a3d casts the base pointer\n"
        "// to the file header and rejects chunk offsets that are not a multiple\n"
        "// of 4. An unaligned array fails on the board, never at compile time.\n"
        "//\n"
        "//   #include \"%s\"\n"
        "//   viewer.openAsset(%s, %s_len);\n"
        "#pragma once\n"
        "#include <stdint.h>\n"
        "\n"
        "__attribute__((aligned(4)))\n"
        "static const uint8_t %s[] = {\n"
        "%s\n"
        "};\n"
        "static const unsigned int %s_len = %d;\n"
        % (out.name, name, name, name, "\n".join(rows), name, len(data))
    )
    print(f"{src} -> {out} ({len(data):,} bytes as `{name}`)")
    print(f"  #include \"{out.name}\"  then  viewer.openAsset({name}, {name}_len);")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="a3d asset exporter")
    ap.add_argument("--self-test", metavar="OUT", help="write the verification fixture")
    ap.add_argument("--inspect", metavar="FILE", help="dump a container's structure")
    ap.add_argument("--header", metavar="FILE",
                    help="write a .a3d as a C byte array for Arduino, which has "
                         "no EMBED_FILES (default output: FILE with a .h suffix)")
    ap.add_argument("--gltf", metavar="FILE", help="import a .gltf or .glb file")
    ap.add_argument("--check", metavar="FILE",
                    help="import FILE, say whether it will run and on what, and "
                         "render a preview image. The one command to try a model "
                         "with: nothing is flashed and nothing is assumed.")
    ap.add_argument("--project", metavar="DIR",
                    help="write a complete ESP-IDF project around the model in "
                         "DIR: partition sized for it, sdkconfig for the part, "
                         "the embed symbol spelled correctly, and one function "
                         "left for your panel. Builds before you edit anything.")
    ap.add_argument("--target", default="esp32s3",
                    help="part the project is for (--project). "
                         "esp32s3, esp32p4 or esp32c6. Default esp32s3.")
    ap.add_argument("--flash-size", type=int, default=4, metavar="MB",
                    help="flash on the board, in MB (--project). Default 4, "
                         "which is the conservative common size: an image built "
                         "for more flash than the board has will not run.")
    ap.add_argument("--panel", default="offscreen",
                    help="display interface for --project; see --list-panels")
    ap.add_argument("--touch", default="none",
                    help="touch controller for --project; see --list-panels")
    ap.add_argument("--psram", default="auto", choices=("auto", "on", "off"),
                    help="PSRAM in the generated sdkconfig (default: auto, "
                         "on for a model over 150 KB on a part that has it)")
    ap.add_argument("--a3d-source", default="path", choices=("path", "managed"),
                    help="how the generated project finds a3d: a local checkout "
                         "by path, or fetched from git by the component manager")
    ap.add_argument("--list-panels", action="store_true",
                    help="what --panel and --touch accept, per target")
    ap.add_argument("--tile-rows", type=int, default=40, metavar="N",
                    help="rows per tile (--project). A slot costs "
                         "width*N*2 bytes twice over. Default 40.")
    ap.add_argument("--viewport", metavar="WxH", default="240x320",
                    help="the viewport the budget is estimated for (default "
                         "240x320, the size every measurement was taken at). A "
                         "800x1280 panel is thirteen times the pixels and the "
                         "rasterizer is paid per pixel, so this matters more "
                         "than the triangle count.")
    ap.add_argument("--preview", metavar="OUT.png",
                    help="where --check writes its preview (default: beside the model)")
    ap.add_argument("--static", action="store_true",
                    help="budget a model that is never animated: no skinning")
    ap.add_argument("--max-triangles", type=int, default=0, metavar="N",
                    help="simplify any mesh above N triangles (0 = off). "
                         "Measured on an ESP32-P4 the frame costs 3.68 us per "
                         "triangle, and an asset drawn at under one pixel per "
                         "triangle is paying that for nothing.")
    ap.add_argument("--anim-tolerance", type=float, default=1.0, metavar="SCALE",
                    help="keyframe reduction strength; 1.0 (default) is 0.1 deg "
                         "of rotation and 0.05%% of model extent, 0 disables it. "
                         "The bound is checked against every original key, so it "
                         "is an error bound on the whole curve.")
    ap.add_argument("--drop-blend", action="store_true",
                    help="leave out primitives whose material is alphaMode "
                         "BLEND or MASK. a3d draws every covered pixel opaquely, "
                         "so a propeller blur disc or a soft shadow quad comes "
                         "out SOLID in front of the model - and inflates the "
                         "bounding box, which reads as the model importing "
                         "small. Without this they are imported and warned about.")
    ap.add_argument("--clips", metavar="LIST",
                    help="comma-separated animation names or indices to keep "
                         "(default: all). Animation keys are not quantized, so "
                         "on a many-bone rig this is what decides file size.")
    ap.add_argument("--grid", type=int, metavar="N",
                    help="generate an NxN textured dome (N*N*2 triangles)")
    ap.add_argument("--skinned", type=int, metavar="RINGS",
                    help="generate a skinned, animated tube with RINGS rings")
    ap.add_argument("--segments", type=int, default=24, help="tube segments (--skinned)")
    ap.add_argument("--bones", type=int, default=8, help="bone count (--skinned)")
    ap.add_argument("-o", "--out", metavar="OUT", help="output .a3d path")
    ap.add_argument("--max-texture", type=int, default=256,
                    help="longest texture edge in pixels (default 256)")
    args = ap.parse_args()

    if args.list_panels:
        import a3d_panel as PN
        print("--panel        (per target)")
        for k, v in PN.PANELS.items():
            print(f"  {k:<14} {v['label']}")
            # "native" is the glass's own size and overrides --viewport.
            # The custom panel has none - saying 240x320 there would name a
            # size the generated project does not use.
            size = (f"native {v['size'][0]}x{v['size'][1]}"
                    if v.get("native", True) else "size: whatever --viewport says")
            print(f"  {'':<14} targets: {', '.join(v['targets'])}   {size}")
            print(f"  {'':<14} pins from: {v['source']}")
        print()
        print("--touch")
        for k, v in PN.TOUCH.items():
            print(f"  {k:<14} {v['label']}")
        return 0

    if args.project:
        return project(args, ap)

    if args.check:
        return check(args, ap)

    if args.inspect:
        return inspect(Path(args.inspect))

    if args.header:
        src = Path(args.header)
        if not src.is_file():
            ap.error(f"no such file: {src}")
        return to_header(src, Path(args.out) if args.out else src.with_suffix(".h"))

    if args.skinned:
        if not args.out:
            ap.error("--skinned requires -o/--out")
        scene = build_skinned_scene(args.skinned, args.segments, args.bones)
        data = S.build_container(scene)
        info = C.read_header(data)
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        tri = sum(len(m.indices) // 3 for m in scene.meshes)
        vtx = sum(len(m.positions) for m in scene.meshes)
        ch = sum(len(c.channels) for c in scene.clips)
        print(f"wrote {out} ({len(data)} bytes) vertices={vtx} triangles={tri} "
              f"bones={len(scene.skeletons[0].bones)} channels={ch}")
        for c2 in info["chunks"]:
            print(f"  {C.fourcc_str(c2['type'])}  off={c2['offset']:<8} size={c2['size']}")
        return 0

    if args.grid:
        if not args.out:
            ap.error("--grid requires -o/--out")
        scene = build_grid_scene(args.grid)
        data = S.build_container(scene)
        info = C.read_header(data)
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        tri = sum(len(m.indices) // 3 for m in scene.meshes)
        vtx = sum(len(m.positions) for m in scene.meshes)
        print(f"wrote {out} ({len(data)} bytes) grid={args.grid} "
              f"triangles={tri} vertices={vtx}")
        for ch in info["chunks"]:
            print(f"  {C.fourcc_str(ch['type'])}  off={ch['offset']:<8} size={ch['size']}")
        return 0

    if args.gltf:
        if not args.out:
            ap.error("--gltf requires -o/--out")
        import a3d_gltf
        scene = a3d_gltf.convert(Path(args.gltf), max_texture=args.max_texture,
                                 clips=args.clips.split(",") if args.clips else None,
                                 max_triangles=args.max_triangles,
                                 anim_tolerance=args.anim_tolerance,
                                 drop_blend=args.drop_blend)
        data = S.build_container(scene)
        info = C.read_header(data)          # never claim success without re-reading
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        print(f"{args.gltf} -> {out} ({len(data)} bytes)")
        print(f"  nodes={len(scene.nodes)} meshes={len(scene.meshes)} "
              f"skins={len(scene.skins)} clips={len(scene.clips)} "
              f"materials={len(scene.materials)} textures={len(scene.textures)}")
        if scene.skeletons:
            print(f"  bones={len(scene.skeletons[0].bones)}")
        tri = sum(len(m.indices) // 3 for m in scene.meshes)
        vtx = sum(len(m.positions) for m in scene.meshes)
        print(f"  triangles={tri} vertices={vtx}")
        for ch in info["chunks"]:
            print(f"    {C.fourcc_str(ch['type'])}  off={ch['offset']:<8} size={ch['size']}")
        print()
        print_budget(vtx, tri, args, animated=bool(scene.clips))
        return 0

    if args.self_test:
        scene = build_self_test_scene()
        data = S.build_container(scene)

        # Re-read what we just wrote before claiming success. An exporter that
        # cannot parse its own output has no business writing files.
        info = C.read_header(data)
        out = Path(args.self_test)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        print(f"wrote {out} ({len(data)} bytes, {len(info['chunks'])} chunks)")
        for ch in info["chunks"]:
            print(f"  {C.fourcc_str(ch['type'])}  off={ch['offset']:<8} size={ch['size']}")
        return 0

    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
