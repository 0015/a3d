// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_freertos_executor.h
 * @brief FreeRTOS / ESP-IDF executor for ESP32-P4 and ESP32-S3.
 *
 * ============================================================================
 *  STATUS: NOT YET RUN ON HARDWARE.
 *
 *  The logic mirrors the std::thread executor, which IS covered by host tests,
 *  but nothing here has executed on a P4 or S3. Validate it in Phase 0 on real
 *  boards before relying on any timing claim.
 * ============================================================================
 *
 * Design: symmetric workers pulling part indices from a shared counter, with
 * two counting semaphores for start/done. No task-notification index is used,
 * so this cannot collide with other libraries using notification slot 0.
 */
#ifndef A3D_FREERTOS_EXECUTOR_H_
#define A3D_FREERTOS_EXECUTOR_H_

#include "a3d/a3d_jobs.h"

#if defined(ESP_PLATFORM)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <atomic>

namespace a3d {

class FreeRtosExecutor final : public IJobExecutor
    {
    public:

        /**
         * @param nbWorkers   total workers INCLUDING the calling task. 1 or 2 on
         *                    P4/S3. Values above A3D_MAX_WORKERS are clamped.
         * @param stackSize   stack for each spawned worker task.
         * @param priority    priority for each spawned worker task.
         *
         * Worker i is pinned to core (i + 1) % portNUM_PROCESSORS so that the
         * calling task keeps its own core.
         */
        explicit FreeRtosExecutor(int nbWorkers,
                                  uint32_t stackSize = 4096,
                                  UBaseType_t priority = 5)
            {
            // Degrading used to be silent, which meant a caller could ask for two
            // workers, get one, and never find out. That happened: the visual
            // demo ran single-threaded for several device rounds before anyone
            // noticed. Every path that reduces the worker count now says so.
            const int requested = nbWorkers;
            if (nbWorkers < 1) nbWorkers = 1;
            if (nbWorkers > A3D_MAX_WORKERS)
                {
                ESP_LOGW("a3d_exec", "requested %d workers, capped to A3D_MAX_WORKERS=%d "
                                     "(board=%s). Is sdkconfig.h reaching a3d_board.h?",
                         requested, (int)A3D_MAX_WORKERS, A3D_BOARD_NAME);
                nbWorkers = A3D_MAX_WORKERS;
                }
            _nbWorkers = nbWorkers;

            const int nbTasks = _nbWorkers - 1;
            if (nbTasks <= 0)
                {
                if (requested > 1)
                    ESP_LOGW("a3d_exec", "running single-threaded: %d worker(s) after "
                                         "capping, %d requested", _nbWorkers, requested);
                return;
                }

            _startSem = xSemaphoreCreateCounting((UBaseType_t)nbTasks, 0);
            _doneSem  = xSemaphoreCreateCounting((UBaseType_t)nbTasks, 0);
            if ((_startSem == nullptr) || (_doneSem == nullptr))
                {
                ESP_LOGE("a3d_exec", "semaphore creation failed; running single-threaded");
                _degrade(); return;
                }

            for (int i = 0; i < nbTasks; i++)
                {
                const BaseType_t core = (BaseType_t)((i + 1) % portNUM_PROCESSORS);
                const BaseType_t rc = xTaskCreatePinnedToCore(
                    &FreeRtosExecutor::_trampoline, "a3d_job",
                    stackSize, this, priority, &_tasks[i], core);
                if (rc != pdPASS)
                    {
                    ESP_LOGE("a3d_exec", "xTaskCreatePinnedToCore(core=%d, stack=%u, prio=%u) "
                                         "returned %d; running single-threaded",
                             (int)core, (unsigned)stackSize, (unsigned)priority, (int)rc);
                    _degrade(); return;
                    }
                _nbTasks++;
                }
            ESP_LOGI("a3d_exec", "%d workers, %d helper task(s) pinned, portNUM_PROCESSORS=%d",
                     _nbWorkers, _nbTasks, (int)portNUM_PROCESSORS);
            }

        ~FreeRtosExecutor() override
            {
            _stop.store(true, std::memory_order_release);
            for (int i = 0; i < _nbTasks; i++) xSemaphoreGive(_startSem);
            // Workers exit on their own; give them a tick to unwind.
            vTaskDelay(1);
            if (_startSem) vSemaphoreDelete(_startSem);
            if (_doneSem)  vSemaphoreDelete(_doneSem);
            }

        FreeRtosExecutor(const FreeRtosExecutor&) = delete;
        FreeRtosExecutor& operator=(const FreeRtosExecutor&) = delete;

        using IJobExecutor::parallelFor;   // keep the 3-argument convenience overload visible

        int workerCount() const override { return _nbWorkers; }

        void parallelFor(int nbItems, JobFn fn, void* ctx, int minChunk) override
            {
            if ((fn == nullptr) || (nbItems <= 0)) return;

            const int parts = planParts(nbItems, _nbWorkers, minChunk);
            if ((parts <= 1) || (_nbTasks <= 0))
                {
                fn(ctx, 0, nbItems);
                return;
                }

            _fn = fn; _ctx = ctx; _nbItems = nbItems; _parts = parts;
            _nextPart.store(1, std::memory_order_release);   // part 0 is the caller's

            const int helpers = parts - 1;
            for (int i = 0; i < helpers; i++) xSemaphoreGive(_startSem);

            int b, e;
            splitRange(nbItems, parts, 0, b, e);
            if (e > b) fn(ctx, b, e);

            for (int i = 0; i < helpers; i++) xSemaphoreTake(_doneSem, portMAX_DELAY);
            }

    private:

        static void _trampoline(void* self)
            {
            static_cast<FreeRtosExecutor*>(self)->_workerLoop();
            }

        void _workerLoop()
            {
            for (;;)
                {
                xSemaphoreTake(_startSem, portMAX_DELAY);
                if (_stop.load(std::memory_order_acquire)) break;

                const int myPart = _nextPart.fetch_add(1, std::memory_order_acq_rel);
                if (myPart < _parts)
                    {
                    int b, e;
                    splitRange(_nbItems, _parts, myPart, b, e);
                    if (e > b) _fn(_ctx, b, e);
                    }
                xSemaphoreGive(_doneSem);
                }
            vTaskDelete(nullptr);
            }

        /** Fall back to single-threaded operation if setup failed. */
        void _degrade() { _nbWorkers = 1; }

        int              _nbWorkers = 1;
        int              _nbTasks   = 0;
        TaskHandle_t     _tasks[A3D_MAX_WORKERS > 1 ? A3D_MAX_WORKERS - 1 : 1] = {};
        SemaphoreHandle_t _startSem = nullptr;
        SemaphoreHandle_t _doneSem  = nullptr;

        JobFn            _fn      = nullptr;
        void*            _ctx     = nullptr;
        int              _nbItems = 0;
        int              _parts   = 0;
        std::atomic<int>  _nextPart{0};
        std::atomic<bool> _stop{false};
    };

} // namespace a3d

#endif // ESP_PLATFORM

#endif // A3D_FREERTOS_EXECUTOR_H_
