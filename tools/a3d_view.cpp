// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_view.cpp
 * @brief Render any .a3d container to an image on the host.
 *
 * Every device round trip so far has been cheaper when the same thing was made
 * to work on the host first. For a real asset - one nobody on this project
 * authored - the questions are visual: is it oriented correctly, is the texture
 * mapped the right way up, does the pose actually move? A hash cannot answer any
 * of those, and flashing to find out costs minutes each time.
 *
 *   a3d_view model.a3d out.png [--clip N] [--time MS] [--frames N] [--size N]
 *
 * The extension picks the format. Prefer .png: the .ppm this used to write
 * opens in neither macOS Preview nor Windows Photos, so the documented way to
 * check an asset ended in a file the person could not look at.
 */
#include "a3d/a3d_runtime.h"
#include "a3d/a3d_anim.h"
#include "backends/soft/a3d_soft_backend.h"
#include "loaders/a3d_load_stdio.h"


#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>


namespace {

void rgb565to888(uint16_t v, unsigned char out[3])
    {
    out[0] = (unsigned char)(((v >> 11) & 0x1F) * 255 / 31);
    out[1] = (unsigned char)(((v >> 5)  & 0x3F) * 255 / 63);
    out[2] = (unsigned char)(( v        & 0x1F) * 255 / 31);
    }

void writePPM(const char* path, const uint16_t* fb, int w, int h)
    {
    FILE* f = fopen(path, "wb");
    if (f == nullptr) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++)
        {
        unsigned char rgb[3];
        rgb565to888((uint16_t)fb[i], rgb);
        fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    }

// ---------------------------------------------------------------------------
// PNG, written here rather than linked
//
// WHY THIS IS WORTH 80 LINES. The documented way to check an asset before
// flashing it produced PPM, which neither macOS Preview nor Windows Photos
// opens. So the one step that saves a flash cycle ended in files a beginner
// cannot look at, and the check may as well not exist. PNG is what a double
// click opens.
//
// No zlib, because a renderer with no third-party dependency does not acquire
// one to write a preview. DEFLATE has a STORED mode - a block header and the
// bytes verbatim - which is a legal zlib stream and costs about 0.03% in size
// on an image this small. The only real work is the two checksums.
// ---------------------------------------------------------------------------
uint32_t crc32of(const unsigned char* p, size_t n, uint32_t crc)
    {
    static uint32_t table[256];
    static bool built = false;
    if (!built)
        {
        for (uint32_t i = 0; i < 256; i++)
            {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
            }
        built = true;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
    }

void be32(unsigned char* p, uint32_t v)
    { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
      p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v; }

void pngChunk(FILE* f, const char* type, const unsigned char* data, size_t n)
    {
    unsigned char len[4];
    be32(len, (uint32_t)n);
    fwrite(len, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n != 0) fwrite(data, 1, n, f);
    uint32_t crc = crc32of((const unsigned char*)type, 4, 0);
    if (n != 0) crc = crc32of(data, n, crc);
    unsigned char c[4];
    be32(c, crc);
    fwrite(c, 1, 4, f);
    }

void writePNG(const char* path, const uint16_t* fb, int w, int h)
    {
    FILE* f = fopen(path, "wb");
    if (f == nullptr) { fprintf(stderr, "cannot write %s\n", path); return; }

    static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);

    unsigned char ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;      // bit depth
    ihdr[9] = 2;      // colour type 2: truecolour RGB
    ihdr[10] = 0;     // deflate
    ihdr[11] = 0;     // adaptive filtering
    ihdr[12] = 0;     // no interlace
    pngChunk(f, "IHDR", ihdr, sizeof ihdr);

    // Raw scanlines, each prefixed with filter type 0.
    std::vector<unsigned char> raw;
    raw.reserve((size_t)h * (1 + (size_t)w * 3));
    for (int y = 0; y < h; y++)
        {
        raw.push_back(0);
        for (int x = 0; x < w; x++)
            {
            unsigned char rgb[3];
            rgb565to888((uint16_t)fb[(size_t)y * w + x], rgb);
            raw.push_back(rgb[0]); raw.push_back(rgb[1]); raw.push_back(rgb[2]);
            }
        }

    // zlib: header, stored DEFLATE blocks of at most 65535 bytes, adler32.
    std::vector<unsigned char> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size() || raw.empty())
        {
        const size_t n = (raw.size() - off > 65535u) ? 65535u : (raw.size() - off);
        const bool last = (off + n >= raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back((unsigned char)(n & 0xFF));
        z.push_back((unsigned char)(n >> 8));
        z.push_back((unsigned char)(~n & 0xFF));
        z.push_back((unsigned char)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
        if (last) break;
        }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw.size(); i++)
        { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    unsigned char ad[4];
    be32(ad, (b << 16) | a);
    z.insert(z.end(), ad, ad + 4);

    pngChunk(f, "IDAT", z.data(), z.size());
    pngChunk(f, "IEND", nullptr, 0);
    fclose(f);
    }

/** PNG when the name says so, PPM otherwise. */
void writeImage(const char* path, const uint16_t* fb, int w, int h)
    {
    const char* dot = strrchr(path, '.');
    const bool png = (dot != nullptr) &&
                     ((dot[1] | 32) == 'p') && ((dot[2] | 32) == 'n') &&
                     ((dot[3] | 32) == 'g') && (dot[4] == '\0');
    if (png) writePNG(path, fb, w, h);
    else     writePPM(path, fb, w, h);
    }

} // namespace


int main(int argc, char** argv)
    {
    if (argc < 3)
        {
        fprintf(stderr, "usage: %s model.a3d out.png [--clip N] [--time MS] "
                        "[--frames N] [--size N] [--orbit DEG] [--dist MULT] "
                        "[--frame cx,cy,cz,extent] [--second FILE] [--only N]\n", argv[0]);
        return 1;
        }
    const char* inPath = argv[1];
    const char* outPath = argv[2];
    int clip = 0, frames = 1, size = 480;
    float timeMs = -1.0f, orbitDeg = 0.0f;
    // 1.7 frames the whole bounding box. An asset that CONTAINS the camera - a
    // skydome, a room - needs less, and no bounding box can tell the two apart.
    float distMult = 1.7f;
    // A second model, laid out exactly as the on-device demo lays out two: each
    // normalised to unit height and placed in a row. --only draws just one of
    // them while keeping BOTH placements and the shared camera, which is what
    // isolates "does the other model's presence change this one" from "is it
    // framed differently".
    const char* secondPath = nullptr;
    const char* backdropPath = nullptr;   // identity placement, like the demo
    int onlyActor = -1;
    // Framing derived from the model's own bounds cannot compare two versions of
    // a model: decimation shrinks the bounds, the camera follows, and the whole
    // silhouette moves. --frame pins it so a difference image shows detail loss
    // rather than a camera shift.
    float frameCx = 0.0f, frameCy = 0.0f, frameCz = 0.0f, frameExtent = 0.0f;
    for (int i = 3; i + 1 < argc; i += 2)
        {
        if      (strcmp(argv[i], "--clip") == 0)   clip = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--time") == 0)   timeMs = (float)atof(argv[i + 1]);
        else if (strcmp(argv[i], "--frames") == 0) frames = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--size") == 0)   size = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--orbit") == 0)  orbitDeg = (float)atof(argv[i + 1]);
        else if (strcmp(argv[i], "--dist") == 0)   distMult = (float)atof(argv[i + 1]);
        else if (strcmp(argv[i], "--second") == 0) secondPath = argv[i + 1];
        else if (strcmp(argv[i], "--backdrop") == 0) backdropPath = argv[i + 1];
        else if (strcmp(argv[i], "--only") == 0)   onlyActor = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--frame") == 0)
            sscanf(argv[i + 1], "%f,%f,%f,%f", &frameCx, &frameCy, &frameCz, &frameExtent);
        }

    a3d::StdioImage file;
    if (!file.load(inPath)) { fprintf(stderr, "cannot open %s\n", inPath); return 1; }
    const a3d::AssetImage& img = file.image();

    printf("%s: %u nodes, %u meshes, %u skins, %u bones, %u clips, %u materials, %u textures\n",
           inPath, img.nodeCount(), img.meshCount(), img.skinCount(),
           img.boneCount(), img.clipCount(), img.materialCount(), img.textureCount());

    const a3d::fmt::MeshEntry* me = img.meshes();
    // The bounding box drives the camera, so an asset of any scale frames itself
    // instead of needing hand-tuned numbers per model.
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t m = 0; m < img.meshCount(); m++)
        {
        printf("  mesh %u: %u vertices, %u triangles, normals=%s, texcoords=%s, material=%u\n",
               m, me[m].vertex_count, me[m].triangle_count,
               (me[m].normals_off   != a3d::fmt::NONE32) ? "yes" : "NO",
               (me[m].texcoords_off != a3d::fmt::NONE32) ? "yes" : "no",
               me[m].material_index);
        for (int k = 0; k < 3; k++)
            {
            if (me[m].bbox_min[k] < mn[k]) mn[k] = me[m].bbox_min[k];
            if (me[m].bbox_max[k] > mx[k]) mx[k] = me[m].bbox_max[k];
            }
        }
    for (uint32_t c = 0; c < img.clipCount(); c++)
        {
        const a3d::fmt::ClipEntry* cl = img.clips();
        const char* nm = img.string(cl[c].name_off);
        printf("  clip %u: \"%s\" %u ms, %u channels\n",
               c, nm ? nm : "(unnamed)", cl[c].duration_ms, cl[c].channel_count);
        }

    std::vector<uint16_t> fb((size_t)size * size);
    const int sheetW = size * frames;
    std::vector<uint16_t> sheet((size_t)sheetW * size, 0);
    std::vector<uint16_t> zbuf((size_t)size * size);
    a3d::SoftBackend backend;
    backend.setViewportSize(size, size);
    backend.setTarget(fb.data(), size, size, size);
    backend.setZBuffer(zbuf.data());
    backend.setCulling(-1);
    backend.setLightDirection(-0.4f, -0.6f, -1.0f);
    a3d::SceneRuntime scene;
    if (!scene.bind(img, backend)) { fprintf(stderr, "scene bind failed\n"); return 1; }

    a3d::AnimationPlayer player;
    const bool animated = player.bind(img) && player.selectClip((uint32_t)clip);
    if (animated)
        printf("  playing clip %d \"%s\" (%.0f ms)\n",
               clip, player.clipName() ? player.clipName() : "", player.durationMs());
    a3d::SerialExecutor exec;

    // Frame the model only after it has been posed once. Where a model IS is a
    // property of the scene graph and the skeleton, not of the stored mesh
    // bounds, and reading it from the file gives the wrong answer for anything
    // placed by a node transform.
    if (animated) { player.seek((timeMs >= 0.0f) ? timeMs : 0.0f); player.apply(scene); }
    scene.updateWorldTransforms();
    scene.skinAll(exec);

    // --- optional second actor, placed the way the demo places two ----------
    a3d::StdioImage file2;
    a3d::SceneRuntime scene2;
    a3d::AnimationPlayer player2;
    bool haveSecond = false, animated2 = false;
    if (secondPath != nullptr)
        {
        if (!file2.load(secondPath)) { fprintf(stderr, "second load failed\n"); return 1; }
        if (!scene2.bind(file2.image(), backend))
            { fprintf(stderr, "second bind failed\n"); return 1; }
        animated2 = player2.bind(file2.image()) && player2.selectClip(0);
        if (animated2) { player2.seek((timeMs >= 0.0f) ? timeMs : 0.0f); player2.apply(scene2); }
        scene2.updateWorldTransforms();
        scene2.skinAll(exec);
        haveSecond = true;

        a3d::SceneRuntime* sc[2] = { &scene, &scene2 };
        for (int a = 0; a < 2; a++)
            {
            float amn[3], amx[3];
            if (!sc[a]->worldBounds(amn, amx)) continue;
            // Rest-pose HEIGHT, matching the demo: a character's scale is how
            // tall it stands, not the volume its animation sweeps.
            const float e = (amx[1] - amn[1] > 1e-6f) ? (amx[1] - amn[1]) : 1.0f;
            const float g = 1.0f / e;
            const float x = (a - 0.5f) * 1.25f;
            const float root[16] = { g,0,0,0, 0,g,0,0, 0,0,g,0,
                                     x - g * 0.5f * (amn[0] + amx[0]),
                                        -g * amn[1],
                                        -g * 0.5f * (amn[2] + amx[2]), 1 };
            sc[a]->setRootTransform(root);
            sc[a]->updateWorldTransforms();
            sc[a]->skinAll(exec);
            printf("  actor %d: extent %.2f -> scale %.4f, slot x=%.2f\n", a, e, g, x);
            }
        }

    a3d::StdioImage file3;
    a3d::SceneRuntime scene3;
    a3d::AnimationPlayer player3;
    bool haveBackdrop = false, animated3 = false;
    if (backdropPath != nullptr)
        {
        if (!file3.load(backdropPath)) { fprintf(stderr, "backdrop load failed\n"); return 1; }
        if (!scene3.bind(file3.image(), backend))
            { fprintf(stderr, "backdrop bind failed\n"); return 1; }
        animated3 = player3.bind(file3.image()) && player3.selectClip(0);
        haveBackdrop = true;
        printf("  backdrop: identity placement\n");
        }

    float wmn[3], wmx[3];
    if (!scene.worldBounds(wmn, wmx))
        { fprintf(stderr, "nothing to draw\n"); return 1; }
    if (haveSecond)
        {
        float m0[3], m1[3];
        if (scene2.worldBounds(m0, m1))
            for (int k = 0; k < 3; k++)
                { if (m0[k] < wmn[k]) wmn[k] = m0[k]; if (m1[k] > wmx[k]) wmx[k] = m1[k]; }
        }
    float cx = 0.5f * (wmn[0] + wmx[0]);
    float cy = 0.5f * (wmn[1] + wmx[1]);
    float cz = 0.5f * (wmn[2] + wmx[2]);
    float extent = 0.0f;
    for (int k = 0; k < 3; k++)
        { const float d = wmx[k] - wmn[k]; if (d > extent) extent = d; }
    if (extent <= 0.0f) extent = 1.0f;
    if (haveBackdrop)
        {
        // Frame the composition, not the union: the backdrop's sky is far wider
        // than the characters and would zoom them to specks. Same widening the
        // demo applies.
        wmn[0] -= 0.34f; wmx[0] += 0.34f;
        wmn[1] = 0.0f;   wmx[1] += 0.22f;
        wmn[2] -= 0.25f; wmx[2] += 0.25f;
        cx = 0.5f * (wmn[0] + wmx[0]);
        cy = 0.5f * (wmn[1] + wmx[1]);
        cz = 0.5f * (wmn[2] + wmx[2]);
        extent = 0.0f;
        for (int k = 0; k < 3; k++)
            { const float d = wmx[k] - wmn[k]; if (d > extent) extent = d; }
        }
    if (frameExtent > 0.0f)
        { cx = frameCx; cy = frameCy; cz = frameCz; extent = frameExtent; }
    // Same factor the on-device demo uses, so what is framed here is what will
    // be framed there. A camera that only agrees approximately would let a
    // framing bug through to the device.
    const float dist = extent * distMult;
    printf("  world bbox centre (%.2f %.2f %.2f) extent %.2f -> camera distance %.2f\n",
           cx, cy, cz, extent, dist);
    backend.setPerspective(45.0f, 1.0f, extent * 0.05f, dist * 4.0f);
    const float orbA = orbitDeg * 3.14159265f / 180.0f;
    const float dx = sinf(orbA) * 0.86f, dy = 0.38f, dz = cosf(orbA) * 0.86f;
    const float invd = dist / sqrtf(dx * dx + dy * dy + dz * dz);
    const float eye[3] = { cx + dx * invd, cy + dy * invd, cz + dz * invd };
    const float ctr[3] = { cx, cy, cz };
    const float upv[3] = { 0.0f, 1.0f, 0.0f };
    backend.setLookAt(eye, ctr, upv);

    uint32_t prevHash = 0;
    for (int f = 0; f < frames; f++)
        {
        if (animated)
            {
            const float t = (timeMs >= 0.0f) ? timeMs
                          : (player.durationMs() * (float)f / (float)(frames > 1 ? frames : 1));
            player.seek(t);
            player.apply(scene);
            }
        scene.updateWorldTransforms();
        scene.skinAll(exec);

        std::fill(fb.begin(), fb.end(), (uint16_t)0);
        backend.clearZBuffer();
        int calls = 0;
        if (haveBackdrop)
            {
            if (animated3)
                {
                player3.seek((timeMs >= 0.0f) ? timeMs : 0.0f);
                player3.apply(scene3);
                }
            scene3.updateWorldTransforms();
            scene3.skinAll(exec);
            calls += scene3.draw(a3d::Shading::Gouraud, a3d::TextureMode::Perspective);
            }
        if (onlyActor != 1)
            calls += scene.draw(a3d::Shading::Gouraud, a3d::TextureMode::Perspective);
        if (haveSecond && (onlyActor != 0))
            {
            if (animated2)
                {
                player2.seek((timeMs >= 0.0f) ? timeMs : 0.0f);
                player2.apply(scene2);
                scene2.updateWorldTransforms();
                scene2.skinAll(exec);
                }
            calls += scene2.draw(a3d::Shading::Gouraud, a3d::TextureMode::Perspective);
            }

        int drawn = 0;
        uint32_t h = 2166136261u;
        for (int i = 0; i < size * size; i++)
            {
            if (fb[i] != 0) drawn++;
            const uint16_t v = (uint16_t)fb[i];
            h ^= (uint8_t)(v & 0xFF); h *= 16777619u;
            h ^= (uint8_t)(v >> 8);   h *= 16777619u;
            }

        // Into the sheet rather than into a file of its own. Eight files of a
        // format the desktop cannot open was the old behaviour, and a strip
        // that opens with a double click is what makes "check it before you
        // flash it" a step somebody actually takes.
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                sheet[(size_t)y * sheetW + (size_t)f * size + x] = fb[(size_t)y * size + x];

        printf("  frame %d: %d draw call(s), %d pixels (%.1f%%), hash 0x%08X%s\n",
               f, calls, drawn, 100.0 * drawn / (size * size), h,
               (f > 0 && h == prevHash) ? "  [SAME AS PREVIOUS - pose did not move]" : "");
        prevHash = h;
        }

    writeImage(outPath, sheet.data(), sheetW, size);
    printf("  wrote %s (%d x %d, %d frame%s left to right)\n",
           outPath, sheetW, size, frames, (frames == 1) ? "" : "s");
    return 0;
    }
