// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file main.cpp
 * @brief a3d, the smallest complete thing: three shapes, one light, one panel.
 *
 * There are no asset files here, no skeletal animation, no benchmark and no
 * frame counter. Three primitives are generated at boot, lit by one
 * directional light, and turned slowly. That is all, and it is on purpose:
 * this is the file to read first.
 *
 * Everything you would want to change is in ONE place - the SETTINGS block
 * below. Change a number there, reflash, and watch what moves.
 *
 * The five things a3d needs, in the order this file does them:
 *
 *   1. A TARGET     where pixels go. Here it is a tile, and section 1 says why.
 *   2. A CAMERA     viewport, projection, view.
 *   3. A LIGHT      direction, material, and a shading model.
 *   4. GEOMETRY     vertices, normals and indices - generated, not loaded.
 *   5. A FRAME      one model matrix per shape, submit, blit.
 *
 * For the same renderer driving loaded models, skeletal animation and two
 * cores, see ../ESP32-S3-LCD2.
 */

#include "a3d/a3d_primitives.h"
#include "a3d/a3d_types.h"
#include "a3d/a3d_vec.h"
#include "backends/soft/a3d_soft_backend.h"

#include "s3_panel.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <string.h>

namespace
{

const char* TAG = "a3d_basic";

// ===========================================================================
// SETTINGS - the whole point of this example. Everything below this block is
// machinery; everything in it is a decision.
// ===========================================================================

// --- the camera ------------------------------------------------------------
constexpr float FOV_DEGREES  = 45.0f;   // vertical field of view. 30 is a long
                                        // lens, 70 is wide and distorts edges.
constexpr float CAMERA_DIST  = 7.0f;    // how far back the eye sits
constexpr float CAMERA_HEIGHT = 1.6f;   // how far above the shapes it looks down from
constexpr float NEAR_PLANE   = 0.5f;    // closer than this is clipped away
constexpr float FAR_PLANE    = 40.0f;   // NEAR and FAR set the depth buffer's
                                        // precision: a huge range wastes it

// --- the light -------------------------------------------------------------
// A DIRECTION, not a position: this is a distant light like the sun, so only
// the direction matters. It points FROM the light TOWARDS the scene, which is
// why z is negative here - the light comes from behind the camera.
constexpr float LIGHT_X = -0.4f;
constexpr float LIGHT_Y = -0.6f;
constexpr float LIGHT_Z = -1.0f;

// --- the material ----------------------------------------------------------
// AMBIENT is the floor: what a surface facing away from the light still shows.
// At 0.0 the dark side is pure black and the shape loses its silhouette.
// DIFFUSE is how much the direct light adds. AMBIENT + DIFFUSE above 1.0
// clips to white on the surfaces facing the light.
constexpr float MAT_AMBIENT  = 0.25f;
constexpr float MAT_DIFFUSE  = 0.85f;
// SPECULAR is the highlight; the EXPONENT is how tight it is. 0 turns it off,
// a low exponent gives a broad sheen, a high one a small hard dot.
constexpr float MAT_SPECULAR = 0.45f;
constexpr int   MAT_SPEC_EXP = 24;

// --- the scene -------------------------------------------------------------
constexpr float SPIN_DEG_PER_SEC = 30.0f;
constexpr uint16_t BACKGROUND    = 0x1082;   // RGB565, near-black

// How round the shapes are. More sectors and stacks is smoother and costs
// triangles: a sphere is sectors * stacks * 2 of them.
constexpr int SPHERE_SECTORS = 24;
constexpr int SPHERE_STACKS  = 16;
constexpr int CONE_SECTORS   = 24;

// ===========================================================================
// 1. THE TARGET
//
// A 240x320 RGB565 framebuffer is 153,600 bytes, and the depth buffer that
// goes with it is another 153,600. This board has about 270 KB of internal
// SRAM free at this point, so the pair does not fit - and pushing them to
// PSRAM makes every pixel write cross a slow bus.
//
// So a3d draws a TILE at a time: a full-width strip 40 rows tall, 19,200
// bytes, that is rendered and then pushed to the panel before the next strip
// is started. The rasterizer clips to whatever target it is given, so drawing
// the whole scene into each strip in turn produces the same picture as drawing
// it once into a framebuffer that does not exist.
//
// The subtlety worth remembering: setViewportSize() is the WHOLE screen and
// setTarget()/setOffset() are the strip. The projection has to know it is
// making a 240x320 image even while it is filling rows 80 to 119.
// ===========================================================================
constexpr int VIEW_W = a3d_s3::LCD_WIDTH;    // 240
constexpr int VIEW_H = a3d_s3::LCD_HEIGHT;   // 320
constexpr int TILE_H = 40;
constexpr int NB_TILES = VIEW_H / TILE_H;
static_assert(VIEW_H % TILE_H == 0, "TILE_H must divide the screen height");

a3d_s3::Panel g_panel;
a3d::SoftBackend g_renderer;
uint16_t* g_tile = nullptr;    // colour, DMA-capable: the panel reads it
uint16_t* g_depth = nullptr;   // depth, same size

// ===========================================================================
// 4. GEOMETRY
//
// a3d's primitive generators BUILD ARRAYS; they do not draw. That is what lets
// a generated shape go down exactly the same path as a loaded model - the same
// culling, the same shading, the same rasterizer.
//
// Each generator has a `...Size()` companion that reports the counts for the
// same parameters, so the buffers are allocated once, exactly, and never
// guessed at.
// ===========================================================================
struct Shape
    {
    float*    positions = nullptr;   // 3 floats per vertex
    float*    normals   = nullptr;   // 3 floats per vertex, unit length
    uint16_t* indices   = nullptr;   // 3 per triangle
    int nbVertices = 0;
    int nbTriangles = 0;
    };

Shape g_sphere, g_box, g_cylinder;

/**
 * Allocate the three streams a lit, untextured shape needs.
 *
 * Positions and normals come from the BACKEND, not from malloc. A backend is
 * allowed to want its vertices as objects of its own type rather than as a
 * bare float array, so asking it to allocate is what keeps that possible. The
 * rasterizer shipped here just returns plain floats.
 */
bool allocShape(Shape& s, a3d::MeshSize size)
    {
    s.nbVertices = size.vertices;
    s.nbTriangles = size.triangles;
    s.positions = g_renderer.allocVec3Array(size.vertices);
    s.normals   = g_renderer.allocVec3Array(size.vertices);
    s.indices   = (uint16_t*)heap_caps_malloc((size_t)size.triangles * 3 * sizeof(uint16_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return (s.positions != nullptr) && (s.normals != nullptr) && (s.indices != nullptr);
    }

bool buildShapes()
    {
    // --- a unit sphere ------------------------------------------------------
    if (!allocShape(g_sphere, a3d::sphereSize(SPHERE_SECTORS, SPHERE_STACKS))) return false;
    {
    // Any stream may be null to skip it. There is no texture here, so no
    // texcoords are generated and nothing pays for them.
    a3d::MeshSink sink;
    sink.positions = g_sphere.positions;
    sink.normals   = g_sphere.normals;
    sink.texcoords = nullptr;
    sink.indices   = g_sphere.indices;
    a3d::sphereMesh(SPHERE_SECTORS, SPHERE_STACKS, sink);
    }

    // --- a box, 1 x 1 x 1 (the arguments are HALF extents) ------------------
    if (!allocShape(g_box, a3d::boxSize())) return false;
    {
    a3d::MeshSink sink;
    sink.positions = g_box.positions;
    sink.normals   = g_box.normals;
    sink.indices   = g_box.indices;
    a3d::boxMesh(0.9f, 0.9f, 0.9f, sink);
    }

    // --- a cylinder: a truncated cone whose two radii are equal -------------
    // Passing 0.0f as the top radius here would make it a cone instead.
    if (!allocShape(g_cylinder, a3d::truncatedConeSize(CONE_SECTORS, true, true))) return false;
    {
    a3d::MeshSink sink;
    sink.positions = g_cylinder.positions;
    sink.normals   = g_cylinder.normals;
    sink.indices   = g_cylinder.indices;
    a3d::truncatedConeMesh(CONE_SECTORS, 0.8f, 0.8f, true, true, sink);
    }

    ESP_LOGI(TAG, "sphere %d tris, box %d tris, cylinder %d tris",
             g_sphere.nbTriangles, g_box.nbTriangles, g_cylinder.nbTriangles);
    return true;
    }

// ===========================================================================
// 5. A FRAME
// ===========================================================================

/**
 * Submit one shape.
 *
 * `TriangleBatch` names its streams separately because a mesh may index
 * positions and normals differently. These shapes have one normal per vertex,
 * so both index streams are the same array.
 */
void drawShape(const Shape& s, const a3d::Mat4& model,
               float r, float g, float b, a3d::Shading shading)
    {
    g_renderer.setModelMatrix(model.M);
    g_renderer.setMaterialColor(r, g, b);
    g_renderer.setShading(shading, a3d::TextureMode::None);

    a3d::TriangleBatch batch;
    batch.nbTriangles  = s.nbTriangles;
    batch.indPositions = s.indices;
    batch.positions    = s.positions;
    batch.indNormals   = s.indices;    // Gouraud needs these; Flat ignores them
    batch.normals      = s.normals;
    g_renderer.drawTriangles(batch);
    }

/** Every shape, at rotation `angle`. Called once per tile. */
void drawScene(float angleDeg)
    {
    a3d::Mat4 m;

    // PRE-multiplication, and the order is the opposite of what the names
    // suggest. setScale, then multRotate, then multTranslate builds T * R * S:
    // the shape is scaled, then spun about its OWN centre, then moved into
    // place. Doing the translate first would make it orbit the origin instead,
    // which does not look like a matrix bug - it looks like the wrong centre
    // of rotation.
    m.setScale(1.0f, 1.0f, 1.0f);
    m.multRotate(angleDeg, a3d::Vec3(0.0f, 1.0f, 0.0f));
    m.multTranslate(a3d::Vec3(-2.2f, 0.0f, 0.0f));
    drawShape(g_sphere, m, 0.35f, 0.65f, 1.00f, a3d::Shading::Gouraud);

    // The same box drawn FLAT instead of Gouraud: one colour per face, so the
    // edges stay hard. On a curved shape Flat shows the individual triangles;
    // on a box it is what you want, because a box really does have flat faces.
    m.setScale(1.0f, 1.0f, 1.0f);
    m.multRotate(angleDeg * 0.7f, a3d::Vec3(0.4f, 1.0f, 0.2f));
    m.multTranslate(a3d::Vec3(0.0f, 0.0f, 0.0f));
    drawShape(g_box, m, 1.00f, 0.72f, 0.25f, a3d::Shading::Flat);

    m.setScale(1.0f, 0.9f, 1.0f);
    m.multRotate(angleDeg * 1.3f, a3d::Vec3(0.2f, 1.0f, 0.0f));
    m.multTranslate(a3d::Vec3(2.2f, 0.0f, 0.0f));
    drawShape(g_cylinder, m, 0.45f, 0.90f, 0.55f, a3d::Shading::Gouraud);
    }

bool g_countFirstFrame = true;
int  g_firstFrameLit = 0;

void renderFrame(float angleDeg)
    {
    for (int tile = 0; tile < NB_TILES; tile++)
        {
        const int y0 = tile * TILE_H;

        // The strip moves; the viewport does not. setOffset tells the
        // rasterizer which rows of the 240x320 image this buffer holds.
        g_renderer.setTarget(g_tile, VIEW_W, TILE_H, VIEW_W);
        g_renderer.setOffset(0, y0);

        for (int i = 0; i < VIEW_W * TILE_H; i++) g_tile[i] = BACKGROUND;
        g_renderer.clearZBuffer();

        drawScene(angleDeg);

        // Counted on the FIRST frame only, and logged once. A black screen is
        // the usual first failure here, and this is what separates "the
        // renderer drew nothing" from "the renderer drew and the panel did not
        // show it" - two problems with the same symptom and no overlap in
        // where you would look.
        if (g_countFirstFrame)
            for (int i = 0; i < VIEW_W * TILE_H; i++)
                if (g_tile[i] != BACKGROUND) g_firstFrameLit++;

        // Blocking: the panel's DMA has finished by the time this returns, so
        // the next tile may reuse the buffer immediately.
        g_panel.sendTile(g_tile, 0, y0, VIEW_W, TILE_H);
        }

    if (g_countFirstFrame)
        {
        ESP_LOGI(TAG, "first frame: %d of %d pixels drawn",
                 g_firstFrameLit, VIEW_W * VIEW_H);
        g_countFirstFrame = false;
        }
    }

} // namespace

extern "C" void app_main(void)
    {
    ESP_ERROR_CHECK(g_panel.begin());

    // -----------------------------------------------------------------------
    // 1. THE TARGET (continued)
    // -----------------------------------------------------------------------
    const size_t tileBytes = (size_t)VIEW_W * TILE_H * sizeof(uint16_t);
    g_tile  = (uint16_t*)heap_caps_malloc(tileBytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    g_depth = (uint16_t*)heap_caps_malloc(tileBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (g_tile == nullptr || g_depth == nullptr)
        {
        ESP_LOGE(TAG, "no internal RAM for the tile buffers");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
        }

    // -----------------------------------------------------------------------
    // 2. THE CAMERA
    // -----------------------------------------------------------------------
    // The viewport is the whole screen even though the target is one strip.
    g_renderer.setViewportSize(VIEW_W, VIEW_H);
    g_renderer.setPerspective(FOV_DEGREES, (float)VIEW_W / (float)VIEW_H,
                              NEAR_PLANE, FAR_PLANE);

    const float eye[3]    = { 0.0f, CAMERA_HEIGHT, CAMERA_DIST };
    const float target[3] = { 0.0f, 0.0f, 0.0f };
    const float up[3]     = { 0.0f, 1.0f, 0.0f };
    g_renderer.setLookAt(eye, target, up);

    // Depth. Without it the shape drawn last wins every overlap, and a sphere
    // shows its own back faces through its front.
    g_renderer.setZBuffer(g_depth);

    // Which way a front face is wound. -1 is the direction that matches the
    // stored projection, whose y row is negated so clip space matches
    // framebuffer rows. Both directions give the same silhouette on a closed
    // shape; only the shading differs.
    g_renderer.setCulling(-1);

    // -----------------------------------------------------------------------
    // 3. THE LIGHT
    // -----------------------------------------------------------------------
    g_renderer.setLightDirection(LIGHT_X, LIGHT_Y, LIGHT_Z);
    g_renderer.setMaterialAmbient(MAT_AMBIENT);
    g_renderer.setMaterialDiffuse(MAT_DIFFUSE);
    g_renderer.setMaterialSpecular(MAT_SPECULAR);
    g_renderer.setMaterialSpecularExponent(MAT_SPEC_EXP);

    // -----------------------------------------------------------------------
    // 4. GEOMETRY
    // -----------------------------------------------------------------------
    if (!buildShapes())
        {
        ESP_LOGE(TAG, "could not build the shapes");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
        }

    // The backlight goes on only now: before the first frame exists, the panel
    // still holds whatever was in its memory at the last boot.
    g_panel.setBacklight(100);

    // -----------------------------------------------------------------------
    // 5. A FRAME, forever
    // -----------------------------------------------------------------------
    float angle = 0.0f;
    TickType_t last = xTaskGetTickCount();
    while (true)
        {
        const TickType_t now = xTaskGetTickCount();
        const float dt = (float)(now - last) * portTICK_PERIOD_MS / 1000.0f;
        last = now;

        // Turn by TIME, not by frame: a scene that advances a fixed step per
        // frame speeds up whenever it gets simpler to draw.
        angle += SPIN_DEG_PER_SEC * dt;
        if (angle >= 360.0f) angle -= 360.0f;

        renderFrame(angle);
        }
    }
