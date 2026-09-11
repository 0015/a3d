// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_viewer.h
 * @brief Put a model on a screen in five lines, without hiding anything.
 *
 * WHAT THIS IS FOR
 *
 *   Everything below this file was already enough to draw a model. It was not
 *   enough to draw one in five lines: the smallest complete example in this
 *   tree spends 183 of its 474 lines on plumbing that is the same in every
 *   application - decide how many workers there are, allocate that many tile
 *   framebuffers and z-buffers and scratch buffers, configure a binner per
 *   part, frame the camera from the model's own bounds, and get the order of
 *   the six per-frame calls right. None of that is a decision about the
 *   application. All of it is a decision this file can make once.
 *
 *   So: the application supplies a Display - a size and one function that
 *   moves a rectangle of pixels - and gets back a viewer that loads `.a3d`
 *   containers, STL files, and generated geometry, animates them, orbits a
 *   camera around them and presents them.
 *
 * THE WHOLE OF IT
 *
 *     a3d::Display panel;                     // your driver, three fields
 *     panel.width = 320; panel.height = 240;
 *     panel.sendTile = &myBlit;               // one function you already have
 *
 *     a3d::Viewer viewer;
 *     viewer.begin(panel);
 *     viewer.openStl("/sdcard/part.stl");     // or openAsset(fox_start, ...)
 *     while (true) viewer.frame(16.0f);
 *
 *   That is a lit, framed, orbitable model. The viewer test in the development
 *   repository (`tests/test_viewer.cpp`, not part of a release) opens with
 *   exactly those four calls and checks that they draw, so the snippet cannot
 *   rot the way a README example does.
 *
 *   Everything else on this class is a default being overridden: which clip
 *   plays, where the memory comes from, how many cores draw, what the light
 *   does, what goes on top.
 *
 * WHAT IT DOES NOT DO
 *
 *   It does not wrap the panel. Bringing up a display is the application's,
 *   because the application has the schematic. See a3d_display.h.
 *
 *   It does not replace the raw API and it does not hide it. `backend()`,
 *   `scene()`, `player()` and `binners()` hand back the real objects, so
 *   anything this file does not do can still be done beside it rather than
 *   instead of it. A wrapper that has to be abandoned wholesale the first time
 *   it is not enough is worse than no wrapper.
 *
 *   It does not overlap a tile's DMA with the next tile's draw, which
 *   `ESP32-P4-Visual-Demo` does by hand and measures. That demo is still the
 *   faster dispatch and is deliberately not built on this.
 *
 * THE ORDER THIS FILE EXISTS TO GET RIGHT
 *
 *   Each of these cost this project time, and each is silent when wrong:
 *
 *   1. The EXECUTOR decides how many tile framebuffers exist, so it is built
 *      before them. One worker allocates one set, not two - which on a 480-wide
 *      panel is 38 KB of internal SRAM that a single-core part does not have.
 *   2. `worldBounds()` reads the SKINNED vertices, so framing the camera
 *      before `skinAll()` silently frames the PREVIOUS pose. A check written
 *      that way once measured 0.0% movement on every animated model and nearly
 *      concluded the animation was dead.
 *   3. Binning happens once, before the workers are released. Every worker
 *      reads the result and none writes it, so there is nothing to lock.
 *   4. The tile is sent to the panel INSIDE the worker that drew it. A slot
 *      owns one tile buffer and reuses it for the next tile it is handed, so a
 *      blit loop after the dispatch sends whatever each slot finished on.
 *   5. On the binned path `Shading::Flat` is a CONSTANT - the rasterizer has
 *      screen-space vertices and no world positions, so it cannot derive a
 *      face normal. Faceted shading comes from UNWELDED vertices plus Gouraud,
 *      which reads backwards and is right. `openStl(..., faceted)` does that.
 *
 * COST
 *
 *   None that is measurable, by construction: this makes the same calls in the
 *   same order as the examples it replaces. That is a claim about the code and
 *   not a measurement - if you care, measure it on the part, where run-to-run
 *   spread is about 1.2 ms on a P4 globe and the host does not predict the
 *   device.
 *
 * SIZE
 *
 *   Header-only, like everything else here. `A3D_VIEWER_NO_STL` and
 *   `A3D_VIEWER_NO_STDIO` drop the two loaders for a flash-tight build.
 */
#ifndef A3D_VIEWER_H_
#define A3D_VIEWER_H_

#include "a3d/a3d_anim.h"
#include "a3d/a3d_binner.h"
#include "a3d/a3d_board.h"
#include "a3d/a3d_display.h"
#include "a3d/a3d_jobs.h"
#include "a3d/a3d_math.h"
#include "a3d/a3d_primitives.h"
#include "a3d/a3d_reader.h"
#include "a3d/a3d_runtime.h"
#include "a3d/a3d_types.h"
#include "backends/soft/a3d_soft_backend.h"

#ifndef A3D_VIEWER_NO_STL
#  include "loaders/a3d_load_stl.h"
#endif
#ifndef A3D_VIEWER_NO_STDIO
#  include "loaders/a3d_load_stdio.h"
#endif

#include <math.h>
#include <new>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * Upper bound on tile workers, and therefore on the resource sets that can be
 * allocated. The number actually used comes from the executor at run time;
 * this only sizes the array.
 */
#ifndef A3D_VIEWER_MAX_SLOTS
#  define A3D_VIEWER_MAX_SLOTS A3D_MAX_WORKERS
#endif

namespace a3d {

/** How the viewport is set up. Everything here can be left alone. */
struct ViewerConfig
    {
    /** 0 takes the Display's size. Set it smaller to render below panel size. */
    int width  = 0;
    int height = 0;

    /**
     * Rows per tile. This is the memory knob: a slot costs
     * `width * tileHeight * 2` bytes for pixels and the same again for depth,
     * and there are `slotCount()` slots. 40 rows of a 480-wide panel is 38 KB
     * a slot.
     */
    int tileHeight = 40;

    /** A z-buffer doubles per-slot memory and is what lets a mesh self-occlude.
     *  Turning it off also selects a genuinely faster rasterizer path, so a
     *  scene that cannot overlap itself should. */
    bool zbuffer = true;

    /** Cleared to this before each tile is drawn. RGB565, native endian. */
    uint16_t background = 0x0000;

    Shading     shading = Shading::Gouraud;
    TextureMode texture = TextureMode::Perspective;

    /** -1 matches the stored projection, whose y row is negated so clip space
     *  matches framebuffer rows. This was measured, not reasoned about. */
    int culling = -1;

    /** Send tiles to the Display. False renders and counts but shows nothing,
     *  which is what an off-screen benchmark wants. */
    bool present = true;

    /** Count non-background pixels each frame. Off by default: it is a second
     *  pass over every pixel, and it exists because "it drew nothing" and "it
     *  drew and the panel did not show it" are two faults with one symptom. */
    bool countPixels = false;
    };


/** What the last frame did. Times are zero unless a clock was supplied. */
struct ViewerStats
    {
    uint32_t frames         = 0;   ///< since begin()
    uint32_t trianglesDrawn = 0;   ///< submitted to the rasterizer, last frame
    uint32_t trianglesBinned = 0;  ///< across all tiles; > triangleCount() when
                                   ///< a triangle spans tiles
    uint32_t litPixels      = 0;   ///< only when ViewerConfig::countPixels
    uint32_t tilesDrawn     = 0;   ///< tiles the binner put geometry in
    uint32_t tilesSkipped   = 0;   ///< tiles that were background only

    uint32_t animUs = 0;   ///< pose + world transforms
    uint32_t skinUs = 0;
    uint32_t binUs  = 0;
    uint32_t drawUs = 0;   ///< wall clock across the workers, blit included
    uint32_t frameUs = 0;
    };


/**
 * A model on a screen.
 *
 * Not copyable: it owns tile buffers, binners and a bound scene. One instance
 * per display.
 */
class Viewer
    {
    public:

        using AllocFn = void* (*)(size_t);
        using FreeFn  = void  (*)(void*);

        /**
         * Draw on top of a tile, after the 3D and before it goes to the panel.
         *
         * `tile` is `width * rows` pixels whose first row is screen row
         * `tileTop`, so a caption at an absolute screen row `y` is drawn at
         * `y - tileTop` and clipped to `rows`. It is called for every tile,
         * which is what lets something straddling two tiles come out in both.
         */
        using OverlayFn = void (*)(void* user, uint16_t* tile, int tileTop,
                                   int rows, int width, int height);

        /** Microseconds, monotonic. `esp_timer_get_time` fits directly. */
        using ClockFn = uint64_t (*)(void* user);

        Viewer() = default;
        ~Viewer() { end(); }
        Viewer(const Viewer&) = delete;
        Viewer& operator=(const Viewer&) = delete;

        // ==================================================================
        // Setup. Everything in this block is read by begin(), so call it first.
        // ==================================================================

        /**
         * Where the TILE BUFFERS come from: pixels and depth, touched by the
         * rasterizer for every pixel of every frame and handed to the panel.
         *
         * This is the memory that has to be fast, and on a part with DMA it is
         * the memory that has to be DMA-capable. On ESP-IDF that means
         * `heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`.
         * Default is plain malloc.
         */
        void setAllocator(AllocFn alloc, FreeFn release)
            {
            if ((alloc != nullptr) && (release != nullptr))
                { _alloc = alloc; _free = release; }
            }

        /**
         * Where the BULK arrays come from: binner lists, per-slot scratch,
         * decoded vertex buffers, generated geometry, an STL's positions.
         *
         * These are read once per frame per triangle rather than per pixel, so
         * PSRAM is usually the right answer for them on a part that has it -
         * and it is a large amount of memory to take out of internal SRAM by
         * accident. Defaults to the tile allocator, which defaults to malloc.
         *
         * The one exception worth knowing: a SKINNED mesh rewrites its decoded
         * vertices every frame, so for an animated model this is a hot write,
         * not a cold read. Measure before putting an animated model's vertices
         * in PSRAM.
         */
        void setLargeAllocator(AllocFn alloc, FreeFn release)
            {
            if ((alloc != nullptr) && (release != nullptr))
                { _bigAlloc = alloc; _bigFree = release; }
            }

        /**
         * Draw tiles across this executor's workers. Default is one worker,
         * inline, with no task and no second framebuffer - which is a
         * supported target here and not a fallback.
         *
         * Must outlive the viewer. Call before begin(): it is what decides how
         * many resource sets get allocated.
         */
        void setExecutor(IJobExecutor& ex) { _exec = &ex; }

        /** Optional. Without it every time in ViewerStats stays zero. */
        void setClock(ClockFn fn, void* user) { _clock = fn; _clockUser = user; }

        /** Optional. See OverlayFn. */
        void setOverlay(OverlayFn fn, void* user) { _overlay = fn; _overlayUser = user; }

        /**
         * Allocate the tile buffers and configure the renderers.
         *
         * @returns false if the display is unusable or a buffer would not
         *          allocate. A partially built viewer frees itself, so a false
         *          return leaves nothing behind.
         */
        bool begin(const Display& display, const ViewerConfig& cfg = ViewerConfig())
            {
            end();
            if (!display.valid()) return false;

            _disp = display;
            _cfg  = cfg;
            _viewW = (cfg.width  > 0) ? cfg.width  : display.width;
            _viewH = (cfg.height > 0) ? cfg.height : display.height;
            _tileH = (cfg.tileHeight > 0) ? cfg.tileHeight : 40;
            if (_tileH > _viewH) _tileH = _viewH;
            if ((_viewW <= 0) || (_viewH <= 0)) return false;

            _nbTiles = (_viewH + _tileH - 1) / _tileH;
            // The binner addresses tiles with a uint8 range per axis.
            if (_nbTiles > 255) { _viewW = 0; return false; }

            // The executor decides the count. Rule 1 in the file comment.
            _nbSlots = tileSlotCount(*_executor(), _nbTiles);
            if (_nbSlots < 1) _nbSlots = 1;
            // REFUSED, not clamped. forEachTile() hands out a slot per worker
            // from its OWN tileSlotCount(), so clamping here would leave two
            // workers sharing one tile framebuffer and one z-buffer - which is
            // not a slow render, it is two threads writing the same pixels.
            // Raise A3D_VIEWER_MAX_SLOTS to match the executor.
            if (_nbSlots > A3D_VIEWER_MAX_SLOTS)
                { _error = "executor has more workers than A3D_VIEWER_MAX_SLOTS"; end(); return false; }

            const size_t tilePixels = (size_t)_viewW * (size_t)_tileH;
            for (int i = 0; i < _nbSlots; i++)
                {
                Slot& s = _slots[i];
                s.fb = (uint16_t*)_alloc(tilePixels * sizeof(uint16_t));
                if (s.fb == nullptr) { end(); return false; }
                if (_cfg.zbuffer)
                    {
                    s.zb = (uint16_t*)_alloc(tilePixels * sizeof(uint16_t));
                    if (s.zb == nullptr) { end(); return false; }
                    }
                // setViewportSize BEFORE setTarget: setTarget adopts the tile
                // size as the viewport when the viewport is still unset, and
                // then every projection is computed for a 40-row screen.
                s.renderer.setViewportSize(_viewW, _viewH);
                s.renderer.setTarget(s.fb, _viewW, _tileH, _viewW);
                s.renderer.setZBuffer(s.zb);
                s.renderer.setCulling(_cfg.culling);
                s.renderer.setShading(_cfg.shading, _cfg.texture);
                s.renderer.setLightDirection(_lx, _ly, _lz);
                s.renderer.setAllocator(_bigAlloc, _bigFree);
                }

            if (_cfg.present && (_disp.fill != nullptr))
                _disp.fill(_disp.user, _cfg.background);

            _ready = true;
            return true;
            }

        /** Release everything, including any open model. Safe to call twice. */
        void end()
            {
            close();
            for (int i = 0; i < A3D_VIEWER_MAX_SLOTS; i++)
                {
                Slot& s = _slots[i];
                if (s.fb != nullptr) { _free(s.fb); s.fb = nullptr; }
                if (s.zb != nullptr) { _free(s.zb); s.zb = nullptr; }
                s.renderer.setTarget(nullptr, 0, 0, 0);
                s.renderer.setZBuffer(nullptr);
                }
            _nbSlots = 0;
            _nbTiles = 0;
            _ready = false;
            }

        bool ready() const { return _ready; }

        // ==================================================================
        // Loading. Any of these replaces whatever was open.
        // ==================================================================

        /**
         * An `.a3d` container already in memory.
         *
         * On ESP-IDF this is the EMBED_FILES symbol, which points at
         * memory-mapped flash: the container is used in place and the mesh
         * costs no RAM until it is decoded. The bytes must outlive the viewer.
         */
        bool openAsset(const void* data, size_t bytes)
            {
            close();
            if (!_ready || (data == nullptr) || (bytes == 0)) return false;
            if (!_image.open(data, bytes)) return _fail("not an a3d container");
            return _bindAsset();
            }

#ifndef A3D_VIEWER_NO_STDIO
        /** An `.a3d` file - an SD card, or anything else stdio can open. */
        bool openAssetFile(const char* path)
            {
            close();
            if (!_ready || (path == nullptr)) return false;
            if (!_file.load(path, _bigAlloc, _bigFree)) return _fail("cannot read file");
            _image = _file.image();
            if (!_image.valid()) return _fail("not an a3d container");
            _ownFile = true;
            return _bindAsset();
            }
#endif

#ifndef A3D_VIEWER_NO_STL
        /**
         * An STL file - the format 3D printing runs on.
         *
         * @param faceted  keep every facet's own normal instead of averaging
         *                 them, which is what a mechanical part or a building
         *                 wants: smoothing rounds its corners off. It works by
         *                 NOT welding, so it also costs about twice the
         *                 vertices and caps the file at 21,845 triangles.
         *
         * Drawing is the limit, not loading: about 12.8 us per triangle on an
         * S3, so 5,000 triangles is a 64 ms frame. Printable STLs are
         * routinely 100,000+. Decimate offline with
         * `tools/a3d_export.py --max-triangles N`.
         */
        bool openStl(const char* path, bool faceted = false)
            {
            StlOptions opt;
            opt.weld = !faceted;
            return openStl(path, opt);
            }

        bool openStl(const char* path, const StlOptions& opt)
            {
            close();
            if (!_ready || (path == nullptr)) return false;
            if (!_stl.load(path, opt, _bigAlloc, _bigFree))
                return _fail(_stl.info().error);
            return _bindStl();
            }

        /** An STL embedded in the firmware, or already read into memory. */
        bool openStlMemory(const void* data, size_t bytes, bool faceted = false)
            {
            StlOptions opt;
            opt.weld = !faceted;
            return openStlMemory(data, bytes, opt);
            }

        bool openStlMemory(const void* data, size_t bytes, const StlOptions& opt)
            {
            close();
            if (!_ready || (data == nullptr)) return false;
            if (!_stl.loadMemory(data, bytes, opt, _bigAlloc, _bigFree))
                return _fail(_stl.info().error);
            return _bindStl();
            }

        /** What the STL turned out to contain. Empty when none is open. */
        const StlInfo& stlInfo() const { return _stl.info(); }
#endif

        /**
         * Plain arrays, BORROWED. They must outlive the model.
         *
         * This is the seam the other loaders go through, and it is public
         * because a mesh a3d has no loader for is still a mesh: read it your
         * way, hand the arrays over, and it goes through the same binning,
         * culling and rasterizer as everything else.
         *
         * @param normals    optional; without them Gouraud degrades to a
         *                   constant shade.
         * @param texcoords  optional; needed with setTexture().
         */
        bool openMesh(const float* positions, const float* normals,
                      const float* texcoords, int nbVertices,
                      const uint16_t* indices, int nbTriangles)
            {
            close();
            if (!_ready) return false;
            if ((positions == nullptr) || (indices == nullptr)) return false;
            if ((nbVertices <= 0) || (nbTriangles <= 0)) return false;
            _mesh.positions = positions;
            _mesh.normals   = normals;
            _mesh.texcoords = texcoords;
            _mesh.indices   = indices;
            _mesh.nbVertices  = nbVertices;
            _mesh.nbTriangles = nbTriangles;
            return _bindMesh();
            }

        /** A UV sphere of radius 1, generated and owned by the viewer. */
        bool openSphere(int sectors = 32, int stacks = 16)
            {
            const MeshSize sz = sphereSize(sectors, stacks);
            MeshSink sink;
            if (!_ownGeometry(sz, true, sink)) return false;
            sphereMesh(sectors, stacks, sink);
            return _bindOwnGeometry(sz);
            }

        /** A box of half-extents hx,hy,hz, with hard edges and per-face uvs. */
        bool openBox(float hx = 1.0f, float hy = 1.0f, float hz = 1.0f)
            {
            const MeshSize sz = boxSize();
            MeshSink sink;
            if (!_ownGeometry(sz, true, sink)) return false;
            boxMesh(hx, hy, hz, sink);
            return _bindOwnGeometry(sz);
            }

        /** Bottom ring at y=-1, top at y=+1. rTop==rBottom is a cylinder,
         *  rTop==0 a cone. */
        bool openCone(int sectors, float rBottom, float rTop,
                      bool capBottom = true, bool capTop = true)
            {
            const MeshSize sz = truncatedConeSize(sectors, capBottom, capTop);
            MeshSink sink;
            if (!_ownGeometry(sz, false, sink)) return false;
            truncatedConeMesh(sectors, rBottom, rTop, capBottom, capTop, sink);
            return _bindOwnGeometry(sz);
            }

        /** Unload the model. The viewer stays usable; open another. */
        void close()
            {
            if (_binners != nullptr)
                {
                for (uint32_t i = 0; i < _nbDrawables; i++)
                    {
                    _binners[i].release();
                    _binners[i].~TileBinner();
                    }
                _bigFree(_binners);
                _binners = nullptr;
                }
            _nbDrawables = 0;
            _meshBinner.release();

            _scene.release();
            _image = AssetImage();
            _player = AnimationPlayer();
#ifndef A3D_VIEWER_NO_STDIO
            if (_ownFile) { _file.release(); _ownFile = false; }
#endif
#ifndef A3D_VIEWER_NO_STL
            _stl.release();
#endif
            _freeOwnGeometry();
            _mesh = MeshSrc();

            for (int i = 0; i < A3D_VIEWER_MAX_SLOTS; i++)
                {
                if (_slots[i].scratch != nullptr)
                    { _bigFree(_slots[i].scratch); _slots[i].scratch = nullptr; }
                _slots[i].scratchTri = 0;
                }
            _src = SrcNone;
            _nbVertices = 0;
            _nbTriangles = 0;
            _error = "";
            }

        bool haveModel() const { return _src != SrcNone; }
        uint32_t vertexCount() const { return _nbVertices; }
        uint32_t triangleCount() const { return _nbTriangles; }
        /** Why the last open() returned false. Never null. */
        const char* error() const { return _error; }

        // ==================================================================
        // Animation. Silently inert on a model that carries no clips.
        // ==================================================================

        uint32_t clipCount() const
            { return _image.valid() ? _image.clipCount() : 0u; }

        bool selectClip(uint32_t index)
            {
            if (!_player.bound() && (clipCount() == 0)) return false;
            return _player.selectClip(index);
            }

        const char* clipName() const { return _player.clipName(); }
        float clipDurationMs() const { return _player.durationMs(); }
        float clipTimeMs() const { return _player.timeMs(); }

        /** Paused still draws; it just stops advancing. */
        void setPlaying(bool play) { _playing = play; }
        bool playing() const { return _playing; }

        /** 1.0 is real time. Negative runs the clip backwards. */
        void setSpeed(float s) { _speed = s; }
        float speed() const { return _speed; }

        /** Jump to a time in the clip. Poses on the next frame(). */
        void seek(float timeMs) { _player.seek(timeMs); _posePending = true; }

        // ==================================================================
        // Camera. An orbit unless setCamera() takes it over.
        // ==================================================================

        /**
         * Point the camera at the model and back off far enough to see it.
         *
         * @param margin how much room to leave around it. 1.0 is an exact fit
         *               of the model's bounding SPHERE, which touches the edge;
         *               1.15 leaves 15%.
         *
         * Called automatically when a model is opened. Call it again after
         * changing the pose if the model's extent changed a lot.
         *
         * TWO THINGS IT HAS TO GET RIGHT, both of which are silent when wrong:
         *
         * 1. It works from `worldBounds()`, which for a skinned mesh reads the
         *    SKINNED vertices - so it is deferred to the next frame(), after
         *    skinning has run. Framing before that silently frames the previous
         *    pose, and the only symptom is a camera that is slightly off.
         * 2. It fits the NARROWER of the two field-of-view half-angles, not
         *    the vertical one. A distance chosen from the vertical alone fits
         *    perfectly on a landscape panel and cuts the sides off a portrait
         *    one - on this tree's 800x1280 board that is 38% of the model
         *    missing, and it looks like the model, not like the camera.
         */
        void frameModel(float margin = 1.15f)
            {
            _margin = (margin > 0.01f) ? margin : 0.01f;
            _distUser = false;        // asking to frame takes the distance back
            _framePending = true;
            }

        /**
         * Distance, in model radii, at which a sphere of the model's radius
         * exactly fills the tighter of the two half-angles. Public because an
         * application that drives the camera itself still wants the number.
         */
        float fitDistance() const
            {
            const float halfV = tanf(_fov * 0.5f * 0.01745329252f);
            const float aspect = (_viewH > 0) ? ((float)_viewW / (float)_viewH) : 1.0f;
            const float halfH = halfV * aspect;
            float lim = (halfH < halfV) ? halfH : halfV;
            if (lim < 1e-3f) lim = 1e-3f;
            return 1.0f / lim;
            }

        /** Yaw and pitch in radians, distance in model radii. */
        void setOrbit(float yaw, float pitch, float distance)
            {
            _yaw = yaw; _pitch = _clampPitch(pitch);
            _dist = _clampDist(distance);
            // A distance the caller picked outranks the one framing would pick.
            // Without this, opening a model and then placing the camera loses
            // the placement: the framing that open() queued runs at the next
            // frame() and overwrites it, which reads as setOrbit() being
            // ignored every other time.
            _distUser = true;
            _custom = false;
            }

        void orbit(float dYaw, float dPitch)
            { setOrbit(_yaw + dYaw, _pitch + dPitch, _dist); }

        /**
         * A RATIO, not a step: spreading two fingers 40 px should mean the same
         * thing whether they started 60 px apart or 300.
         */
        void zoom(float ratio)
            { if (ratio > 1e-3f) setOrbit(_yaw, _pitch, _dist * ratio); }

        /** One-finger drag, in pixels. The sign is what makes the model follow
         *  the finger rather than run from it. */
        void drag(int dx, int dy, float radiansPerPixel = 0.010f)
            { orbit(-(float)dx * radiansPerPixel, (float)dy * radiansPerPixel); }

        float yaw() const { return _yaw; }
        float pitch() const { return _pitch; }
        float distance() const { return _dist; }

        /** Stop the pitch short of the pole, where the view direction and up
         *  become parallel and the picture rolls over. */
        void setPitchLimits(float lo, float hi) { _pitchLo = lo; _pitchHi = hi; }
        /** In model radii. Pass 0,0 to go back to the defaults, which are
         *  0.45x and 3.2x `fitDistance()` and therefore follow the aspect. */
        void setDistanceLimits(float lo, float hi) { _distLo = lo; _distHi = hi; }

        void setFieldOfView(float degrees) { _fov = degrees; }

        /**
         * Drive the camera yourself. The orbit stops being consulted until
         * clearCamera() puts it back.
         */
        void setCamera(const float eye[3], const float target[3], const float up[3])
            {
            for (int i = 0; i < 3; i++)
                { _eye[i] = eye[i]; _target[i] = target[i]; _up[i] = up[i]; }
            _custom = true;
            }
        void clearCamera() { _custom = false; }

        /** Where the camera is aiming, and how big the model is around it. */
        const float* modelCentre() const { return _centre; }
        float modelRadius() const { return _radius; }
        /** Override what frameModel() worked out - a wide flat sheet wants the
         *  camera aimed at the ground, not at the middle of its bounding box. */
        void setModelCentre(float x, float y, float z)
            { _centre[0] = x; _centre[1] = y; _centre[2] = z; }

        // ==================================================================
        // Appearance. Applied to every slot, so two workers cannot disagree.
        // ==================================================================

        /**
         * WHAT HAPPENS TO Shading::Flat, because it is not what it reads like.
         *
         * On the binned path the rasterizer receives screen-space vertices and
         * has no world positions, so it cannot derive a face normal: Flat
         * collapses to one constant `ambient + diffuse * 0.75` for every
         * triangle, and a floor plan renders as a single grey silhouette. That
         * looked like a lighting bug for twenty minutes once.
         *
         * So on the ARRAY sources - STL, generated shapes, openMesh() - Flat is
         * promoted to Gouraud, which is the faceted look when the vertices are
         * unwelded (each carries its own face's normal, and three identical
         * normals interpolate to a constant across the triangle) and smooth
         * when they are welded. `openStl(path, true)` - the faceted flag -
         * is how to ask for the first.
         *
         * A CONTAINER is left exactly as asked: its parts carry node transforms
         * and their own materials, and SceneRuntime already decides this.
         */
        void setShading(Shading s, TextureMode m)
            {
            _cfg.shading = s; _cfg.texture = m;
            for (int i = 0; i < _nbSlots; i++) _slots[i].renderer.setShading(s, m);
            }

        void setBackground(uint16_t rgb565) { _cfg.background = rgb565; }

        void setLightDirection(float x, float y, float z)
            {
            _lx = x; _ly = y; _lz = z;
            for (int i = 0; i < _nbSlots; i++) _slots[i].renderer.setLightDirection(x, y, z);
            }

        void setCulling(int direction)
            {
            _cfg.culling = direction;
            for (int i = 0; i < _nbSlots; i++) _slots[i].renderer.setCulling(direction);
            for (uint32_t d = 0; d < _nbDrawables; d++) _binners[d].setCulling(direction);
            _meshBinner.setCulling(direction);
            }

        /** The material used for geometry that carries none: STL, generated
         *  shapes, and openMesh() arrays. A container supplies its own. */
        void setMaterial(const Material& m) { _material = m; }
        void setMaterialColor(float r, float g, float b)
            { _material.color[0] = r; _material.color[1] = g; _material.color[2] = b; }

        void setTextureFilter(bool bilinear)
            { for (int i = 0; i < _nbSlots; i++) _slots[i].renderer.setTextureFilter(bilinear); }
        void setMipBias(int levels)
            { for (int i = 0; i < _nbSlots; i++) _slots[i].renderer.setMipBias(levels); }
        void setOpacity(float o)
            { for (int i = 0; i < _nbSlots; i++) _slots[i].renderer.setOpacity(o); }
        void setNearPlane(float n)
            { for (int i = 0; i < _nbSlots; i++) _slots[i].renderer.setNearPlane(n); }

        /**
         * Wrap pixels as a texture for the untextured sources. Both dimensions
         * must be powers of two; the pixels are NOT copied and must outlive it.
         * A container's own textures come through bind() and need none of this.
         */
        TextureHandle createTexture(int w, int h, const void* rgb565Pixels)
            {
            if (_nbSlots < 1) return TextureHandle();
            return _slots[0].renderer.createTexture(w, h, 0, rgb565Pixels);
            }
        void setTexture(TextureHandle t) { _texture = t; }

        // ==================================================================
        // The frame
        // ==================================================================

        /**
         * Pose, skin, bin, draw every tile, and present.
         *
         * @param dtMs milliseconds since the last call. 0 leaves the pose
         *             alone, which is the static case and skips skinning
         *             entirely.
         */
        void frame(float dtMs)
            {
            if (!_ready || (_src == SrcNone)) return;
            const uint64_t t0 = _now();

            // Only a container has a skeleton, clips or skinned vertices. An
            // STL or a generated shape is posed once at open() and never again.
            const bool animated = (_src == SrcAsset);
            const bool advancing = animated && _playing && (dtMs != 0.0f) && _player.bound();
            const bool reposing  = animated && (advancing || _posePending);
            if (reposing)
                {
                if (advancing) _player.advance(dtMs * _speed);
                _player.apply(_scene);
                _scene.updateWorldTransforms();
                }
            const uint64_t t1 = _now();

            // Rule 2: the bounds below read the SKINNED vertices, so this has
            // to run before _frameNow() and not after it.
            if (reposing) _scene.skinAll(*_executor());
            _posePending = false;
            if (_framePending) { _frameNow(); _framePending = false; }
            const uint64_t t2 = _now();

            for (int i = 0; i < _nbSlots; i++) _configureCamera(_slots[i].renderer);
            _computeViewProj();

            // Rule 3: once, before the workers are released.
            _stats.trianglesBinned = 0;
            if (_src == SrcAsset)
                {
                for (uint32_t d = 0; d < _nbDrawables; d++)
                    {
                    _scene.binDrawable(d, _binners[d], _viewProj);
                    _stats.trianglesBinned += _binners[d].totalSubmissions();
                    }
                }
            else
                {
                float mvp[16];
                mat4Multiply(_viewProj, _model, mvp);
                _meshBinner.bin(_mesh.positions, _mesh.nbVertices,
                                _mesh.indices, _mesh.nbTriangles, mvp);
                _stats.trianglesBinned = _meshBinner.totalSubmissions();
                }
            const uint64_t t3 = _now();

            for (int i = 0; i < _nbSlots; i++)
                { _slots[i].drawn = 0; _slots[i].lit = 0; _slots[i].skipped = 0; }

            forEachTile(*_executor(), _nbTiles, &_tileThunk, this);
            const uint64_t t4 = _now();

            _stats.trianglesDrawn = 0; _stats.litPixels = 0; _stats.tilesSkipped = 0;
            for (int i = 0; i < _nbSlots; i++)
                {
                _stats.trianglesDrawn += _slots[i].drawn;
                _stats.litPixels      += _slots[i].lit;
                _stats.tilesSkipped   += _slots[i].skipped;
                }
            _stats.tilesDrawn = (uint32_t)_nbTiles - _stats.tilesSkipped;
            _stats.animUs  = (uint32_t)(t1 - t0);
            _stats.skinUs  = (uint32_t)(t2 - t1);
            _stats.binUs   = (uint32_t)(t3 - t2);
            _stats.drawUs  = (uint32_t)(t4 - t3);
            _stats.frameUs = (uint32_t)(t4 - t0);
            _stats.frames++;
            }

        /**
         * Where a world-space point lands on the screen, for an overlay drawn
         * on top of the model - a marker, a label, a leader line.
         *
         * Uses the last frame's camera, so call it after frame(). False means
         * the point is behind the eye or outside the depth range, and a caller
         * that ignores that draws markers for things behind the camera.
         *
         * The half-pixel in here is not decoration: the rasterizer places a
         * vertex at `(ndc + 1) * half - 0.5` because pixel centres sit at
         * `x + 0.5`. On a globe where one pixel is 20 km, dropping it puts a
         * city marker a city's width beside the city.
         */
        bool project(float x, float y, float z, int* px, int* py) const
            { return projectToPixel(_viewProj, x, y, z, _viewW, _viewH, px, py); }

        /** The model matrix for the non-container sources. Column-major, and
         *  PRE-multiplying: `setScale(); multRotate(); multTranslate()` spins
         *  in place and then moves. */
        void setModelMatrix(const float m16[16])
            {
            if (m16 == nullptr) mat4Identity(_model);
            else                mat4Copy(m16, _model);
            }

        // ==================================================================
        // The way out. These are the real objects, not copies.
        // ==================================================================

        SoftBackend& backend(int slot = 0)
            { return _slots[(slot >= 0 && slot < _nbSlots) ? slot : 0].renderer; }
        SceneRuntime& scene() { return _scene; }
        AnimationPlayer& player() { return _player; }
        const AssetImage& image() const { return _image; }
        /** One per drawable, in drawable order. Null when no container is open. */
        TileBinner* binners() { return _binners; }
        uint32_t drawableCount() const { return _nbDrawables; }

        int slotCount() const { return _nbSlots; }
        int tileCount() const { return _nbTiles; }
        int viewportWidth() const { return _viewW; }
        int viewportHeight() const { return _viewH; }
        int tileHeight() const { return _tileH; }
        const float* viewProjection() const { return _viewProj; }
        const ViewerStats& stats() const { return _stats; }
        const Display& display() const { return _disp; }

    private:

        enum Src { SrcNone = 0, SrcAsset, SrcMesh };

        struct Slot
            {
            SoftBackend renderer;
            uint16_t* fb = nullptr;
            uint16_t* zb = nullptr;
            uint16_t* scratch = nullptr;
            int scratchTri = 0;
            uint32_t drawn = 0;
            uint32_t lit = 0;
            uint32_t skipped = 0;
            };

        /** Arrays plus who owns them. Covers STL, generated shapes and
         *  openMesh() with one draw path, because they differ only in where
         *  the floats came from. */
        struct MeshSrc
            {
            const float*    positions = nullptr;
            const float*    normals   = nullptr;
            const float*    texcoords = nullptr;
            const uint16_t* indices   = nullptr;
            int nbVertices  = 0;
            int nbTriangles = 0;
            };

        // --- allocation -----------------------------------------------------
        static void* _defaultAlloc(size_t n) { return malloc(n); }
        static void  _defaultFree(void* p)   { free(p); }

        IJobExecutor* _executor() { return (_exec != nullptr) ? _exec : &_serial; }

        /**
         * Give up on an open, and leave a reason behind.
         *
         * The order is the whole of it: close() resets _error, so setting the
         * reason first loses it and error() comes back empty on every refused
         * load. Every caller here still returned false, so nothing looked
         * wrong except an application that could not say why.
         *
         * `why` must outlive the call - a literal, or StlInfo::error, which is
         * also always a literal.
         */
        bool _fail(const char* why)
            {
            close();
            _error = ((why != nullptr) && (why[0] != '\0')) ? why : "open failed";
            return false;
            }

        // --- sources --------------------------------------------------------

        /** Shared tail of every container open: bind, size the binners, frame. */
        bool _bindAsset()
            {
            if (!_scene.bind(_image, _slots[0].renderer))
                return _fail("model would not bind");
            _player.bind(_image);
            if (_image.clipCount() > 0) _player.selectClip(0);

            _nbDrawables = _scene.drawableCount();
            if (_nbDrawables == 0) return _fail("container has no drawable mesh");

            _binners = (TileBinner*)_bigAlloc(sizeof(TileBinner) * _nbDrawables);
            if (_binners == nullptr) return _fail("no memory for binners");

            uint32_t maxTri = 0;
            _nbVertices = 0; _nbTriangles = 0;
            for (uint32_t d = 0; d < _nbDrawables; d++)
                {
                new (&_binners[d]) TileBinner();
                uint32_t v = 0, t = 0;
                _scene.drawableSize(d, v, t);
                if (t > maxTri) maxTri = t;
                _nbVertices += v; _nbTriangles += t;
                _binners[d].setAllocator(_bigAlloc, _bigFree);
                _binners[d].setCulling(_cfg.culling);
                if (!_binners[d].configure(_viewW, _viewH, _viewW, _tileH, (int)t, (int)v))
                    return _fail("binner would not configure");
                }
            if (!_allocScratch(maxTri)) return false;

            // A container places its parts with node transforms, and on the
            // projected path the rasterizer takes normals through whatever
            // model matrix it is holding. Identity is what the binner projected
            // with, so identity is what lighting has to use.
            float id[16]; mat4Identity(id);
            for (int i = 0; i < _nbSlots; i++) _slots[i].renderer.setModelMatrix(id);
            mat4Identity(_model);

            _src = SrcAsset;
            // Pose once so a static model is not drawn in its bind pose, and
            // so the bounds below are the ones that will be drawn.
            _player.apply(_scene);
            _scene.updateWorldTransforms();
            _scene.skinAll(*_executor());
            _distUser = false;
            _framePending = true;
            return true;
            }

#ifndef A3D_VIEWER_NO_STL
        bool _bindStl()
            {
            if (!_stl.valid()) return _fail("STL is empty");
            _mesh.positions   = _stl.positions();
            _mesh.normals     = _stl.normals();
            _mesh.texcoords   = nullptr;
            _mesh.indices     = _stl.indices();
            _mesh.nbVertices  = (int)_stl.vertexCount();
            _mesh.nbTriangles = (int)_stl.triangleCount();
            return _bindMesh();
            }
#endif

        /** One binner, one draw path, whatever produced the arrays. */
        bool _bindMesh()
            {
            _meshBinner.setAllocator(_bigAlloc, _bigFree);
            _meshBinner.setCulling(_cfg.culling);
            if (!_meshBinner.configure(_viewW, _viewH, _viewW, _tileH,
                                       _mesh.nbTriangles, _mesh.nbVertices))
                return _fail("mesh is too large for a binner");
            if (!_allocScratch((uint32_t)_mesh.nbTriangles)) return false;

            _nbVertices  = (uint32_t)_mesh.nbVertices;
            _nbTriangles = (uint32_t)_mesh.nbTriangles;
            mat4Identity(_model);
            _src = SrcMesh;

            // No skinning and no clips, so the bounds are final now.
            _boundsFromArrays();
            _distUser = false;
            _framePending = true;
            return true;
            }

        bool _allocScratch(uint32_t maxTri)
            {
            if (maxTri == 0) return _fail("model has no triangles");
            for (int i = 0; i < _nbSlots; i++)
                {
                if (_slots[i].scratch != nullptr) _bigFree(_slots[i].scratch);
                _slots[i].scratch = (uint16_t*)_bigAlloc((size_t)maxTri * 3 * sizeof(uint16_t));
                _slots[i].scratchTri = (int)maxTri;
                if (_slots[i].scratch == nullptr) return _fail("no memory for scratch");
                }
            return true;
            }

        bool _ownGeometry(const MeshSize& sz, bool withUv, MeshSink& sink)
            {
            close();
            if (!_ready || (sz.vertices <= 0) || (sz.triangles <= 0)) return false;
            _ownPos = (float*)_bigAlloc((size_t)sz.vertices * 3 * sizeof(float));
            _ownNrm = (float*)_bigAlloc((size_t)sz.vertices * 3 * sizeof(float));
            _ownIdx = (uint16_t*)_bigAlloc((size_t)sz.triangles * 3 * sizeof(uint16_t));
            if (withUv) _ownUv = (float*)_bigAlloc((size_t)sz.vertices * 2 * sizeof(float));
            if ((_ownPos == nullptr) || (_ownNrm == nullptr) || (_ownIdx == nullptr) ||
                (withUv && (_ownUv == nullptr)))
                { _fail("no memory for generated geometry"); return false; }
            sink.positions = _ownPos;
            sink.normals   = _ownNrm;
            sink.texcoords = _ownUv;
            sink.indices   = _ownIdx;
            return true;
            }

        bool _bindOwnGeometry(const MeshSize& sz)
            {
            _mesh.positions   = _ownPos;
            _mesh.normals     = _ownNrm;
            _mesh.texcoords   = _ownUv;
            _mesh.indices     = _ownIdx;
            _mesh.nbVertices  = sz.vertices;
            _mesh.nbTriangles = sz.triangles;
            return _bindMesh();
            }

        void _freeOwnGeometry()
            {
            if (_ownPos != nullptr) { _bigFree(_ownPos); _ownPos = nullptr; }
            if (_ownNrm != nullptr) { _bigFree(_ownNrm); _ownNrm = nullptr; }
            if (_ownUv  != nullptr) { _bigFree(_ownUv);  _ownUv  = nullptr; }
            if (_ownIdx != nullptr) { _bigFree(_ownIdx); _ownIdx = nullptr; }
            }

        // --- camera ---------------------------------------------------------

        float _clampPitch(float p) const
            { return (p < _pitchLo) ? _pitchLo : ((p > _pitchHi) ? _pitchHi : p); }
        float _clampDist(float d) const
            {
            const float fit = fitDistance();
            const float lo = (_distLo > 0.0f) ? _distLo : (fit * 0.45f);
            const float hi = (_distHi > 0.0f) ? _distHi : (fit * 3.2f);
            return (d < lo) ? lo : ((d > hi) ? hi : d);
            }

        void _boundsFromArrays()
            {
            if ((_mesh.positions == nullptr) || (_mesh.nbVertices <= 0)) return;
            float mn[3], mx[3];
            for (int k = 0; k < 3; k++) { mn[k] = mx[k] = _mesh.positions[k]; }
            for (int v = 1; v < _mesh.nbVertices; v++)
                for (int k = 0; k < 3; k++)
                    {
                    const float c = _mesh.positions[(size_t)v * 3 + k];
                    if (c < mn[k]) mn[k] = c;
                    if (c > mx[k]) mx[k] = c;
                    }
            _adoptBounds(mn, mx);
            }

        void _adoptBounds(const float mn[3], const float mx[3])
            {
            float ext = 0.0f;
            for (int i = 0; i < 3; i++)
                {
                _centre[i] = 0.5f * (mn[i] + mx[i]);
                const float half = 0.5f * (mx[i] - mn[i]);
                if (half > ext) ext = half;
                }
            _radius = (ext > 1e-4f) ? ext : 1.0f;
            }

        void _frameNow()
            {
            if (_src == SrcAsset)
                {
                float mn[3], mx[3];
                if (_scene.worldBounds(mn, mx)) _adoptBounds(mn, mx);
                }
            else
                {
                _boundsFromArrays();
                }
            if (!_custom && !_distUser) _dist = _margin * fitDistance();
            }

        void _configureCamera(SoftBackend& r)
            {
            const float dist = _radius * _dist;
            r.setViewportSize(_viewW, _viewH);
            r.setPerspective(_fov, (float)_viewW / (float)_viewH,
                             _radius * 0.05f, dist + _radius * 8.0f);
            if (_custom)
                {
                r.setLookAt(_eye, _target, _up);
                return;
                }
            const float cp = cosf(_pitch);
            const float eye[3] = { _centre[0] + dist * cp * sinf(_yaw),
                                   _centre[1] + dist * sinf(_pitch),
                                   _centre[2] + dist * cp * cosf(_yaw) };
            const float up[3] = { 0.0f, 1.0f, 0.0f };
            r.setLookAt(eye, _centre, up);
            }

        /** The projection the binner must use, read from the renderer that
         *  will rasterize with it so the two cannot drift apart. */
        void _computeViewProj()
            {
            float id[16]; mat4Identity(id);
            _slots[0].renderer.setModelMatrix(id);
            _slots[0].renderer.currentMVP(_viewProj);
            }

        // --- the tile -------------------------------------------------------

        static void _tileThunk(void* ctx, int slot, int tile)
            { ((Viewer*)ctx)->_tile(slot, tile); }

        void _tile(int slot, int tile)
            {
            Slot& s = _slots[slot];
            const int y0 = tile * _tileH;
            int rows = _viewH - y0;
            if (rows > _tileH) rows = _tileH;
            if (rows <= 0) return;

            s.renderer.setTarget(s.fb, _viewW, rows, _viewW);
            s.renderer.setOffset(0, y0);

            const size_t px = (size_t)_viewW * (size_t)rows;
            _fill(s.fb, px, _cfg.background);

            // The binner already knows whether anything reaches this tile, so
            // ask it rather than clearing a depth buffer and walking the draw
            // path to produce the same rectangle of background. On a map seen
            // from above this skips about half the tiles.
            if (_tileHasGeometry(tile))
                {
                if (s.zb != nullptr) s.renderer.clearZBuffer();
                s.drawn += (uint32_t)_drawTile(s, tile);
                }
            else
                {
                s.skipped++;
                }

            if (_cfg.countPixels)
                for (size_t i = 0; i < px; i++)
                    if (s.fb[i] != _cfg.background) s.lit++;

            if (_overlay != nullptr)
                _overlay(_overlayUser, s.fb, y0, rows, _viewW, _viewH);

            // Rule 4: this belongs here, not in a loop after the dispatch.
            if (_cfg.present && (_disp.sendTile != nullptr))
                {
                if (_disp.swapBytes) _swap(s.fb, px);
                if (_disp.lockBus != nullptr) _disp.lockBus(_disp.user);
                _disp.sendTile(_disp.user, s.fb, 0, y0, _viewW, rows);
                if (_disp.unlockBus != nullptr) _disp.unlockBus(_disp.user);
                }
            }

        bool _tileHasGeometry(int tile) const
            {
            if (_src == SrcAsset)
                {
                for (uint32_t d = 0; d < _nbDrawables; d++)
                    if (_binners[d].trianglesInTile(tile) != 0) return true;
                return false;
                }
            return _meshBinner.trianglesInTile(tile) != 0;
            }

        int _drawTile(Slot& s, int tile)
            {
            if (_src == SrcAsset)
                {
                int n = 0;
                for (uint32_t d = 0; d < _nbDrawables; d++)
                    n += _scene.drawDrawableTile(d, _binners[d], tile,
                                                 s.scratch, s.scratchTri,
                                                 _cfg.shading, _cfg.texture,
                                                 &s.renderer);
                return n;
                }
            return _drawMeshTile(s, tile);
            }

        /**
         * The array path: STL, generated shapes, borrowed arrays.
         *
         * Mirrors SceneRuntime::drawDrawableTile including the projected fast
         * path - the binner already transformed every vertex with this frame's
         * matrix, and re-projecting them would be doing the same arithmetic
         * twice and calling the second answer the real one.
         *
         * Unlike StlMesh::drawTile this can carry texture coordinates, because
         * a generated sphere has them and an STL does not.
         */
        int _drawMeshTile(Slot& s, int tile)
            {
            const int n = _meshBinner.buildTileIndices(tile, _mesh.indices,
                                                       s.scratch, s.scratchTri);
            if (n <= 0) return 0;   // 0 triangles here, or -1: scratch too small

            Shading use = _cfg.shading;
            // Rule 5. On this path Flat has no world positions to derive a face
            // normal from and collapses to one constant shade for the whole
            // model, which looks like a lighting bug and is not one.
            if ((use == Shading::Gouraud) && (_mesh.normals == nullptr)) use = Shading::Unlit;
            if (use == Shading::Flat) use = (_mesh.normals != nullptr) ? Shading::Gouraud
                                                                      : Shading::Unlit;

            TextureMode tex = _cfg.texture;
            if ((_mesh.texcoords == nullptr) || !_texture.valid()) tex = TextureMode::None;

            // Lighting takes normals through the model matrix even on the
            // projected path, so it has to be pushed even though the positions
            // bypass it.
            s.renderer.setModelMatrix(_model);
            s.renderer.setMaterial(_material);
            s.renderer.setShading(use, tex);

            if (s.renderer.supportsProjectedVertices() &&
                (_meshBinner.screenX() != nullptr) && (_meshBinner.screenW() != nullptr))
                {
                ProjectedBatch pb;
                pb.nbTriangles = n;
                pb.indices = s.scratch;
                pb.screenX = _meshBinner.screenX();
                pb.screenY = _meshBinner.screenY();
                pb.screenW = _meshBinner.screenW();
                pb.behind  = _meshBinner.behind();
                if ((use != Shading::Unlit) && (_mesh.normals != nullptr))
                    { pb.indNormals = s.scratch; pb.normals = _mesh.normals; }
                if (tex != TextureMode::None)
                    {
                    pb.indTexcoords = s.scratch;
                    pb.texcoords = _mesh.texcoords;
                    pb.texture = _texture;
                    }
                s.renderer.drawTrianglesProjected(pb);
                return n;
                }

            TriangleBatch b;
            b.nbTriangles = n;
            b.indPositions = s.scratch;
            b.positions = _mesh.positions;
            if ((use != Shading::Unlit) && (_mesh.normals != nullptr))
                { b.indNormals = s.scratch; b.normals = _mesh.normals; }
            if (tex != TextureMode::None)
                {
                b.indTexcoords = s.scratch;
                b.texcoords = _mesh.texcoords;
                b.texture = _texture;
                }
            s.renderer.drawTriangles(b);
            return n;
            }

        /** Two pixels a store. The scalar form writes half a million uint16 a
         *  frame one at a time, which on an in-order core is real time spent
         *  painting a background the model then covers. */
        static void _fill(uint16_t* px, size_t n, uint16_t colour)
            {
            const uint32_t two = ((uint32_t)colour << 16) | (uint32_t)colour;
            uint32_t* w = (uint32_t*)px;
            const size_t pairs = n >> 1;
            for (size_t i = 0; i < pairs; i++) w[i] = two;
            if (n & 1u) px[n - 1] = colour;
            }

        static void _swap(uint16_t* px, size_t n)
            {
            for (size_t i = 0; i < n; i++)
                { const uint16_t v = px[i]; px[i] = (uint16_t)((v >> 8) | (v << 8)); }
            }

        uint64_t _now() const
            { return (_clock != nullptr) ? _clock(_clockUser) : 0u; }

        // --- state ----------------------------------------------------------
        Display      _disp;
        ViewerConfig _cfg;
        bool _ready = false;

        AllocFn _alloc    = &_defaultAlloc;
        FreeFn  _free     = &_defaultFree;
        AllocFn _bigAlloc = &_defaultAlloc;
        FreeFn  _bigFree  = &_defaultFree;

        SerialExecutor _serial;
        IJobExecutor*  _exec = nullptr;

        Slot _slots[A3D_VIEWER_MAX_SLOTS];
        int  _nbSlots = 0;
        int  _nbTiles = 0;
        int  _viewW = 0, _viewH = 0, _tileH = 0;

        Src _src = SrcNone;
        AssetImage      _image;
        SceneRuntime    _scene;
        AnimationPlayer _player;
        TileBinner*     _binners = nullptr;
        uint32_t        _nbDrawables = 0;
#ifndef A3D_VIEWER_NO_STDIO
        StdioImage _file;
        bool       _ownFile = false;
#endif
#ifndef A3D_VIEWER_NO_STL
        StlMesh _stl;
#endif
        MeshSrc    _mesh;
        TileBinner _meshBinner;
        float*     _ownPos = nullptr;
        float*     _ownNrm = nullptr;
        float*     _ownUv  = nullptr;
        uint16_t*  _ownIdx = nullptr;

        uint32_t _nbVertices = 0, _nbTriangles = 0;
        const char* _error = "";

        bool  _playing = true;
        float _speed = 1.0f;
        bool  _posePending = false;
        bool  _framePending = false;

        float _yaw = 0.6f, _pitch = 0.25f, _dist = 2.6f;
        float _margin = 1.15f;   ///< room left around the fit by frameModel()
        bool  _distUser = false; ///< the caller set the distance; do not reframe it
        float _pitchLo = -1.45f, _pitchHi = 1.45f;
        // The ceiling is a multiple of the FIT rather than a constant, because
        // the fit itself depends on the aspect: 2.4 radii on a square view and
        // 3.9 on this tree's 800x1280 board. A fixed 8.0 was already close
        // enough to the second to cap the zoom-out after two pinches.
        float _distLo = 0.0f, _distHi = 0.0f;   ///< 0: derived from fitDistance()
        float _fov = 45.0f;
        float _centre[3] = { 0.0f, 0.0f, 0.0f };
        float _radius = 1.0f;
        bool  _custom = false;
        float _eye[3]    = { 0.0f, 0.0f, 1.0f };
        float _target[3] = { 0.0f, 0.0f, 0.0f };
        float _up[3]     = { 0.0f, 1.0f, 0.0f };

        float _lx = -0.4f, _ly = -0.6f, _lz = -1.0f;
        Material      _material;
        TextureHandle _texture;
        float _model[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        float _viewProj[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

        OverlayFn _overlay = nullptr;
        void*     _overlayUser = nullptr;
        ClockFn   _clock = nullptr;
        void*     _clockUser = nullptr;

        ViewerStats _stats;
    };

} // namespace a3d

#endif // A3D_VIEWER_H_
