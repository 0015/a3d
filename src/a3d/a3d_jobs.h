// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_jobs.h
 * @brief Configurable parallel execution. No RTOS or threading dependency here.
 *
 * WHY AN ABSTRACTION
 *
 *   Only some work is safely parallelizable, and the safe part lives entirely
 *   inside a3d:
 *
 *   - SKINNING is embarrassingly parallel. Inputs are read-only, each vertex
 *     writes its own output slot, so partitioning is always safe and the
 *     RESULT IS BIT-IDENTICAL regardless of worker count. That last property
 *     matters: benchmark checksums stay stable when the core count changes.
 *
 *   - RASTERIZATION is not. A rasterizer holds mutable state and writes a
 *     shared z-buffer with no synchronization, so a single draw call can never
 *     be split across cores. Tile-parallel rendering is possible instead - one
 *     renderer instance, sub-image and z-buffer slice per core - but it costs
 *     one full geometry pass PER TILE, so it only wins when fill-bound.
 *     Decide that by measurement, not by assumption.
 *
 *
 * No std::function: it allocates and may throw. A plain function pointer plus
 * a context pointer keeps this usable on an MCU.
 */
#ifndef A3D_JOBS_H_
#define A3D_JOBS_H_

#include "a3d_board.h"

namespace a3d {

/**
 * Work item callback. Processes the half-open range [begin, end).
 */
using JobFn = void (*)(void* ctx, int begin, int end);


/**
 * Deterministic range partitioning, shared by every executor so that a given
 * (nbItems, nbParts) always produces the same split.
 *
 * Remainder items go to the first chunks, so chunk sizes differ by at most 1.
 */
inline void splitRange(int nbItems, int nbParts, int part, int& begin, int& end)
    {
    if ((nbParts <= 0) || (part < 0) || (part >= nbParts) || (nbItems <= 0))
        {
        begin = 0; end = 0; return;
        }
    const int base = nbItems / nbParts;
    const int rem  = nbItems % nbParts;
    begin = part * base + (part < rem ? part : rem);
    end   = begin + base + (part < rem ? 1 : 0);
    }


/**
 * How many parts a workload should actually be split into.
 * Never more than the worker count, never so many that chunks fall below
 * A3D_PARALLEL_MIN_CHUNK.
 */
inline int planParts(int nbItems, int workerCount, int minChunk)
    {
    if (nbItems <= 0) return 0;
    if (workerCount < 1) workerCount = 1;
    if (minChunk < 1) minChunk = 1;
    int parts = nbItems / minChunk;
    if (parts < 1) parts = 1;
    if (parts > workerCount) parts = workerCount;
    return parts;
    }


/**
 * Parallel execution strategy. Blocking: parallelFor() returns only when all
 * parts have completed.
 */
class IJobExecutor
    {
    public:

        virtual ~IJobExecutor() = default;

        /** Number of parallel workers, including the calling thread. >= 1. */
        virtual int workerCount() const = 0;

        /**
         * Run `fn` over [0, nbItems) split across workers, and wait.
         *
         * @param minChunk  do not create parts smaller than this.
         */
        virtual void parallelFor(int nbItems, JobFn fn, void* ctx, int minChunk) = 0;

        /**
         * Convenience overload using A3D_PARALLEL_MIN_CHUNK.
         *
         * Deliberately NON-virtual rather than a default argument on the
         * virtual above: default arguments on virtual functions bind
         * statically to the declared type, which silently gives different
         * behavior depending on the pointer type used at the call site.
         *
         * Derived classes must pull this in with `using IJobExecutor::parallelFor;`
         * so the override does not hide it.
         */
        void parallelFor(int nbItems, JobFn fn, void* ctx)
            {
            parallelFor(nbItems, fn, ctx, A3D_PARALLEL_MIN_CHUNK);
            }
    };


/**
 * Always-available single-threaded executor.
 *
 * This is the default. A3D is fully functional with it; multicore is an
 * optimization, never a requirement.
 */
class SerialExecutor final : public IJobExecutor
    {
    public:

        using IJobExecutor::parallelFor;   // keep the 3-argument convenience overload visible

        int workerCount() const override { return 1; }

        void parallelFor(int nbItems, JobFn fn, void* ctx, int minChunk) override
            {
            (void)minChunk;
            if ((nbItems > 0) && (fn != nullptr)) fn(ctx, 0, nbItems);
            }
    };


// ---------------------------------------------------------------------------
// Tiled drawing across workers
// ---------------------------------------------------------------------------
//
// `IRenderBackend::drawDrawableTile()` is safe to call concurrently for
// different tiles provided each caller passes its OWN backend and its OWN
// scratch buffer. What was missing was the part that decides how many "own"
// there are, and every application that wanted two cores wrote its own - hard
// wired to two, asserting on a second core that has to exist, and allocating a
// second tile framebuffer whether or not anything would use it.
//
// The rule these two functions exist to keep in one place:
//
//     allocate tileSlotCount(ex, nbTiles) resource sets,
//     and forEachTile() will only ever hand you a slot below that.
//
// With SerialExecutor that count is 1, the tiles run inline on the calling
// core, and no task, semaphore or second framebuffer exists. Single core is not
// a fallback path here - it is the same path with one slot.
//

/**
 * How many private resource sets a tiled draw needs - backends, tile
 * framebuffers, scratch index buffers. Allocate exactly this many.
 */
inline int tileSlotCount(const IJobExecutor& ex, int nbTiles)
    {
    if (nbTiles <= 0) return 0;
    int n = ex.workerCount();
    if (n < 1) n = 1;
    // More slots than tiles would allocate a framebuffer nothing can use.
    return (n > nbTiles) ? nbTiles : n;
    }

/**
 * Per-tile work. `slot` is below tileSlotCount() and selects this call's
 * private resources; `tile` is which tile to draw.
 */
using TileFn = void (*)(void* ctx, int slot, int tile);

/** Internal: the context forEachTile hands to the executor. */
struct TileDispatch
    {
    TileFn fn;
    void*  ctx;
    int    nbTiles;
    int    nbSlots;
    };

inline void _tileSlotRange(void* p, int begin, int end)
    {
    TileDispatch* d = (TileDispatch*)p;
    // Tiles are handed out STRIDED rather than in contiguous blocks. Tile cost
    // is not uniform - a centred model puts most of its pixels in the middle
    // tiles - and a block split would give one worker the whole model while the
    // other finished early and waited.
    for (int slot = begin; slot < end; slot++)
        for (int t = slot; t < d->nbTiles; t += d->nbSlots)
            d->fn(d->ctx, slot, t);
    }

/**
 * Run `fn` for every tile, spread across the executor's workers. Blocking:
 * returns when every tile is done.
 *
 * The minimum chunk is 1 deliberately. The items being split here are WORKERS,
 * not tiles, so there are only ever a handful of them, and the default minimum
 * chunk would collapse the split to a single part and silently render
 * everything on one core.
 */
inline void forEachTile(IJobExecutor& ex, int nbTiles, TileFn fn, void* ctx)
    {
    if ((nbTiles <= 0) || (fn == nullptr)) return;
    TileDispatch d;
    d.fn = fn; d.ctx = ctx; d.nbTiles = nbTiles;
    d.nbSlots = tileSlotCount(ex, nbTiles);
    if (d.nbSlots < 1) return;
    ex.parallelFor(d.nbSlots, &_tileSlotRange, &d, 1);
    }

} // namespace a3d

#endif // A3D_JOBS_H_
