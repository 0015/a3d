// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_demo_app.cpp
 * @brief Benchmark and viewer, shared by every board in this tree.
 *
 * THE BENCHMARK RENDERS AT A FIXED 240x320 AND NEVER SENDS A PIXEL
 * ----------------------------------------------------------------
 * The panels differ: 240x320 SPI on one board, 480x480 QSPI on another,
 * MIPI DSI on a third. Benchmarking at each panel's own size would produce
 * three numbers that cannot be divided by anything, because the pixel counts,
 * the triangle coverage and the blit bandwidth would all differ at once.
 *
 * So the sweep fixes the viewport at BENCH_W x BENCH_H on every board and
 * draws into memory. The result is a property of the CPU, the memory system
 * and the compiler - which is what a board comparison is supposed to isolate.
 * The panel is then measured separately, by the present=1 runs, and the
 * difference between the two is that panel's cost.
 *
 * WHY THE FRAME COUNTS ARE NOT CONSTANT
 * -------------------------------------
 * A part without an FPU is more than an order of magnitude slower than one
 * with it. A fixed 48-frame measurement is four seconds on one board and four
 * minutes on another. The counts are therefore chosen from a probe frame to
 * hit a time budget, and the count actually used is printed with every result
 * so a reader can see how much averaging is behind each number.
 */

#include "a3d_demo_app.h"

#include "a3d/a3d_anim.h"
#include "a3d/a3d_binner.h"
#include "a3d/a3d_board.h"
#include "a3d/a3d_jobs.h"
#include "a3d/a3d_reader.h"
#include "a3d/a3d_runtime.h"
#include "backends/freertos/a3d_freertos_executor.h"
#include "backends/soft/a3d_soft_backend.h"
#include "backends/soft/a3d_soft_text.h"

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>

// The containers, embedded at build time. On ESP32 `.rodata` is memory-mapped
// flash, so these pointers are readable in place: opening an image off one is
// a zero-copy XIP load, and the rasterizer samples the textures straight out
// of flash without ever copying them to RAM.
extern const uint8_t fox_start[]    asm("_binary_fox_a3d_start");
extern const uint8_t fox_end[]      asm("_binary_fox_a3d_end");
extern const uint8_t cesium_start[] asm("_binary_cesiumman_a3d_start");
extern const uint8_t cesium_end[]   asm("_binary_cesiumman_a3d_end");

namespace a3d_demo
{
namespace
{

const char* TAG = "a3d_demo";

// --- the comparison viewport ----------------------------------------------
// Fixed on every board. See the file comment.
constexpr int BENCH_W = 240;
constexpr int BENCH_H = 320;

// A tile is width * TILE_H * 2 bytes of colour and the same again of depth,
// per worker. 40 rows keeps a 480-wide panel at 38 KB a buffer, which is what
// a part with 512 KB of SRAM and no PSRAM can actually spare once the bind
// arrays (32 bytes per vertex) are accounted for.
constexpr int TILE_H = 40;

constexpr int MAX_SLOTS = 2;
constexpr int MAX_TILES = 64;

// --- colours (RGB565) ------------------------------------------------------
constexpr uint16_t COL_BG   = 0x1082;
constexpr uint16_t COL_TEXT = 0xFFFF;
constexpr uint16_t COL_DIM  = 0x8410;
constexpr uint16_t COL_BAR  = 0x2124;
constexpr uint16_t COL_HIT  = 0x03BF;

// --- the models ------------------------------------------------------------
struct ModelDesc
    {
    const char* name;
    const uint8_t* begin;
    const uint8_t* end;
    };

const ModelDesc kModels[] = {
    { "FOX",       fox_start,    fox_end },
    { "CESIUMMAN", cesium_start, cesium_end },
};
constexpr int NB_MODELS = (int)(sizeof(kModels) / sizeof(kModels[0]));

// --- one renderer per worker ----------------------------------------------
struct Slot
    {
    a3d::SoftBackend renderer;
    uint16_t* fb = nullptr;        // DMA-capable: a panel may read it directly
    uint16_t* zb = nullptr;
    uint16_t* scratch = nullptr;   // per-tile index list
    int scratchTri = 0;
    uint64_t clearUs = 0;
    uint64_t rasterUs = 0;
    uint64_t swapUs = 0;
    uint64_t blitUs = 0;
    };
Slot g_slots[MAX_SLOTS];

a3d::FreeRtosExecutor* g_exec1 = nullptr;   // one worker: spawns nothing, runs inline
a3d::FreeRtosExecutor* g_exec2 = nullptr;   // two workers, where the part has two cores
a3d::IJobExecutor* g_exec = nullptr;
int g_nbSlots = 1;
// How many slots were actually ALLOCATED. On a single-core part that is one,
// and the difference is 38 KB of internal SRAM on a 480-wide panel - which is
// not a rounding error on a board with 512 KB and no PSRAM.
int g_nbAllocSlots = 1;

Display g_disp;
void* g_blitMutex = nullptr;    // SemaphoreHandle_t; a panel bus is not re-entrant

// --- viewport, chosen at runtime ------------------------------------------
int g_viewW = BENCH_W, g_viewH = BENCH_H;
int g_nbTiles = (BENCH_H + TILE_H - 1) / TILE_H;
int g_maxViewW = BENCH_W;

// --- the model currently bound --------------------------------------------
a3d::AssetImage g_image;
a3d::SceneRuntime g_scene;
a3d::AnimationPlayer g_player;
a3d::TileBinner* g_binners = nullptr;
uint32_t g_nbDrawables = 0;
uint8_t* g_psramCopy = nullptr;
bool g_haveModel = false;
int g_modelIndex = -1;
uint32_t g_totalVerts = 0, g_totalTris = 0;
size_t g_bindInternal = 0;
size_t g_bindPsram = 0;

float g_centre[3] = {0, 0, 0};
float g_radius = 1.0f;

// --- camera ---------------------------------------------------------------
float g_yaw = 0.6f, g_pitch = 0.25f, g_dist = 2.6f;
constexpr float kDistMin = 1.5f, kDistMax = 8.0f, kPitchLimit = 1.45f;

// --- per-frame switches read by the tile worker ---------------------------
bool g_present = false;
bool g_drawHud = false;
float g_viewProj[16];

uint64_t nowUs() { return (uint64_t)esp_timer_get_time(); }

size_t freeInternal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
size_t freePsram()    { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
bool   havePsram()    { return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0; }

int tileRows(int tile)
    {
    const int y0 = tile * TILE_H;
    const int rows = g_viewH - y0;
    return (rows > TILE_H) ? TILE_H : rows;
    }

// ---------------------------------------------------------------------------
// Text and boxes drawn straight into a TILE. `yAbs` is a screen row; the glyph
// clipper does the rest, so a caption straddling two tiles comes out in both.
// ---------------------------------------------------------------------------
void tileText(uint16_t* fb, int tileTop, int rows, int x, int yAbs, const char* s, uint16_t col)
    {
    a3d::drawText5x7(fb, g_viewW, g_viewW, rows, x, yAbs - tileTop, s, col);
    }

void tileRect(uint16_t* fb, int tileTop, int rows, int x, int yAbs, int w, int h, uint16_t col)
    {
    int y = yAbs - tileTop;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_viewW) w = g_viewW - x;
    if (y + h > rows) h = rows - y;
    for (int r = 0; r < h; r++)
        {
        uint16_t* row = fb + (size_t)(y + r) * g_viewW + x;
        for (int i = 0; i < w; i++) row[i] = col;
        }
    }

// ---------------------------------------------------------------------------
// Model loading
// ---------------------------------------------------------------------------
void unloadModel()
    {
    if (g_binners != nullptr)
        {
        for (uint32_t i = 0; i < g_nbDrawables; i++) g_binners[i].release();
        heap_caps_free(g_binners);
        g_binners = nullptr;
        }
    g_nbDrawables = 0;
    for (int i = 0; i < g_nbAllocSlots; i++)
        {
        heap_caps_free(g_slots[i].scratch);
        g_slots[i].scratch = nullptr;
        g_slots[i].scratchTri = 0;
        }
    g_scene.release();
    g_image = a3d::AssetImage();
    if (g_psramCopy != nullptr) { heap_caps_free(g_psramCopy); g_psramCopy = nullptr; }
    g_haveModel = false;
    g_modelIndex = -1;
    }

/**
 * @param psram copy the container into PSRAM instead of opening it in
 *              memory-mapped flash. What this changes is not the load: it is
 *              that the rasterizer then samples the texture out of PSRAM
 *              rather than through the flash cache, for every textured pixel
 *              of every frame.
 */
bool loadModel(int index, bool psram)
    {
    unloadModel();
    if (index < 0 || index >= NB_MODELS) return false;

    const ModelDesc& md = kModels[index];
    const size_t bytes = (size_t)(md.end - md.begin);
    const uint8_t* data = md.begin;

    if (psram)
        {
        if (!havePsram()) return false;
        g_psramCopy = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_psramCopy == nullptr) { ESP_LOGE(TAG, "no PSRAM for %s", md.name); return false; }
        memcpy(g_psramCopy, md.begin, bytes);
        data = g_psramCopy;
        }

    if (!g_image.open(data, bytes))
        { ESP_LOGE(TAG, "%s is not a valid container", md.name); return false; }

    // Where the bind arrays LAND is measured, not assumed. a3d asks for them
    // with plain operator new, and on a part with PSRAM the IDF heap sends any
    // allocation above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB by default)
    // to PSRAM. So a 3,273-vertex model quietly puts its 105 KB of decoded
    // vertices in the slow memory, and nothing says so. Both deltas are
    // reported for exactly that reason.
    const size_t beforeInt = freeInternal();
    const size_t beforePs  = freePsram();
    if (!g_scene.bind(g_image, g_slots[0].renderer))
        { ESP_LOGE(TAG, "%s would not bind", md.name); unloadModel(); return false; }
    g_bindInternal = (beforeInt > freeInternal()) ? (beforeInt - freeInternal()) : 0;
    g_bindPsram    = (beforePs  > freePsram())    ? (beforePs  - freePsram())    : 0;
    g_player.bind(g_image);

    g_nbDrawables = g_scene.drawableCount();
    if (g_nbDrawables == 0) { unloadModel(); return false; }

    g_binners = (a3d::TileBinner*)heap_caps_calloc(
        g_nbDrawables, sizeof(a3d::TileBinner), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (g_binners == nullptr) { ESP_LOGE(TAG, "no RAM for binners"); unloadModel(); return false; }

    uint32_t maxTri = 0;
    g_totalVerts = 0; g_totalTris = 0;
    for (uint32_t d = 0; d < g_nbDrawables; d++)
        {
        new (&g_binners[d]) a3d::TileBinner();
        uint32_t v = 0, t = 0;
        g_scene.drawableSize(d, v, t);
        if (t > maxTri) maxTri = t;
        g_totalVerts += v; g_totalTris += t;
        g_binners[d].setCulling(-1);
        if (!g_binners[d].configure(g_viewW, g_viewH, g_viewW, TILE_H, (int)t, (int)v))
            {
            ESP_LOGE(TAG, "binner would not configure for part %u", (unsigned)d);
            unloadModel();
            return false;
            }
        }

    const uint32_t caps = havePsram() ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                      : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    for (int i = 0; i < g_nbAllocSlots; i++)
        {
        g_slots[i].scratch = (uint16_t*)heap_caps_malloc(
            (size_t)maxTri * 3 * sizeof(uint16_t), caps);
        g_slots[i].scratchTri = (int)maxTri;
        if (g_slots[i].scratch == nullptr)
            { ESP_LOGE(TAG, "no RAM for scratch"); unloadModel(); return false; }
        }

    // Frame the model from its own bounds. The order matters and is the one
    // trap this sequence has: worldBounds() reads the SKINNED vertices, so it
    // must come after skinAll() or it silently reports the previous pose.
    g_player.selectClip(0);
    g_player.seek(0.0f);
    g_player.apply(g_scene);
    g_scene.updateWorldTransforms();
    g_scene.skinAll(*g_exec);

    float mn[3], mx[3];
    if (g_scene.worldBounds(mn, mx))
        {
        float ext = 0.0f;
        for (int i = 0; i < 3; i++)
            {
            g_centre[i] = 0.5f * (mn[i] + mx[i]);
            const float half = 0.5f * (mx[i] - mn[i]);
            if (half > ext) ext = half;
            }
        g_radius = (ext > 1e-4f) ? ext : 1.0f;
        }

    g_haveModel = true;
    g_modelIndex = index;
    return true;
    }

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void configureCamera(a3d::SoftBackend& r)
    {
    const float dist = g_radius * g_dist;
    r.setViewportSize(g_viewW, g_viewH);
    r.setPerspective(45.0f, (float)g_viewW / (float)g_viewH,
                     g_radius * 0.05f, dist + g_radius * 8.0f);
    const float cp = cosf(g_pitch);
    const float eye[3] = { g_centre[0] + dist * cp * sinf(g_yaw),
                           g_centre[1] + dist * sinf(g_pitch),
                           g_centre[2] + dist * cp * cosf(g_yaw) };
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    r.setLookAt(eye, g_centre, up);
    }

const char* clipName()
    {
    if (!g_player.bound()) return "-";
    const a3d::fmt::ClipEntry* clips = g_image.clips();
    if (clips == nullptr) return "-";
    const uint32_t off = clips[g_player.clipIndex()].name_off;
    return (off != a3d::fmt::NONE32) ? g_image.string(off) : "-";
    }

// Four buttons rather than a pinch, because one of these boards has a
// single-touch controller and a gesture it cannot report is worse than a
// button: it compiles, reads one finger, and never fires.
constexpr int BAR_H = 34;
constexpr int NB_BUTTONS = 4;
const char* kButtonLabels[NB_BUTTONS] = { "MODEL", "CLIP", "-", "+" };
// On a round panel the buttons are a centred list, and a list has room for
// words. Same four actions, same order.
const char* kRoundLabels[NB_BUTTONS] = { "MODEL", "CLIP", "ZOOM OUT", "ZOOM IN" };

int barY() { return g_viewH - BAR_H; }

// --- round-panel geometry --------------------------------------------------
constexpr int EDGE_PAD = 12;   // clear of the bezel, along the chord

/** Leftmost x still on the glass at row `yAbs`, plus a little air. */
int insetAt(int yAbs)
    {
    if (!g_disp.round) return 4;
    const float r = 0.5f * (float)g_viewW;
    const float dy = (float)yAbs - r;
    const float inside = r * r - dy * dy;
    if (inside <= 0.0f) return (int)r;          // past the top or bottom
    return (int)r - (int)sqrtf(inside) + EDGE_PAD;
    }

void tileTextCentred(uint16_t* fb, int tileTop, int rows, int yAbs,
                     const char* s, uint16_t col)
    {
    const int tw = (int)strlen(s) * a3d::kFontAdvance;
    int x = (g_viewW - tw) / 2;
    if (x < insetAt(yAbs)) x = insetAt(yAbs);
    tileText(fb, tileTop, rows, x, yAbs, s, col);
    }

// --- the round overlay menu -----------------------------------------------
// A bar along the bottom edge of a circle is off the glass, so on a round
// panel the buttons are a centred list that a tap brings up and a timeout
// takes away.
constexpr int ROW_H = 46;
constexpr int ROW_GAP = 6;
int menuTop() { return (g_viewH - NB_BUTTONS * (ROW_H + ROW_GAP)) / 2; }
int menuRowY(int i) { return menuTop() + i * (ROW_H + ROW_GAP); }

bool g_menuVisible = false;

char g_hud1[64] = {0};
char g_hud2[64] = {0};
int  g_pressed = -1;

int buttonAt(int x, int y)
    {
    if (g_disp.round)
        {
        if (!g_menuVisible) return -1;
        for (int i = 0; i < NB_BUTTONS; i++)
            {
            const int y0 = menuRowY(i);
            if (y < y0 || y >= y0 + ROW_H) continue;
            const int x0 = insetAt(y0 + ROW_H / 2);
            if (x >= x0 && x <= g_viewW - x0) return i;
            }
        return -1;
        }
    if (y < barY()) return -1;
    const int i = (x * NB_BUTTONS) / g_viewW;
    return (i >= 0 && i < NB_BUTTONS) ? i : -1;
    }

void drawHudRound(uint16_t* fb, int tileTop, int rows)
    {
    // Top centre. Two lines, both inside the chord at their own row.
    tileTextCentred(fb, tileTop, rows, 54, g_hud1, COL_TEXT);
    tileTextCentred(fb, tileTop, rows, 70, g_hud2, COL_DIM);

    if (!g_menuVisible)
        {
        tileTextCentred(fb, tileTop, rows, g_viewH - 56, "TAP FOR MENU", COL_DIM);
        return;
        }

    for (int i = 0; i < NB_BUTTONS; i++)
        {
        const int y0 = menuRowY(i);
        // The bar follows the same chord as its label, so a selected row is a
        // shape that fits the circle rather than a rectangle with its ends cut
        // off by the bezel.
        const int x0 = insetAt(y0 + ROW_H / 2);
        const int w = g_viewW - 2 * x0;
        if (w <= 0) continue;
        tileRect(fb, tileTop, rows, x0, y0, w, ROW_H,
                 (i == g_pressed) ? COL_HIT : COL_BAR);
        const int tw = (int)strlen(kRoundLabels[i]) * a3d::kFontAdvance;
        tileText(fb, tileTop, rows, x0 + (w - tw) / 2, y0 + ROW_H / 2 + 4,
                 kRoundLabels[i], COL_TEXT);
        }
    }

void drawHudRect(uint16_t* fb, int tileTop, int rows)
    {
    tileText(fb, tileTop, rows, 4, 12, g_hud1, COL_TEXT);
    tileText(fb, tileTop, rows, 4, 24, g_hud2, COL_DIM);

    const int bw = g_viewW / NB_BUTTONS;
    for (int i = 0; i < NB_BUTTONS; i++)
        {
        tileRect(fb, tileTop, rows, i * bw + 1, barY() + 2, bw - 2, BAR_H - 4,
                 (i == g_pressed) ? COL_HIT : COL_BAR);
        const int tw = (int)strlen(kButtonLabels[i]) * a3d::kFontAdvance;
        tileText(fb, tileTop, rows, i * bw + (bw - tw) / 2, barY() + 22,
                 kButtonLabels[i], COL_TEXT);
        }
    }

void drawHud(uint16_t* fb, int tileTop, int rows)
    {
    if (g_disp.round) drawHudRound(fb, tileTop, rows);
    else              drawHudRect(fb, tileTop, rows);
    }

/** RGB565 halfword swap, in place. The tile is redrawn next frame anyway. */
void swapTile(uint16_t* px, int n)
    {
    for (int i = 0; i < n; i++)
        {
        const uint16_t v = px[i];
        px[i] = (uint16_t)((v >> 8) | (v << 8));
        }
    }

void drawOneTile(void* ctx, int slot, int tile)
    {
    (void)ctx;
    Slot& s = g_slots[slot];
    const int y0 = tile * TILE_H;
    const int rows = tileRows(tile);
    if (rows <= 0) return;

    s.renderer.setTarget(s.fb, g_viewW, rows, g_viewW);
    s.renderer.setOffset(0, y0);

    const uint64_t c0 = nowUs();
    const int px = g_viewW * rows;
    for (int i = 0; i < px; i++) s.fb[i] = COL_BG;
    s.renderer.clearZBuffer();
    const uint64_t c1 = nowUs();

    for (uint32_t d = 0; d < g_nbDrawables; d++)
        g_scene.drawDrawableTile(d, g_binners[d], tile, s.scratch, s.scratchTri,
                                 a3d::Shading::Gouraud,
                                 a3d::TextureMode::Perspective,
                                 &s.renderer);
    const uint64_t c2 = nowUs();
    s.clearUs += (c1 - c0);
    s.rasterUs += (c2 - c1);

    if (g_drawHud) drawHud(s.fb, y0, rows);

    // The blit belongs HERE, not in a loop after the dispatch. A slot owns one
    // tile buffer and overwrites it as soon as it is handed the next tile, so a
    // copy made afterwards would send whatever each slot happened to finish on.
    if (g_present && g_disp.sendTile != nullptr)
        {
        if (g_disp.swapBytes)
            {
            const uint64_t w0 = nowUs();
            swapTile(s.fb, px);
            s.swapUs += (nowUs() - w0);
            }
        const uint64_t b0 = nowUs();
        // One bus, so this is the one place the workers have to take turns.
        if (g_blitMutex != nullptr)
            xSemaphoreTake((SemaphoreHandle_t)g_blitMutex, portMAX_DELAY);
        g_disp.sendTile(s.fb, 0, y0, g_viewW, rows);
        if (g_blitMutex != nullptr)
            xSemaphoreGive((SemaphoreHandle_t)g_blitMutex);
        s.blitUs += (nowUs() - b0);
        }
    }

struct FrameTimes
    {
    double totalUs = 0, animUs = 0, skinUs = 0, binUs = 0, drawUs = 0;
    // Summed over slots, so these are CORE microseconds while drawUs is wall
    // clock. On two workers they add up to about twice drawUs, and that is
    // exactly what the second core bought.
    double clearUs = 0, rasterUs = 0, swapUs = 0, blitUs = 0;
    };

/** One frame. `dtMs` of 0 leaves the pose alone, which is the static case. */
FrameTimes renderFrame(float dtMs)
    {
    FrameTimes t;
    const uint64_t t0 = nowUs();

    if (dtMs > 0.0f && g_player.bound())
        {
        g_player.advance(dtMs);
        g_player.apply(g_scene);
        g_scene.updateWorldTransforms();
        }
    const uint64_t t1 = nowUs();

    if (dtMs > 0.0f) g_scene.skinAll(*g_exec);
    const uint64_t t2 = nowUs();

    for (int i = 0; i < g_nbSlots; i++) configureCamera(g_slots[i].renderer);
    // Bin BEFORE the workers are released: every slot reads the result and none
    // writes it, so there is nothing to lock and the work happens once.
    g_scene.viewProjection(g_viewProj);
    for (uint32_t d = 0; d < g_nbDrawables; d++)
        g_scene.binDrawable(d, g_binners[d], g_viewProj);
    const uint64_t t3 = nowUs();

    for (int i = 0; i < g_nbAllocSlots; i++)
        {
        g_slots[i].clearUs = 0; g_slots[i].rasterUs = 0;
        g_slots[i].swapUs = 0; g_slots[i].blitUs = 0;
        }
    a3d::forEachTile(*g_exec, g_nbTiles, drawOneTile, nullptr);
    const uint64_t t4 = nowUs();

    t.animUs = (double)(t1 - t0);
    t.skinUs = (double)(t2 - t1);
    t.binUs  = (double)(t3 - t2);
    t.drawUs = (double)(t4 - t3);
    for (int i = 0; i < g_nbAllocSlots; i++)
        {
        t.clearUs  += (double)g_slots[i].clearUs;
        t.rasterUs += (double)g_slots[i].rasterUs;
        t.swapUs   += (double)g_slots[i].swapUs;
        t.blitUs   += (double)g_slots[i].blitUs;
        }
    t.totalUs = (double)(t4 - t0);
    return t;
    }

void useExecutor(int workers)
    {
    g_exec = (workers >= 2 && g_exec2 != nullptr) ? (a3d::IJobExecutor*)g_exec2
                                                  : (a3d::IJobExecutor*)g_exec1;
    g_nbSlots = a3d::tileSlotCount(*g_exec, g_nbTiles);
    if (g_nbSlots < 1) g_nbSlots = 1;
    if (g_nbSlots > g_nbAllocSlots) g_nbSlots = g_nbAllocSlots;
    }

void setViewport(int w, int h)
    {
    unloadModel();          // the binners are configured for the old size
    g_viewW = w; g_viewH = h;
    g_nbTiles = (h + TILE_H - 1) / TILE_H;
    if (g_nbTiles > MAX_TILES) g_nbTiles = MAX_TILES;
    for (int i = 0; i < g_nbAllocSlots; i++) g_slots[i].renderer.setViewportSize(w, h);
    }

void report()
    {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("[A3D] build date=\"%s\" time=\"%s\" idf=%s\n", __DATE__, __TIME__, IDF_VER);
    printf("[A3D] board target=%s name=%s cores=%d cpu_mhz=%d fpu=%d psram=%d max_workers=%d\n",
           CONFIG_IDF_TARGET, A3D_BOARD_NAME, chip.cores,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, a3d::BoardInfo::hasFpu ? 1 : 0,
           havePsram() ? 1 : 0, (int)A3D_MAX_WORKERS);
    printf("[A3D] bench_view w=%d h=%d tile_h=%d slots=%d tile_bytes=%u\n",
           BENCH_W, BENCH_H, TILE_H, g_nbAllocSlots,
           (unsigned)((size_t)g_maxViewW * TILE_H * sizeof(uint16_t)));
    if (g_disp.sendTile != nullptr)
        printf("[A3D] panel kind=%s w=%d h=%d clk_mhz=%d swap_bytes=%d\n",
               g_disp.kind, g_disp.width, g_disp.height, g_disp.clockMHz,
               g_disp.swapBytes ? 1 : 0);
    else
        printf("[A3D] panel kind=none\n");
    printf("[A3D] mem internal_free=%u psram_free=%u\n",
           (unsigned)freeInternal(), (unsigned)freePsram());
    }

// ---------------------------------------------------------------------------
// The viewer
// ---------------------------------------------------------------------------
void runViewer()
    {
    setViewport(g_disp.width, g_disp.height);
    useExecutor(A3D_MAX_WORKERS);
    if (!loadModel(0, false))
        {
        ESP_LOGE(TAG, "nothing to show");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
        }

    g_present = true;
    g_drawHud = true;
    g_yaw = 0.6f; g_pitch = 0.25f; g_dist = 2.6f;

    bool wasDown = false, dragged = false, pinched = false;
    int lastX = 0, lastY = 0, downButton = -1, lastN = 0;
    // Pinch is a RATIO against the separation when the second finger landed,
    // never an accumulated delta, so it cannot drift: putting the fingers back
    // where they started puts the zoom back where it started, however far it
    // wandered in between.
    float pinchRef = 0.0f, distRef = 0.0f;
    uint64_t lastMs = nowUs() / 1000;
    double fps = 0.0;
    bool userDrove = false;
    // Round panels only: the menu is an overlay, so it has to go away again.
    uint64_t lastTouchMs = nowUs() / 1000;
    constexpr uint64_t MENU_HIDE_MS = 5000;

    while (true)
        {
        int tx = 0, ty = 0, tx2 = 0, ty2 = 0;
        // touchRead2 wins when a board offers it; touchRead is the one-finger
        // fallback, and a board with neither simply never drives the camera.
        // For n of 0 or 1 the state machine below is exactly what it was before
        // pinch existed, which is why the boards that do not implement
        // touchRead2 did not have to change.
        int n = 0;
        if (g_disp.touchRead2 != nullptr)
            n = g_disp.touchRead2(&tx, &ty, &tx2, &ty2);
        else if (g_disp.touchRead != nullptr)
            n = g_disp.touchRead(&tx, &ty) ? 1 : 0;
        if (n < 0) n = 0;
        if (n > 2) n = 2;
        const bool down = (n > 0);

        // A CHANGE IN THE NUMBER OF CONTACTS RE-ANCHORS EVERYTHING. Without
        // this, putting a second finger down or lifting one hands the rotate a
        // delta measured against a point that belonged to a different gesture,
        // and the model jumps.
        if (n != lastN && n > 0)
            {
            lastX = tx; lastY = ty;
            if (lastN == 0)
                {
                dragged = false; pinched = false;
                downButton = buttonAt(tx, ty);
                g_pressed = downButton;
                }
            if (n == 2)
                {
                const float ddx = (float)(tx2 - tx), ddy = (float)(ty2 - ty);
                pinchRef = sqrtf(ddx * ddx + ddy * ddy);
                distRef  = g_dist;
                pinched  = true;
                // A second finger is not a button press, whatever the first
                // one landed on.
                downButton = -1;
                g_pressed = -1;
                }
            }
        else if (n == 2)
            {
            const float ddx = (float)(tx2 - tx), ddy = (float)(ty2 - ty);
            const float d = sqrtf(ddx * ddx + ddy * ddy);
            // Below a few pixels the ratio is noise divided by noise.
            if (pinchRef > 8.0f && d > 1.0f)
                {
                // g_dist is a distance, so it moves OPPOSITE to the zoom:
                // fingers apart (d rising) means closer in, means smaller dist.
                g_dist = distRef * (pinchRef / d);
                if (g_dist < kDistMin) g_dist = kDistMin;
                if (g_dist > kDistMax) g_dist = kDistMax;
                userDrove = true;
                }
            }
        else if (n == 1)
            {
            const int dx = tx - lastX, dy = ty - lastY;
            if ((dx * dx + dy * dy) > 9) dragged = true;
            if (downButton < 0)
                {
                g_yaw   -= (float)dx * 0.012f;
                g_pitch += (float)dy * 0.012f;
                if (g_pitch >  kPitchLimit) g_pitch =  kPitchLimit;
                if (g_pitch < -kPitchLimit) g_pitch = -kPitchLimit;
                userDrove = true;
                }
            lastX = tx; lastY = ty;
            }
        else if (!down && wasDown)
            {
            // A tap on empty glass toggles the overlay menu. A DRAG does not:
            // it is how the model is turned, and a menu that appeared every
            // time the user let go of the model would be unusable.
            if (g_disp.round && downButton < 0 && !dragged && !pinched)
                g_menuVisible = !g_menuVisible;

            if (downButton >= 0 && !dragged && buttonAt(lastX, lastY) == downButton)
                {
                switch (downButton)
                    {
                    case 0:
                        {
                        const int next = (g_modelIndex + 1) % NB_MODELS;
                        const float yaw = g_yaw, pitch = g_pitch, dist = g_dist;
                        if (loadModel(next, false))
                            { g_yaw = yaw; g_pitch = pitch; g_dist = dist; }
                        break;
                        }
                    case 1:
                        if (g_player.clipCount() > 0)
                            g_player.selectClip((g_player.clipIndex() + 1) % g_player.clipCount());
                        break;
                    case 2: g_dist *= 1.25f; if (g_dist > kDistMax) g_dist = kDistMax; break;
                    case 3: g_dist *= 0.8f;  if (g_dist < kDistMin) g_dist = kDistMin; break;
                    }
                }
            g_pressed = -1;
            downButton = -1;
            pinched = false;
            }
        wasDown = down;
        lastN = n;

        const uint64_t now = nowUs() / 1000;
        if (down) lastTouchMs = now;
        if (g_menuVisible && (now - lastTouchMs) > MENU_HIDE_MS) g_menuVisible = false;
        float dt = (float)(now - lastMs);
        lastMs = now;
        if (dt <= 0.0f) dt = 1.0f;
        if (dt > 200.0f) dt = 200.0f;

        // A slow idle turn until the first drag, then it is the user's model.
        if (!userDrove) g_yaw += dt * 0.0004f;

        snprintf(g_hud1, sizeof(g_hud1), "%s  %uT %uV",
                 kModels[g_modelIndex].name, (unsigned)g_totalTris, (unsigned)g_totalVerts);
        snprintf(g_hud2, sizeof(g_hud2), "%.1f FPS  %d CORE  %s",
                 fps, g_nbSlots, clipName());

        const FrameTimes t = renderFrame(dt);
        const double ms = t.totalUs / 1000.0;
        const double inst = (ms > 0) ? 1000.0 / ms : 0.0;
        fps = (fps <= 0.0) ? inst : (fps * 0.9 + inst * 0.1);
        }
    }

bool allocSlots()
    {
    // Exactly as many as the executor can ever hand out. tileSlotCount() is the
    // authority on that, and the point of asking it rather than a constant is
    // that a single-core part then never pays for a second tile framebuffer.
    g_nbAllocSlots = a3d::tileSlotCount(*g_exec, g_nbTiles);
    if (g_nbAllocSlots < 1) g_nbAllocSlots = 1;
    if (g_nbAllocSlots > MAX_SLOTS) g_nbAllocSlots = MAX_SLOTS;

    for (int i = 0; i < g_nbAllocSlots; i++)
        {
        // Sized for the WIDEST viewport this build will ever use, because the
        // benchmark and the viewer are different sizes and the buffers are
        // allocated once.
        const size_t bytes = (size_t)g_maxViewW * TILE_H * sizeof(uint16_t);
        // DMA-capable because a panel may read this buffer directly.
        g_slots[i].fb = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        g_slots[i].zb = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (g_slots[i].fb == nullptr || g_slots[i].zb == nullptr)
            {
            ESP_LOGE(TAG, "no internal RAM for slot %d (%u bytes x2)", i, (unsigned)bytes);
            return false;
            }
        g_slots[i].renderer.setTarget(g_slots[i].fb, g_maxViewW, TILE_H, g_maxViewW);
        // A loaded model is an arbitrary shape, so the depth test is not
        // optional here. Checked rather than assumed: setZBuffer(nullptr) is a
        // supported FAST PATH, so a failed allocation would not crash - it
        // would quietly draw the model with its back faces showing through.
        g_slots[i].renderer.setZBuffer(g_slots[i].zb);
        g_slots[i].renderer.setCulling(-1);
        g_slots[i].renderer.setShading(a3d::Shading::Gouraud, a3d::TextureMode::Perspective);
        g_slots[i].renderer.setLightDirection(-0.4f, -0.6f, -1.0f);
        }
    return true;
    }

} // namespace

void run(const Display& display)
    {
    g_disp = display;
    g_maxViewW = (display.width > BENCH_W) ? display.width : BENCH_W;

    // The executors are built BEFORE the buffers, because they are what decides
    // how many there are. One worker spawns no task and runs every tile inline;
    // that is a supported target, not a fallback.
    g_exec1 = new a3d::FreeRtosExecutor(1);
    if (A3D_MAX_WORKERS > 1) g_exec2 = new a3d::FreeRtosExecutor(2, 8192, 5);
    g_blitMutex = xSemaphoreCreateMutex();
    useExecutor(A3D_MAX_WORKERS);

    // Allocated before the report, so the report can state how many slots this
    // build actually has rather than how many the array could hold.
    const bool slotsOk = allocSlots();
    report();
    if (!slotsOk)
        {
        printf("[A3D] tile buffers would not allocate; halting\n");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
        }

    if (display.fill != nullptr) display.fill(COL_BG);

    // No benchmark. This copy of the demo goes straight to the viewer: it is
    // here to SHOW a3d on a panel, and a three-minute sweep between power-on
    // and the first picture is the wrong first impression for that.
    // The measuring version is the original, examples/Waveshare_*.
    if (display.sendTile != nullptr)
        {
        runViewer();
        }
    else
        {
        printf("[A3D] headless build, nothing to display; halting\n");
        while (true) vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

} // namespace a3d_demo
