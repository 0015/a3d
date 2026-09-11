// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_load_esp_partition.h
 * @brief Map a container straight out of a flash partition. ESP-IDF only.
 *
 * ============================================================================
 *  STATUS: NOT YET RUN ON HARDWARE.
 *  The reader itself is covered by host tests; only this mapping wrapper is
 *  unverified. Validate before relying on it.
 * ============================================================================
 *
 * This is the reason the container has no pointers. The image is used in place
 * at its flash address, so a mapped asset costs **zero RAM** - index buffers and
 * texture pixels are read directly from flash and never copied.
 *
 * Add a partition to your table, for example:
 *
 *     assets, data, 0x40, 0x210000, 1M,
 *
 * and write the .a3d file to it with:
 *
 *     esptool.py write_flash 0x210000 model.a3d
 */
#ifndef A3D_LOAD_ESP_PARTITION_H_
#define A3D_LOAD_ESP_PARTITION_H_

#include "a3d/a3d_reader.h"

#if defined(ESP_PLATFORM)

#include "esp_partition.h"

namespace a3d {

class PartitionImage
    {
    public:

        PartitionImage() = default;
        ~PartitionImage() { release(); }

        PartitionImage(const PartitionImage&) = delete;
        PartitionImage& operator=(const PartitionImage&) = delete;

        /**
         * Map the partition named `label` and validate the container in it.
         *
         * The container's own header carries `file_size`, so the whole partition
         * is mapped and then the image is opened against that declared size -
         * a partition is almost always larger than the asset written into it.
         */
        bool map(const char* label, esp_partition_subtype_t subtype =
                     (esp_partition_subtype_t)0x40)
            {
            release();
            const esp_partition_t* part = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA, subtype, label);
            if (part == nullptr) return false;

            const void* base = nullptr;
            if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                                   &base, &_handle) != ESP_OK)
                return false;
            _mapped = true;

            // Read the declared size from the header before trusting anything else.
            uint32_t declared = 0;
            if (part->size >= sizeof(fmt::FileHeader))
                declared = ((const fmt::FileHeader*)base)->file_size;
            if ((declared < sizeof(fmt::FileHeader)) || (declared > part->size))
                { release(); return false; }

            if (!_image.open(base, declared)) { release(); return false; }
            _size = declared;
            return true;
            }

        void release()
            {
            if (_mapped) { esp_partition_munmap(_handle); _mapped = false; }
            _size = 0;
            _image = AssetImage();
            }

        const AssetImage& image() const { return _image; }
        size_t size() const { return _size; }
        bool valid() const { return _image.valid(); }

    private:

        AssetImage                   _image;
        esp_partition_mmap_handle_t  _handle = 0;
        bool                         _mapped = false;
        size_t                       _size = 0;
    };

} // namespace a3d

#endif // ESP_PLATFORM

#endif // A3D_LOAD_ESP_PARTITION_H_
