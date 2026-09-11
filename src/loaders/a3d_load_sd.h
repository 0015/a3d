// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_load_sd.h
 * @brief Mount an SD card and find the assets on it. ESP-IDF only.
 *
 * Reading is NOT here. `a3d_load_stdio.h` already reads a container through
 * stdio, and ESP-IDF exposes FATFS through the same calls, so an asset on an SD
 * card needs no separate read path - it needs a card that is mounted and a way
 * to discover what is on it. That is what this adds, and it is the whole of the
 * difference between an asset chosen at build time and one chosen at run time.
 *
 * -------------
 *     a3d::SdCard sd;
 *     if (!sd.mount()) { ... }
 *
 *     char names[16][64];
 *     const int n = sd.listAssets(names, 16, 64);
 *
 *     char path[128];
 *     sd.pathFor(names[0], path, sizeof(path));
 *     a3d::StdioImage img;
 *     img.load(path, psramAlloc, psramFree);
 *
 * Two things that are easy to get wrong on an ESP32-P4
 * ----------------------------------------------------
 * 1. **The card is not powered until an LDO is told to power it.** On P4 the
 *    SDMMC IO rail comes from an internal LDO (`SOC_SDMMC_IO_POWER_EXTERNAL`),
 *    and without `sd_pwr_ctrl_new_on_chip_ldo()` first the mount fails with a
 *    timeout that reads exactly like a missing or faulty card. The channel is
 *    board wiring, not a chip constant - 4 on the Waveshare P4 boards.
 *
 * 2. **Bus width 4 needs four data pins AND external pull-ups.** The internal
 *    pull-ups are documented by Espressif as insufficient. If a 4-bit mount is
 *    unreliable on a board, drop to `width = 1` before suspecting the card.
 *
 * Pins default to the ESP32-P4 assignment used by the vendor `03_sdmmc`
 * example. Override any of them from the build if a board differs.
 */
#ifndef A3D_LOAD_SD_H_
#define A3D_LOAD_SD_H_

// BUILD REQUIREMENT, and it is not a3d's to satisfy: this header pulls in the
// SD host driver and FATFS. The a3d component deliberately does not REQUIRE
// them - a user who only wants a rasterizer should not link a filesystem - so
// the component that includes THIS file must add them to its own:
//
//     idf_component_register(... REQUIRES a3d sdmmc fatfs esp_driver_sdmmc)
//
// TARGET REQUIREMENT: this file uses the SDMMC host (`SDMMC_HOST_DEFAULT`,
// `sdmmc_slot_config_t`), which not every part has. It compiles on ESP32-P4 and
// ESP32-S3; on ESP32-C6, C3 and H2 there is no SDMMC peripheral and the
// includes resolve while the symbols do not. Those parts need an SDSPI path,
// which this header does not have.
//
#include "a3d/a3d_reader.h"

#if defined(ESP_PLATFORM)

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#ifndef A3D_SD_MOUNT_POINT
#define A3D_SD_MOUNT_POINT "/sdcard"
#endif

#ifndef A3D_SD_PIN_CLK
#define A3D_SD_PIN_CLK 43
#endif
#ifndef A3D_SD_PIN_CMD
#define A3D_SD_PIN_CMD 44
#endif
#ifndef A3D_SD_PIN_D0
#define A3D_SD_PIN_D0 39
#endif
#ifndef A3D_SD_PIN_D1
#define A3D_SD_PIN_D1 40
#endif
#ifndef A3D_SD_PIN_D2
#define A3D_SD_PIN_D2 41
#endif
#ifndef A3D_SD_PIN_D3
#define A3D_SD_PIN_D3 42
#endif

/** 4 is the fast path; 1 needs only D0 and survives weak pull-ups. */
#ifndef A3D_SD_BUS_WIDTH
#define A3D_SD_BUS_WIDTH 4
#endif

/** Internal LDO channel feeding the SDMMC IO rail. Board wiring. */
#ifndef A3D_SD_LDO_CHANNEL
#define A3D_SD_LDO_CHANNEL 4
#endif

/**
 * Bus clock in kHz.
 *
 * Left at SDMMC_FREQ_DEFAULT (20 MHz) because SDMMC_FREQ_HIGHSPEED (40 MHz) was
 * measured and is not worth it here: reading a 1.5 MB file went from 0.45 to
 * 0.49 MB/s, about 7%, while the card's SCR read started failing CRC often
 * enough to abort the mount outright. Destination memory (PSRAM against
 * internal) and read chunk size (4 KB against 64 KB) made NO difference at all -
 * six combinations came out within one millisecond of each other - so the limit
 * on this path is the card, not the bus clock or anything above it.
 *
 * Raise it only with a card and wiring that have been measured to hold it.
 */
#ifndef A3D_SD_FREQ_KHZ
#define A3D_SD_FREQ_KHZ SDMMC_FREQ_DEFAULT
#endif

namespace a3d {

/**
 * A mounted SD card, and the assets on it.
 *
 * Owns the mount for its lifetime and releases it in the destructor, because a
 * card left mounted across a re-init is a second mount of the same volume and
 * fails in a way that looks like the card disappeared.
 */
class SdCard
    {
    public:

        SdCard() = default;
        ~SdCard() { unmount(); }

        SdCard(const SdCard&) = delete;
        SdCard& operator=(const SdCard&) = delete;

        /**
         * @param formatIfMountFailed  format a card FATFS cannot read. Default
         *        false: a demo that reformats the user's card on a bad contact
         *        is worse than one that says it found nothing.
         */
        bool mount(bool formatIfMountFailed = false)
            {
            if (_card != nullptr) return true;

#if SOC_SDMMC_IO_POWER_EXTERNAL
            // Before the host, not after: the bus cannot be probed while the
            // rail it lives on is dark.
            sd_pwr_ctrl_ldo_config_t ldo = {};
            ldo.ldo_chan_id = A3D_SD_LDO_CHANNEL;
            _err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &_pwr);
            if (_err != ESP_OK) { _pwr = nullptr; return false; }
#endif

            sdmmc_host_t host = SDMMC_HOST_DEFAULT();
            host.max_freq_khz = A3D_SD_FREQ_KHZ;
#if SOC_SDMMC_IO_POWER_EXTERNAL
            host.pwr_ctrl_handle = _pwr;
#endif

            sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
            slot.width = A3D_SD_BUS_WIDTH;
#ifdef SOC_SDMMC_USE_GPIO_MATRIX
            slot.clk = (gpio_num_t)A3D_SD_PIN_CLK;
            slot.cmd = (gpio_num_t)A3D_SD_PIN_CMD;
            slot.d0  = (gpio_num_t)A3D_SD_PIN_D0;
#if A3D_SD_BUS_WIDTH == 4
            slot.d1  = (gpio_num_t)A3D_SD_PIN_D1;
            slot.d2  = (gpio_num_t)A3D_SD_PIN_D2;
            slot.d3  = (gpio_num_t)A3D_SD_PIN_D3;
#endif
#endif
            slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

            esp_vfs_fat_sdmmc_mount_config_t cfg = {};
            cfg.format_if_mount_failed = formatIfMountFailed;
            cfg.max_files = 4;
            cfg.allocation_unit_size = 16 * 1024;

            // The error is kept rather than collapsed into false. "It did not
            // mount" is not actionable; ESP_ERR_TIMEOUT (no card, or an unpowered
            // rail), ESP_ERR_INVALID_RESPONSE (wiring) and ESP_FAIL (mounted but
            // not FAT) each send you somewhere different.
            // Retried, because the failure that actually happens here is
            // transient. Reading the card's SCR register - the one that says
            // whether it supports a 4-bit bus - comes back ESP_ERR_INVALID_CRC
            // on some boots and not others, and a CRC error on the data lines
            // aborts the whole mount rather than degrading to 1-bit. Espressif
            // document the internal pullups as insufficient and ask for 10k
            // external ones; where those are not present this is what it looks
            // like from software, and a second attempt usually succeeds.
            for (int attempt = 0; attempt < 3; attempt++)
                {
                _err = esp_vfs_fat_sdmmc_mount(A3D_SD_MOUNT_POINT, &host, &slot,
                                               &cfg, &_card);
                if (_err == ESP_OK) return true;
                _card = nullptr;
                }
            _releasePower();
            return false;
            }

        void unmount()
            {
            if (_card != nullptr)
                {
                esp_vfs_fat_sdcard_unmount(A3D_SD_MOUNT_POINT, _card);
                _card = nullptr;
                }
            _releasePower();
            }

        bool mounted() const { return _card != nullptr; }

        /** Clock the card actually negotiated, in kHz. 0 when not mounted. */
        int clockKHz() const { return (_card != nullptr) ? _card->max_freq_khz : 0; }

        /** Why the last mount() failed. ESP_OK when it did not. */
        esp_err_t lastError() const { return _err; }
        const sdmmc_card_t* card() const { return _card; }
        static const char* mountPoint() { return A3D_SD_MOUNT_POINT; }

        /** Capacity in MiB, 0 when not mounted. */
        uint32_t capacityMiB() const
            {
            if (_card == nullptr) return 0;
            return (uint32_t)(((uint64_t)_card->csd.capacity *
                               (uint64_t)_card->csd.sector_size) >> 20);
            }

        /**
         * Names of the `.a3d` files in `subdir` (null or "" means the root).
         *
         * Names only, not paths: an MCU menu wants the short form to show, and
         * `pathFor()` builds the long one when a file is actually opened. The
         * caller owns the storage so nothing here allocates.
         *
         * @return how many were written, or -1 if the directory could not be read.
         */
        int listAssets(char (*names)[64], int maxNames, int nameCap = 64,
                       const char* subdir = nullptr) const
            {
            if ((names == nullptr) || (maxNames <= 0) || !mounted()) return -1;

            char dirPath[128];
            _dirPath(subdir, dirPath, sizeof(dirPath));

            DIR* d = opendir(dirPath);
            if (d == nullptr) return -1;

            int n = 0;
            struct dirent* e;
            while (((e = readdir(d)) != nullptr) && (n < maxNames))
                {
                if (!_hasA3dSuffix(e->d_name)) continue;
                // A name that does not fit is SKIPPED, not truncated. A
                // truncated name still looks like a file in a menu, and then
                // pathFor() builds a path to something that is not there and
                // the load fails for a reason the screen cannot explain.
                // Whether names arrive long or in 8.3 form is a FATFS build
                // option (CONFIG_FATFS_LFN_*), not something this can fix.
                if (strlen(e->d_name) >= (size_t)nameCap) continue;
                strncpy(names[n], e->d_name, (size_t)nameCap - 1);
                names[n][nameCap - 1] = '\0';
                n++;
                }
            closedir(d);
            return n;
            }

        /** `<mount>/<subdir>/<name>` into `out`. False if it would not fit. */
        bool pathFor(const char* name, char* out, size_t cap,
                     const char* subdir = nullptr) const
            {
            if ((name == nullptr) || (out == nullptr) || (cap == 0)) return false;
            char dirPath[128];
            _dirPath(subdir, dirPath, sizeof(dirPath));
            const int wrote = snprintf(out, cap, "%s/%s", dirPath, name);
            return (wrote > 0) && ((size_t)wrote < cap);
            }

    private:

        void _releasePower()
            {
#if SOC_SDMMC_IO_POWER_EXTERNAL
            if (_pwr != nullptr) { sd_pwr_ctrl_del_on_chip_ldo(_pwr); _pwr = nullptr; }
#endif
            }

        static void _dirPath(const char* subdir, char* out, size_t cap)
            {
            if ((subdir == nullptr) || (subdir[0] == '\0'))
                snprintf(out, cap, "%s", A3D_SD_MOUNT_POINT);
            else
                snprintf(out, cap, "%s/%s", A3D_SD_MOUNT_POINT, subdir);
            }

        /** Case-insensitive `.a3d`, because FATFS reports 8.3 names uppercase. */
        static bool _hasA3dSuffix(const char* name)
            {
            if (name == nullptr) return false;
            const size_t n = strlen(name);
            if (n < 5) return false;                  // "x.a3d" is the shortest
            const char* s = name + (n - 4);
            return (s[0] == '.') &&
                   ((s[1] == 'a') || (s[1] == 'A')) &&
                   (s[2] == '3') &&
                   ((s[3] == 'd') || (s[3] == 'D'));
            }

        sdmmc_card_t* _card = nullptr;
        esp_err_t     _err = ESP_OK;
#if SOC_SDMMC_IO_POWER_EXTERNAL
        sd_pwr_ctrl_handle_t _pwr = nullptr;
#endif
    };

} // namespace a3d

#endif // ESP_PLATFORM
#endif // A3D_LOAD_SD_H_
