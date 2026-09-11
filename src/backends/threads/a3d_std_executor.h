// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_std_executor.h
 * @brief std::thread executor. Host development and unit tests.
 *
 * Uses a persistent worker pool: threads are created once, not per frame.
 */
#ifndef A3D_STD_EXECUTOR_H_
#define A3D_STD_EXECUTOR_H_

#include "a3d/a3d_jobs.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cstdint>

namespace a3d {

class StdThreadExecutor final : public IJobExecutor
    {
    public:

        /** @param nbWorkers total workers INCLUDING the calling thread. >= 1. */
        explicit StdThreadExecutor(int nbWorkers)
            : _nbWorkers(nbWorkers < 1 ? 1 : nbWorkers)
            {
            for (int i = 0; i < _nbWorkers - 1; i++)
                _threads.emplace_back([this, i]{ workerLoop(i); });
            }

        ~StdThreadExecutor() override
            {
                {
                std::lock_guard<std::mutex> lk(_m);
                _stop = true;
                _gen++;
                }
            _cvStart.notify_all();
            for (auto& t : _threads) if (t.joinable()) t.join();
            }

        StdThreadExecutor(const StdThreadExecutor&) = delete;
        StdThreadExecutor& operator=(const StdThreadExecutor&) = delete;

        using IJobExecutor::parallelFor;   // keep the 3-argument convenience overload visible

        int workerCount() const override { return _nbWorkers; }

        void parallelFor(int nbItems, JobFn fn, void* ctx, int minChunk) override
            {
            if ((fn == nullptr) || (nbItems <= 0)) return;

            const int parts = planParts(nbItems, _nbWorkers, minChunk);
            if ((parts <= 1) || _threads.empty())
                {
                fn(ctx, 0, nbItems);
                return;
                }

                {
                std::lock_guard<std::mutex> lk(_m);
                _fn = fn; _ctx = ctx; _nbItems = nbItems; _parts = parts;
                _pending = (int)_threads.size();   // every thread wakes and reports back
                _gen++;
                }
            _cvStart.notify_all();

            // The calling thread always takes part 0 - no thread sits idle.
            int b, e;
            splitRange(nbItems, parts, 0, b, e);
            if (e > b) fn(ctx, b, e);

                {
                std::unique_lock<std::mutex> lk(_m);
                _cvDone.wait(lk, [this]{ return _pending == 0; });
                }
            }

    private:

        void workerLoop(int index)
            {
            uint64_t seen = 0;
            for (;;)
                {
                JobFn fn; void* ctx; int nbItems, parts;
                    {
                    std::unique_lock<std::mutex> lk(_m);
                    _cvStart.wait(lk, [this, seen]{ return _stop || (_gen != seen); });
                    if (_stop) return;
                    seen = _gen;
                    fn = _fn; ctx = _ctx; nbItems = _nbItems; parts = _parts;
                    }

                const int myPart = index + 1;      // part 0 belongs to the caller
                if (myPart < parts)
                    {
                    int b, e;
                    splitRange(nbItems, parts, myPart, b, e);
                    if (e > b) fn(ctx, b, e);
                    }

                    {
                    std::lock_guard<std::mutex> lk(_m);
                    if (--_pending == 0) _cvDone.notify_one();
                    }
                }
            }

        int                      _nbWorkers;
        std::vector<std::thread> _threads;

        std::mutex               _m;
        std::condition_variable  _cvStart;
        std::condition_variable  _cvDone;

        JobFn    _fn      = nullptr;
        void*    _ctx     = nullptr;
        int      _nbItems = 0;
        int      _parts   = 0;
        uint64_t _gen     = 0;
        int      _pending = 0;
        bool     _stop    = false;
    };

} // namespace a3d

#endif // A3D_STD_EXECUTOR_H_
