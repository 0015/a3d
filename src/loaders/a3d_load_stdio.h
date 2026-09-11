// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_load_stdio.h
 * @brief Load a container into RAM through stdio.
 *
 * One implementation covers the host AND the MCU: ESP-IDF exposes SD cards
 * (FATFS) and SPIFFS through the same stdio calls, so an asset streamed from an
 * SD card needs no separate code path.
 *
 * This is the RAM-resident path: the whole image is read in. For assets already
 * in flash prefer `a3d_load_esp_partition.h`, which costs no RAM at all.
 */
#ifndef A3D_LOAD_STDIO_H_
#define A3D_LOAD_STDIO_H_

#include "a3d/a3d_reader.h"

#include <stdio.h>
#include <stdlib.h>

namespace a3d {

class StdioImage
    {
    public:

        StdioImage() = default;
        ~StdioImage() { release(); }

        StdioImage(const StdioImage&) = delete;
        StdioImage& operator=(const StdioImage&) = delete;

        /**
         * @param path   file to read.
         * @param alloc  optional allocator, so a caller can place the image in a
         *               specific memory kind (PSRAM on ESP32, for example).
         *               Phase 0 measured PSRAM as free below ~3,000 vertices and
         *               2x slower above, so where this lands is a real decision.
         */
        bool load(const char* path,
                  void* (*alloc)(size_t) = nullptr,
                  void  (*dealloc)(void*) = nullptr)
            {
            release();
            if (path == nullptr) return false;

            FILE* f = fopen(path, "rb");
            if (f == nullptr) return false;

            if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
            const long sz = ftell(f);
            if ((sz <= 0) || (fseek(f, 0, SEEK_SET) != 0)) { fclose(f); return false; }

            _alloc = alloc; _dealloc = dealloc;
            _data = (unsigned char*)(alloc ? alloc((size_t)sz) : malloc((size_t)sz));
            if (_data == nullptr) { fclose(f); return false; }

            const size_t got = fread(_data, 1, (size_t)sz, f);
            fclose(f);
            if (got != (size_t)sz) { release(); return false; }

            if (!_image.open(_data, (size_t)sz)) { release(); return false; }
            _size = (size_t)sz;
            return true;
            }

        void release()
            {
            if (_data != nullptr)
                {
                if (_dealloc) _dealloc(_data); else free(_data);
                _data = nullptr;
                }
            _size = 0;
            _image = AssetImage();
            }

        const AssetImage& image() const { return _image; }
        size_t size() const { return _size; }
        bool valid() const { return _image.valid(); }

    private:

        AssetImage     _image;
        unsigned char* _data = nullptr;
        size_t         _size = 0;
        void* (*_alloc)(size_t) = nullptr;
        void  (*_dealloc)(void*) = nullptr;
    };

} // namespace a3d

#endif // A3D_LOAD_STDIO_H_
