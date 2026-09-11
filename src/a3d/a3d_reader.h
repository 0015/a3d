// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_reader.h
 * @brief Read-only view over an a3d container image.
 *
 * Holds no ownership and copies nothing: every accessor is `base + offset`.
 * That is the whole point of the offset-only format - the same code path serves
 * an image mapped in flash (XIP, zero RAM) and one read into RAM from SD.
 *
 * Every accessor is bounds-checked against the image, so a truncated or
 * corrupt file yields nullptr rather than a wild pointer.
 */
#ifndef A3D_READER_H_
#define A3D_READER_H_

#include "a3d_format.h"

#include <stddef.h>
#include <string.h>

namespace a3d {

class AssetImage
    {
    public:

        AssetImage() = default;

        /**
         * Validate and attach to an image. Returns false and leaves the object
         * unusable if the header, byte order, size or chunk table is wrong.
         */
        bool open(const void* data, size_t size)
            {
            _base = nullptr; _size = 0; _strt = nullptr; _strtSize = 0;

            if ((data == nullptr) || (size < sizeof(fmt::FileHeader))) return false;
            const uint8_t* p = (const uint8_t*)data;
            const fmt::FileHeader* h = (const fmt::FileHeader*)p;

            if (h->magic != fmt::MAGIC) return false;
            if (h->byte_order != fmt::BYTE_ORDER_OK) return false;   // wrong endianness
            if (h->version_major > fmt::VERSION_MAJOR) return false; // breaking change
            if (h->file_size != (uint32_t)size) return false;
            if ((h->chunk_table_off % 4) != 0) return false;

            const uint64_t tableEnd = (uint64_t)h->chunk_table_off
                                    + (uint64_t)h->chunk_count * sizeof(fmt::ChunkEntry);
            if (tableEnd > size) return false;

            const fmt::ChunkEntry* tab = (const fmt::ChunkEntry*)(p + h->chunk_table_off);
            for (uint32_t i = 0; i < h->chunk_count; i++)
                {
                if ((tab[i].offset % 4) != 0) return false;
                if ((uint64_t)tab[i].offset + tab[i].size > size) return false;
                }

            _base = p; _size = size; _header = h; _chunks = tab;

            const fmt::ChunkEntry* s = chunk(fmt::CHUNK_STRT);
            if (s != nullptr) { _strt = _base + s->offset; _strtSize = s->size; }
            return true;
            }

        bool valid() const { return _base != nullptr; }
        uint16_t versionMajor() const { return _header->version_major; }
        uint16_t versionMinor() const { return _header->version_minor; }
        uint32_t chunkCount() const { return _header->chunk_count; }

        /** Find a chunk by FourCC, or nullptr. Unknown types are simply absent. */
        const fmt::ChunkEntry* chunk(uint32_t type) const
            {
            for (uint32_t i = 0; i < _header->chunk_count; i++)
                if (_chunks[i].type == type) return &_chunks[i];
            return nullptr;
            }

        /** Typed view at a file-absolute offset, bounds-checked for `count` items. */
        template <typename T>
        const T* at(uint32_t offset, uint32_t count = 1) const
            {
            if (offset == fmt::NONE32) return nullptr;
            if ((uint64_t)offset + (uint64_t)count * sizeof(T) > _size) return nullptr;
            return (const T*)(_base + offset);
            }

        /** Resolve a string-table offset. Returns nullptr for NONE32. */
        const char* string(uint32_t off) const
            {
            if ((off == fmt::NONE32) || (_strt == nullptr) || (off >= _strtSize))
                return nullptr;
            return (const char*)(_strt + off);
            }

        // ------------------------------------------------------------------
        // Chunk accessors. Each returns 0 / nullptr when the chunk is absent,
        // so a caller never has to test for chunk presence separately.
        // ------------------------------------------------------------------

        uint32_t nodeCount() const { return _count(fmt::CHUNK_NODE); }
        const fmt::NodeEntry* nodes() const
            { return _array<fmt::NodeEntry>(fmt::CHUNK_NODE, 4); }

        uint32_t boneCount() const { return _count(fmt::CHUNK_SKEL); }
        const fmt::BoneEntry* bones() const
            { return _array<fmt::BoneEntry>(fmt::CHUNK_SKEL, 8); }   // count + reserved

        uint32_t meshCount() const { return _count(fmt::CHUNK_MESH); }
        const fmt::MeshEntry* meshes() const
            { return _array<fmt::MeshEntry>(fmt::CHUNK_MESH, 4); }

        uint32_t cookedCount() const { return _count(fmt::CHUNK_MSHC); }
        const fmt::CookedMeshEntry* cooked() const
            { return _array<fmt::CookedMeshEntry>(fmt::CHUNK_MSHC, 4); }

        uint32_t skinCount() const { return _count(fmt::CHUNK_SKIN); }
        const fmt::SkinEntry* skins() const
            { return _array<fmt::SkinEntry>(fmt::CHUNK_SKIN, 4); }

        uint32_t materialCount() const { return _count(fmt::CHUNK_MATL); }
        const fmt::MaterialEntry* materials() const
            { return _array<fmt::MaterialEntry>(fmt::CHUNK_MATL, 4); }

        uint32_t textureCount() const { return _count(fmt::CHUNK_TEXR); }
        const fmt::TextureEntry* textures() const
            { return _array<fmt::TextureEntry>(fmt::CHUNK_TEXR, 4); }

        uint32_t clipCount() const { return _count(fmt::CHUNK_ANIM); }
        const fmt::ClipEntry* clips() const
            { return _array<fmt::ClipEntry>(fmt::CHUNK_ANIM, 4); }

        /** Find the cooked blob for `meshIndex` matching `backendId`, or nullptr. */
        const void* cookedBlob(uint32_t meshIndex, uint32_t backendId, uint32_t& sizeOut) const
            {
            sizeOut = 0;
            const fmt::CookedMeshEntry* ck = cooked();
            const uint32_t n = cookedCount();
            for (uint32_t i = 0; (ck != nullptr) && (i < n); i++)
                {
                if ((ck[i].backend_id != backendId) || (ck[i].mesh_index != meshIndex))
                    continue;
                const void* p = at<uint8_t>(ck[i].blob_off, ck[i].blob_size);
                if (p != nullptr) { sizeOut = ck[i].blob_size; return p; }
                }
            return nullptr;
            }

        // ------------------------------------------------------------------
        // Dequantization. These MUST match a3d_scene.py and the formulas in
        // docs/ASSET_FORMAT.md exactly; the host test asserts that they do.
        // ------------------------------------------------------------------

        struct MeshQuant
            {
            float center[3];
            float pscale;
            };

        static MeshQuant meshQuant(const fmt::MeshEntry& m)
            {
            MeshQuant q;
            float extent = 0.0f;
            for (int k = 0; k < 3; k++)
                {
                q.center[k] = 0.5f * (m.bbox_min[k] + m.bbox_max[k]);
                const float d = m.bbox_max[k] - m.bbox_min[k];
                if (d > extent) extent = d;
                }
            if (extent <= 0.0f) extent = 1.0f;
            q.pscale = extent / 32767.0f;
            return q;
            }

        static void decodePosition(const MeshQuant& q, const int16_t* p, float out[3])
            { for (int k = 0; k < 3; k++) out[k] = q.center[k] + (float)p[k] * q.pscale; }

        static void decodeNormal(const int16_t* n, float out[3])
            { for (int k = 0; k < 3; k++) out[k] = (float)n[k] * (1.0f / 32767.0f); }

        static void decodeTexcoord(const int16_t* t, float out[2])
            { for (int k = 0; k < 2; k++) out[k] = (float)t[k] * (4.0f / 32767.0f); }

        static float decodeKeyTimeMs(uint16_t t, uint32_t durationMs)
            { return ((float)t / 65535.0f) * (float)durationMs; }

        static void decodeRotationKey(const int16_t* q, float out[4])
            { for (int k = 0; k < 4; k++) out[k] = (float)q[k] * (1.0f / 32767.0f); }

        static void decodeVec3Key(const fmt::ChannelEntry& ch, const uint16_t* v, float out[3])
            {
            for (int k = 0; k < 3; k++)
                {
                const float span = ch.vmax[k] - ch.vmin[k];
                out[k] = ch.vmin[k] + ((float)v[k] / 65535.0f) * span;
                }
            }

    private:

        /** Chunks that hold a count start with a uint32 count at their base. */
        uint32_t _count(uint32_t type) const
            {
            const fmt::ChunkEntry* c = chunk(type);
            if ((c == nullptr) || (c->size < 4)) return 0;
            uint32_t n;
            memcpy(&n, _base + c->offset, 4);
            return n;
            }

        template <typename T>
        const T* _array(uint32_t type, uint32_t headerBytes) const
            {
            const fmt::ChunkEntry* c = chunk(type);
            if (c == nullptr) return nullptr;
            const uint32_t n = _count(type);
            if (n == 0) return nullptr;
            if ((uint64_t)headerBytes + (uint64_t)n * sizeof(T) > c->size) return nullptr;
            return (const T*)(_base + c->offset + headerBytes);
            }

        const uint8_t*              _base = nullptr;
        size_t                      _size = 0;
        const fmt::FileHeader*      _header = nullptr;
        const fmt::ChunkEntry*      _chunks = nullptr;
        const uint8_t*              _strt = nullptr;
        uint32_t                    _strtSize = 0;
    };

} // namespace a3d

#endif // A3D_READER_H_
