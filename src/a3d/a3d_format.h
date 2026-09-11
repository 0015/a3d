// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_format.h
 * @brief On-disk asset container layout. Normative definition.
 *
 * See docs/ASSET_FORMAT.md for the rationale and the loading model.
 *
 * HARD RULES
 *  1. NO POINTERS. Every internal reference is a uint32_t BYTE OFFSET from the
 *     start of the file image. This is what makes the container usable
 *     zero-copy directly from flash (XIP).
 *  2. Little-endian. All current targets are LE (x86, ARM, Xtensa, RISC-V).
 *  3. Every chunk offset and every referenced blob offset is a multiple of 4.
 *  4. Unknown chunk types MUST be skipped, not treated as an error.
 */
#ifndef A3D_FORMAT_H_
#define A3D_FORMAT_H_

#include <stdint.h>

namespace a3d {
namespace fmt {

// ---------------------------------------------------------------------------
// FourCC / sentinels
// ---------------------------------------------------------------------------

constexpr uint32_t fourcc(char a, char b, char c, char d)
    {
    return (uint32_t)(uint8_t)a
         | ((uint32_t)(uint8_t)b << 8)
         | ((uint32_t)(uint8_t)c << 16)
         | ((uint32_t)(uint8_t)d << 24);
    }

constexpr uint32_t MAGIC        = fourcc('A','3','D','A');
constexpr uint32_t BYTE_ORDER_OK = 0x04030201u;   ///< reads back as this on LE

constexpr uint16_t VERSION_MAJOR = 0;
constexpr uint16_t VERSION_MINOR = 1;

/** Sentinel for "no reference". Used for every optional index/offset. */
constexpr uint32_t NONE32 = 0xFFFFFFFFu;
constexpr uint16_t NONE16 = 0xFFFFu;
constexpr uint8_t  NONE8  = 0xFFu;


// ---------------------------------------------------------------------------
// Chunk type registry
// ---------------------------------------------------------------------------

enum ChunkType : uint32_t
    {
    CHUNK_STRT = fourcc('S','T','R','T'),  ///< string table
    CHUNK_NODE = fourcc('N','O','D','E'),  ///< node hierarchy (scene graph)
    CHUNK_SKEL = fourcc('S','K','E','L'),  ///< skeleton: bones + inverse bind matrices
    CHUNK_MESH = fourcc('M','E','S','H'),  ///< generic mesh (quantized arrays)
    CHUNK_MSHC = fourcc('M','S','H','C'),  ///< cooked mesh, backend-specific opaque blob
    CHUNK_SKIN = fourcc('S','K','I','N'),  ///< per-vertex bone binding
    CHUNK_MATL = fourcc('M','A','T','L'),  ///< materials
    CHUNK_TEXR = fourcc('T','E','X','R'),  ///< textures
    CHUNK_ANIM = fourcc('A','N','I','M'),  ///< animation clips

    /// RESERVED for morph targets. v0.1 runtime ignores it; the importer may
    /// still emit it so no re-export is needed when runtime support lands.
    CHUNK_MRPH = fourcc('M','R','P','H'),
    };

/** Backend id for a cooked mesh blob a renderer can consume directly.
    A container may carry a pre-built mesh for one particular backend; the id
    says which, and a runtime that does not recognise it ignores the blob. */
constexpr uint32_t BACKEND_COOKED_NONE = 0u;


// ---------------------------------------------------------------------------
// File header and chunk table
// ---------------------------------------------------------------------------

struct FileHeader                   // 32 bytes
    {
    uint32_t magic;                 ///< MAGIC
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t byte_order;            ///< must read back as BYTE_ORDER_OK
    uint32_t file_size;             ///< total image size in bytes
    uint32_t chunk_count;
    uint32_t chunk_table_off;       ///< offset of ChunkEntry[chunk_count]
    uint32_t flags;
    uint32_t reserved;
    };

struct ChunkEntry                   // 16 bytes
    {
    uint32_t type;                  ///< ChunkType
    uint32_t offset;                ///< from file start, multiple of 4
    uint32_t size;                  ///< payload size in bytes
    uint32_t flags;
    };


// ---------------------------------------------------------------------------
// NODE - scene graph. Also the target of node/rigid animation.
// Payload: uint32_t node_count, then NodeEntry[node_count].
// ---------------------------------------------------------------------------

struct NodeEntry                    // 52 bytes
    {
    uint32_t name_off;              ///< into STRT payload, or NONE32
    uint16_t parent;                ///< node index, or NONE16 for a root
    uint16_t flags;
    uint16_t mesh_index;            ///< into MESH, or NONE16
    uint16_t skin_index;            ///< into SKIN, or NONE16
    float    translation[3];
    float    rotation[4];           ///< quaternion, xyzw, unit length
    float    scale[3];
    };


// ---------------------------------------------------------------------------
// SKEL - bones. A bone is animated through its NODE; this chunk only adds the
// bind-pose information the skinning step needs.
// Payload: uint32_t bone_count, uint32_t reserved, then BoneEntry[bone_count].
// ---------------------------------------------------------------------------

struct BoneEntry                    // 72 bytes
    {
    uint32_t name_off;
    uint16_t node_index;            ///< into NODE
    uint8_t  parent;                ///< bone index, or NONE8 for a root
    uint8_t  _pad;
    float    inv_bind[16];          ///< inverse bind matrix, COLUMN-MAJOR
    };

/// Bone indices are uint8_t in SkinVertex, so a skeleton is capped at 255 bones.
constexpr int MAX_BONES = 255;

/// Decided in review: 2 influences per vertex. Halves both cost and data.
constexpr int MAX_BONES_PER_VERTEX = 2;


// ---------------------------------------------------------------------------
// MESH - generic, quantized, skinning-capable geometry.
// Payload: uint32_t mesh_count, then MeshEntry[mesh_count], then blobs.
//
// Decode (must match the exporter bit for bit):
//   center  = 0.5 * (bbox_min + bbox_max)
//   extent  = max(bbox_max - bbox_min over x,y,z)
//   pscale  = extent / 32767
//   position = center + int16 q * pscale
//   normal   = int16 q * (1 / 32767)
//   texcoord = int16 q * (4 / 32767)          // approx [-4, 4], allows tiling
// ---------------------------------------------------------------------------

struct MeshEntry                    // 56 bytes
    {
    uint32_t name_off;
    uint32_t vertex_count;
    uint32_t triangle_count;
    uint32_t positions_off;         ///< int16[3 * vertex_count]
    uint32_t normals_off;           ///< int16[3 * vertex_count], or NONE32
    uint32_t texcoords_off;         ///< int16[2 * vertex_count], or NONE32
    uint32_t indices_off;           ///< uint16[3 * triangle_count]
    uint16_t material_index;        ///< into MATL, or NONE16
    uint16_t flags;
    float    bbox_min[3];
    float    bbox_max[3];
    };


// ---------------------------------------------------------------------------
// MSHC - cooked mesh: a backend-optimized blob the core never interprets.
//
// This is how the offline NP-hard work (triangle-chain stripification via
// LKH/GA-EAX, meshlet construction, visibility cones) survives into the
// runtime without the core knowing anything about the renderer that made it.
//
// A file MAY contain both MESH and MSHC for the same logical mesh; a backend
// that does not recognise backend_id falls back to the generic MESH.
//
// Payload: uint32_t cooked_count, then CookedMeshEntry[cooked_count], then blobs.
// ---------------------------------------------------------------------------

struct CookedMeshEntry              // 16 bytes
    {
    uint32_t backend_id;            ///< which renderer this blob was built for
    uint32_t mesh_index;            ///< logical mesh in MESH, or NONE32
    uint32_t blob_off;              ///< multiple of 4
    uint32_t blob_size;
    };


// ---------------------------------------------------------------------------
// SKIN - per-vertex bone binding.
// Payload: uint32_t skin_count, then SkinEntry[skin_count], then blobs.
// ---------------------------------------------------------------------------

struct SkinVertex                   // 4 bytes
    {
    uint8_t bone0;                  ///< bone index in the referenced SKEL
    uint8_t bone1;
    uint8_t weight0;                ///< weight1 is implicitly 255 - weight0
    uint8_t _pad;
    };

struct SkinEntry                    // 16 bytes
    {
    uint32_t mesh_index;            ///< the MESH this binds
    uint32_t skeleton_index;        ///< the SKEL this binds against
    uint32_t bindings_off;          ///< SkinVertex[mesh.vertex_count]
    uint32_t reserved;
    };


// ---------------------------------------------------------------------------
// MATL / TEXR
// ---------------------------------------------------------------------------

struct MaterialEntry                // 32 bytes
    {
    uint32_t name_off;
    float    color[3];
    float    ambient;
    float    diffuse;
    float    specular;
    uint16_t specular_exponent;
    uint16_t texture_index;         ///< into TEXR, or NONE16
    };

enum TexelFormat : uint16_t
    {
    TEXEL_RGB565 = 0,
    TEXEL_RGB24  = 1,
    TEXEL_RGB32  = 2,
    };

enum TextureFlags : uint16_t
    {
    TEXFLAG_POW2 = 1u << 0,         ///< both dimensions are powers of two -> wrap-capable
    };

struct TextureEntry                 // 20 bytes
    {
    uint32_t name_off;
    uint16_t width;
    uint16_t height;
    uint16_t format;                ///< TexelFormat
    uint16_t flags;                 ///< TextureFlags
    uint32_t pixels_off;            ///< multiple of 4
    uint32_t pixels_size;
    };


// ---------------------------------------------------------------------------
// ANIM - clips and channels.
//
// Times are uint16 fractions of the clip duration: t = key/65535 * duration_ms.
// Rotations are int16 snorm quaternions (xyzw), value = q / 32767.
// Translation and scale are uint16 lerped between the per-channel vmin/vmax.
// ---------------------------------------------------------------------------

enum AnimPath : uint8_t
    {
    ANIM_TRANSLATION = 0,
    ANIM_ROTATION    = 1,
    ANIM_SCALE       = 2,
    };

enum AnimInterp : uint8_t
    {
    ANIM_STEP   = 0,
    ANIM_LINEAR = 1,
    };

enum ClipFlags : uint16_t
    {
    CLIP_LOOP = 1u << 0,
    };

struct ChannelEntry                 // 48 bytes
    {
    uint16_t target_node;           ///< into NODE. Bones animate through their node.
    uint8_t  path;                  ///< AnimPath
    uint8_t  interp;                ///< AnimInterp
    uint16_t key_count;
    uint16_t _pad;
    uint32_t times_off;             ///< uint16[key_count]
    uint32_t values_off;            ///< int16[4*n] for rotation, uint16[3*n] otherwise
    float    vmin[3];               ///< translation/scale decode range (unused for rotation)
    float    vmax[3];
    uint32_t reserved[2];
    };

struct ClipEntry                    // 24 bytes
    {
    uint32_t name_off;
    uint32_t duration_ms;
    uint32_t channel_count;
    uint32_t channels_off;          ///< ChannelEntry[channel_count]
    uint16_t flags;                 ///< ClipFlags
    uint16_t _pad;
    uint32_t reserved;
    };


// ---------------------------------------------------------------------------
// Layout guarantees. A silent size change here would corrupt every asset file,
// so make it a compile error instead.
// ---------------------------------------------------------------------------

static_assert(sizeof(FileHeader)      == 32, "FileHeader must be 32 bytes");
static_assert(sizeof(ChunkEntry)      == 16, "ChunkEntry must be 16 bytes");
static_assert(sizeof(NodeEntry)       == 52, "NodeEntry must be 52 bytes");
static_assert(sizeof(BoneEntry)       == 72, "BoneEntry must be 72 bytes");
static_assert(sizeof(MeshEntry)       == 56, "MeshEntry must be 56 bytes");
static_assert(sizeof(CookedMeshEntry) == 16, "CookedMeshEntry must be 16 bytes");
static_assert(sizeof(SkinVertex)      ==  4, "SkinVertex must be 4 bytes");
static_assert(sizeof(SkinEntry)       == 16, "SkinEntry must be 16 bytes");
static_assert(sizeof(MaterialEntry)   == 32, "MaterialEntry must be 32 bytes");
static_assert(sizeof(TextureEntry)    == 20, "TextureEntry must be 20 bytes");
static_assert(sizeof(ChannelEntry)    == 48, "ChannelEntry must be 48 bytes");
static_assert(sizeof(ClipEntry)       == 24, "ClipEntry must be 24 bytes");

static_assert(alignof(FileHeader) <= 4 && alignof(NodeEntry)  <= 4 &&
              alignof(BoneEntry)  <= 4 && alignof(MeshEntry)  <= 4 &&
              alignof(ChannelEntry) <= 4,
              "all format structs must be 4-byte alignable");

} // namespace fmt
} // namespace a3d

#endif // A3D_FORMAT_H_
