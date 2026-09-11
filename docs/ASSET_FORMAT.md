# a3d Asset Container — Format Specification v0.1

> **Normative definition:** `src/a3d/a3d_format.h`
> Every struct size in this document is enforced by `static_assert` and verified
> by compilation.

---

## 1. Goals

1. **Zero-copy from flash.** A file mapped at a flash address must be usable
   without rewriting a single byte. This is why there are **no pointers** —
   only `uint32_t` byte offsets.
2. **Loadable from SD at runtime.** Read once into one block; the same offset
   arithmetic applies.
3. **Extensible without breaking.** Chunk-based, so morph targets and future
   backends can be added with no re-export of existing assets.
4. **Preserve offline optimization.** Expensive offline work — triangle-chain
   stripification, meshlet construction, visibility cones — must be able to
   survive to the runtime, without the core knowing which renderer produced it.
5. **Small.** Flash and RAM are the binding constraints, not ALU throughput.

---

## 2. Conventions

| Rule | Value |
|---|---|
| Byte order | Little-endian only. Verified via `byte_order` sentinel `0x04030201`. |
| Alignment | Every chunk offset and every referenced blob offset is a multiple of **4**. Pad with zero bytes. |
| References | `uint32_t` byte offset **from the start of the file image**. Never a pointer. |
| "No value" | `NONE32 = 0xFFFFFFFF`, `NONE16 = 0xFFFF`, `NONE8 = 0xFF` |
| Matrices | 16 floats, **column-major** (matches OpenGL and `a3d::IRenderBackend`). |
| Quaternions | `xyzw`, unit length. |
| Strings | UTF-8, NUL-terminated, in the `STRT` chunk. Referenced by byte offset into that chunk's payload. |
| Unknown chunks | **MUST be skipped**, never an error. This is the forward-compatibility mechanism. |

---

## 3. File layout

```
┌────────────────────────────────┐  offset 0
│ FileHeader              32 B   │
├────────────────────────────────┤
│ ChunkEntry[chunk_count] 16 B ea│  at header.chunk_table_off
├────────────────────────────────┤
│ chunk payloads...              │  each at entry.offset, 4-byte aligned
│   STRT / NODE / SKEL / MESH    │
│   MSHC / SKIN / MATL / TEXR    │
│   ANIM / (MRPH reserved)       │
└────────────────────────────────┘  total = header.file_size
```

### FileHeader (32 B)

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | `'A3DA'` = `0x41443341` |
| 4 | 2 | `version_major` | 0 |
| 6 | 2 | `version_minor` | 1 |
| 8 | 4 | `byte_order` | must read back as `0x04030201` |
| 12 | 4 | `file_size` | total image size |
| 16 | 4 | `chunk_count` | |
| 20 | 4 | `chunk_table_off` | |
| 24 | 4 | `flags` | |
| 28 | 4 | `reserved` | |

### ChunkEntry (16 B)

| Off | Size | Field |
|---|---|---|
| 0 | 4 | `type` (FourCC) |
| 4 | 4 | `offset` (from file start, %4 == 0) |
| 8 | 4 | `size` |
| 12 | 4 | `flags` |

### Chunk registry

| FourCC | Purpose | v0.1 runtime |
|---|---|---|
| `STRT` | String table | ✅ |
| `NODE` | Scene graph; target of node/rigid animation | ✅ |
| `SKEL` | Bones, parents, inverse bind matrices | ✅ |
| `MESH` | Generic quantized geometry (skinning-capable) | ✅ |
| `MSHC` | Cooked, backend-specific opaque blob | ✅ |
| `SKIN` | Per-vertex bone binding | ✅ |
| `MATL` | Materials | ✅ |
| `TEXR` | Textures | ✅ |
| `ANIM` | Animation clips and channels | ✅ |
| `MRPH` | **RESERVED — morph targets** | ⛔ skipped |

**On `MRPH`** — Reserved per the scope decision: the runtime
does not implement morph blending, but the chunk ID is allocated and the glTF
importer may emit it. When morph support lands, existing asset files do not
need re-exporting.

---

## 4. Two geometry paths

**EN** — This is the central design decision of the format.

A cooked mesh quantizes vertices relative to a **per-meshlet bounding sphere**
and culls with **visibility cones**. Both assume vertices never move, so a
skinned mesh cannot use that path. Dropping it for static meshes too would
forfeit the faster route where it is available. So the format carries both:

| Mesh kind | Chunk | Draw path | Culling |
|---|---|---|---|
| **Static** | `MSHC` if a recognised `backend_id` is present, else `MESH` | the backend's own mesh path | full meshlet sphere + cone culling |
| **Skinned** | `MESH` + `SKIN` | skinning → `IRenderBackend::drawTriangles()` | per-mesh AABB derived from bone bounds |

A file MAY contain **both** `MESH` and `MSHC` for the same logical mesh. A
backend that does not recognise `backend_id` silently falls back to the generic
`MESH`. That is what keeps the container portable across future renderers.

---

## 5. Chunk payloads

### 5.1 `STRT` — string table

```
uint8_t data[chunk.size]     // concatenated NUL-terminated UTF-8 strings
```
A `name_off` of `NONE32` means "no name". Offset `0` is a valid (empty) string
if the table begins with `'\0'`.

### 5.2 `NODE` — scene graph

```
uint32_t   node_count
NodeEntry  nodes[node_count]      // 52 B each
```

**NodeEntry (52 B)**

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `name_off` | into `STRT`, or `NONE32` |
| 4 | 2 | `parent` | node index, or `NONE16` for a root |
| 6 | 2 | `flags` | |
| 8 | 2 | `mesh_index` | into `MESH`, or `NONE16` |
| 10 | 2 | `skin_index` | into `SKIN`, or `NONE16` |
| 12 | 12 | `translation[3]` | float |
| 24 | 16 | `rotation[4]` | float quaternion xyzw |
| 40 | 12 | `scale[3]` | float |

Parents MUST appear **before** their children, so a single forward pass computes
world transforms with no recursion and no stack.

### 5.3 `SKEL` — skeleton

```
uint32_t   bone_count      // <= 255
uint32_t   reserved
BoneEntry  bones[bone_count]      // 72 B each
```

**BoneEntry (72 B)**

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `name_off` | |
| 4 | 2 | `node_index` | into `NODE` — bones are animated through their node |
| 6 | 1 | `parent` | bone index, or `NONE8` |
| 7 | 1 | `_pad` | |
| 8 | 64 | `inv_bind[16]` | inverse bind matrix, column-major |

**Cap: 255 bones**, because `SkinVertex` stores `uint8_t` bone indices.
Typical character rigs use 40–80.

### 5.4 `MESH` — generic quantized geometry

```
uint32_t   mesh_count
MeshEntry  meshes[mesh_count]     // 56 B each
... blobs referenced by the entries ...
```

**MeshEntry (56 B)**

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `name_off` | |
| 4 | 4 | `vertex_count` | |
| 8 | 4 | `triangle_count` | |
| 12 | 4 | `positions_off` | `int16[3 * vertex_count]` |
| 16 | 4 | `normals_off` | `int16[3 * vertex_count]`, or `NONE32` |
| 20 | 4 | `texcoords_off` | `int16[2 * vertex_count]`, or `NONE32` |
| 24 | 4 | `indices_off` | `uint16[3 * triangle_count]` |
| 28 | 2 | `material_index` | into `MATL`, or `NONE16` |
| 30 | 2 | `flags` | |
| 32 | 12 | `bbox_min[3]` | float |
| 44 | 12 | `bbox_max[3]` | float |

**Decode — exporter and loader MUST match bit for bit:**

```
center = 0.5 * (bbox_min + bbox_max)
extent = max(bbox_max.x-bbox_min.x, bbox_max.y-bbox_min.y, bbox_max.z-bbox_min.z)
pscale = extent / 32767

position = center + q * pscale          // q : int16[3]
normal   = q * (1 / 32767)              // q : int16[3], snorm
texcoord = q * (4 / 32767)              // q : int16[2], approx [-4,4] to allow tiling
```

A **uniform** position scale is used (one `extent`, not per-axis) to avoid
anisotropic precision artifacts, matching `Mesh3Dv2`'s proven approach.

**Cost: 16 bytes per vertex** (6 pos + 6 normal + 4 uv) versus 32 bytes if
stored as floats. The dequantization is 3 multiply-adds per vertex — negligible
next to the ~150 flops the skinning step already spends there.

### 5.5 `MSHC` — cooked mesh

```
uint32_t         cooked_count
CookedMeshEntry  cooked[cooked_count]   // 16 B each
... opaque blobs ...
```

**CookedMeshEntry (16 B)**

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `backend_id` | which renderer this blob was cooked for; 0 = none |
| 4 | 4 | `mesh_index` | matching logical mesh in `MESH`, or `NONE32` |
| 8 | 4 | `blob_off` | %4 == 0 |
| 12 | 4 | `blob_size` | |

The core **never** interprets the blob. Only a backend advertising the matching
`backend_id` consumes it.

### 5.6 `SKIN` — per-vertex bone binding

```
uint32_t   skin_count
SkinEntry  skins[skin_count]      // 16 B each
... SkinVertex arrays ...
```

**SkinEntry (16 B)**

| Off | Size | Field |
|---|---|---|
| 0 | 4 | `mesh_index` |
| 4 | 4 | `skeleton_index` |
| 8 | 4 | `bindings_off` → `SkinVertex[mesh.vertex_count]` |
| 12 | 4 | `reserved` |

**SkinVertex (4 B)**

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `bone0` | |
| 1 | 1 | `bone1` | |
| 2 | 1 | `weight0` | `weight1` is implicitly `255 - weight0` |
| 3 | 1 | `_pad` | |

**Two influences per vertex** per the scope decision. Storing only `weight0`
makes the pair self-normalizing and costs **4 B/vertex instead of 8 B**.
The glTF importer keeps the two largest weights and renormalizes.

### 5.7 `MATL` — materials

```
uint32_t       material_count
MaterialEntry  materials[material_count]   // 32 B each
```

| Off | Size | Field |
|---|---|---|
| 0 | 4 | `name_off` |
| 4 | 12 | `color[3]` float, RGB in [0,1] |
| 16 | 4 | `ambient` float |
| 20 | 4 | `diffuse` float |
| 24 | 4 | `specular` float |
| 28 | 2 | `specular_exponent` uint16 |
| 30 | 2 | `texture_index` into `TEXR`, or `NONE16` |

Fields map 1:1 onto `a3d::Material`.

### 5.8 `TEXR` — textures

```
uint32_t      texture_count
TextureEntry  textures[texture_count]      // 20 B each
... pixel blobs, each 4-byte aligned ...
```

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `name_off` | |
| 4 | 2 | `width` | |
| 6 | 2 | `height` | |
| 8 | 2 | `format` | `0` RGB565, `1` RGB24, `2` RGB32 |
| 10 | 2 | `flags` | bit0 `TEXFLAG_POW2` — both dims power of two → wrap-capable |
| 12 | 4 | `pixels_off` | %4 == 0 |
| 16 | 4 | `pixels_size` | |

`TEXFLAG_POW2` matters because wrapping by bit mask requires power-of-two
dimensions; otherwise a slower clamp or modulo path must be selected. a3d's
rasterizer masks, so the exporter guarantees the flag.

### 5.9 `ANIM` — clips and channels

```
uint32_t   clip_count
ClipEntry  clips[clip_count]      // 24 B each
... ChannelEntry arrays, time arrays, value arrays ...
```

**ClipEntry (24 B)**

| Off | Size | Field |
|---|---|---|
| 0 | 4 | `name_off` |
| 4 | 4 | `duration_ms` |
| 8 | 4 | `channel_count` |
| 12 | 4 | `channels_off` → `ChannelEntry[channel_count]` |
| 16 | 2 | `flags` — bit0 `CLIP_LOOP` |
| 18 | 2 | `_pad` |
| 20 | 4 | `reserved` |

**ChannelEntry (48 B)**

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | `target_node` | into `NODE` |
| 2 | 1 | `path` | 0 translation, 1 rotation, 2 scale |
| 3 | 1 | `interp` | 0 step, 1 linear |
| 4 | 2 | `key_count` | |
| 6 | 2 | `_pad` | |
| 8 | 4 | `times_off` | `uint16[key_count]` |
| 12 | 4 | `values_off` | see below |
| 16 | 12 | `vmin[3]` | float, translation/scale only |
| 28 | 12 | `vmax[3]` | float, translation/scale only |
| 40 | 8 | `reserved[2]` | |

**Keyframe decode**

```
time_ms  = (times[i] / 65535.0) * clip.duration_ms

rotation : int16  q[4]  ->  value = q / 32767            // xyzw, renormalize after lerp
translate: uint16 v[3]  ->  value = vmin + (v / 65535.0) * (vmax - vmin)
scale    : uint16 v[3]  ->  same as translate
```

Per-key cost: **10 B** for rotation, **8 B** for translation/scale.

**Animation targets nodes, not bones.** A bone is animated by animating its
node; the skeleton only supplies inverse bind matrices. This makes node/rigid
animation and skeletal animation the **same code path**, with skinning as an
extra step applied only when a `SKIN` binding exists.

---

## 6. Loading model

**EN** — Two modes, one code path:

| Mode | RAM cost | How |
|---|---|---|
| **XIP / in place** | asset RAM = **0** | The image already sits at a readable address (flash, memory-mapped partition). Only a small runtime index of pointers (`base + offset`) is built. |
| **Read into RAM** | asset RAM = `file_size` | `malloc(file_size)`, read once, then identical offset arithmetic. |

**Neither mode patches per-element data.** Only a handful of small runtime
structs hold pointers. A backend consuming a cooked blob builds its own mesh
struct plus its material array in RAM, with the pointers aimed into the blob —
a fixup of a few pointers, not of every vertex.

---

## 7. Worked memory budget

Model: 3,000 vertices, 5,000 triangles, 80 bones, one 256×256 RGB565 texture.

| Chunk | Computation | Size |
|---|---|---|
| `MESH` positions | 6 B × 3,000 | 17.6 KB |
| `MESH` normals | 6 B × 3,000 | 17.6 KB |
| `MESH` texcoords | 4 B × 3,000 | 11.7 KB |
| `MESH` indices | 2 B × 3 × 5,000 | 29.3 KB |
| `SKIN` | 4 B × 3,000 | 11.7 KB |
| `SKEL` | 72 B × 80 | 5.6 KB |
| `NODE` | 52 B × ~100 | 5.1 KB |
| `ANIM` | 240 channels × 48 B + ~24 KB keys | ~35 KB |
| `TEXR` | 256 × 256 × 2 B | 128 KB |
| **Asset file total** | | **≈ 262 KB** |

| Runtime RAM | Computation | Size |
|---|---|---|
| Skinning output (positions + normals, backend-allocated `fVec3`) | 24 B × 3,000 | **70 KB** |

| Deployment | RAM needed |
|---|---|
| Asset in **flash (XIP)** + skinning buffer | **≈ 70 KB** |
| Asset from **SD into RAM** + skinning buffer | **≈ 332 KB** |

**EN** — The texture dominates the file. Keeping textures in flash while
streaming only geometry and animation from SD is therefore the most effective
hybrid, and the chunk layout supports it directly: chunks are independently
addressable, so a loader may map `TEXR` from flash and read the rest from SD.

---

## 8. Versioning

| Change | Version bump | Old readers |
|---|---|---|
| New chunk type | minor | skip it, keep working |
| New field in reserved space | minor | ignore it, keep working |
| Changed struct size or decode formula | **major** | **must reject** |

Readers MUST reject a file whose `version_major` exceeds the one they were
built against, and MUST accept any `version_minor`.

---

## 9. Open items for v0.2

| # | Item | Note |
|---|---|---|
| 1 | `MRPH` payload layout | Reserved only. Sparse delta storage is essential — dense targets would exceed the mesh itself.
| 2 | Optional zlib/LZ4 per chunk | Would break XIP for compressed chunks; only worth it for SD-loaded assets.
| 3 | Bone-bounds chunk for skinned culling | Skinned meshes lose meshlet culling; a per-bone AABB would restore coarse rejection.
| 4 | 4-bone variant | `flags` bit reserved; would need an 8 B `SkinVertex`.
| 5 | Emissive material fields | `Mesh3Dv2` already carries them; the renderer does not shade with them yet.

---

## 10. Status

- ✅ Struct definitions: `src/a3d/a3d_format.h`
- ✅ All 12 struct sizes verified by `static_assert` + compilation
- ⬜ PC-side exporter (Phase 2)
- ⬜ MCU-side loader (Phase 3)
