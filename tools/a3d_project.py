# SPDX-FileCopyrightText: 2026 Eric Nam
# SPDX-License-Identifier: Apache-2.0
"""Turn a .glb into an ESP-IDF project that builds before you have written a line.

WHY

    `--check` answers "will my model run". It leaves the person holding a
    `.a3d` and a much bigger question: an IDF project, a partition table big
    enough for the asset, the sdkconfig their part needs, the EMBED_FILES
    symbol spelled the way the linker will spell it, and the calls. The
    examples in this tree contain all of that, mixed in with SD browsing,
    touch gestures and a HUD, and reverse-engineering one is where people stop.

    So this writes the project. Everything that can be derived is derived: the
    partition is sized from the model that is actually going in it, the symbol
    from the file name that is actually embedded, the sdkconfig from the part
    that was actually asked for.

THE ONE DECISION IT MAKES, AND WHY

    The generated app renders OFF SCREEN and logs its frame rate. It does not
    ship a panel driver, because a3d cannot know the board - but that is not
    why it starts off screen. It starts off screen so that `idf.py flash
    monitor` works on the first try and prints the real frame time for YOUR
    model on YOUR part, which beats any estimate this tree can interpolate.
    Then one function is replaced and the same frames go to the glass.

    A project that cannot build until the user has written the hard part is a
    project most people never build.
"""

from __future__ import annotations

import re
from pathlib import Path

import a3d_panel as PANEL

#: Where a3d comes from when the project does not carry a copy of it.
A3D_GIT = "https://github.com/0015/a3d.git"
A3D_REF = "v0.9.0"

#: What each part needs in sdkconfig that a default project does not have.
TARGETS = {
    "esp32s3": dict(
        name="ESP32-S3", cores=2, psram=True,
        extra=[],
    ),
    "esp32p4": dict(
        name="ESP32-P4", cores=2, psram=True,
        extra=[
            "",
            "# ESP32-P4 revision v1.0 is refused by esptool at FLASH time with an",
            "# IDF 5.5.4 default build: \"requires chip revision in range",
            "# [v3.1 - v3.99] (this chip is revision v1.0)\". BOTH lines are needed",
            "# and the first is the one that is missed: ESP32P4_REV_MIN_100 depends",
            "# on ESP32P4_SELECTS_REV_LESS_V3, so setting the revision alone is",
            "# silently dropped. Delete both if your part is v3.0 or newer.",
            "CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y",
            "CONFIG_ESP32P4_REV_MIN_100=y",
        ],
    ),
    "esp32c6": dict(
        name="ESP32-C6", cores=1, psram=False,
        extra=[
            "",
            "# The C6 has no FPU: every float in the rasterizer is a libgcc call, and",
            "# this tree has measured a model at 9.6x the frame time of an ESP32-S3",
            "# against a clock ratio of only 1.5x. Keep the model small.",
        ],
    ),
}


def embed_symbol(filename: str) -> str:
    """The name the linker will use, spelled the way CMake spells it.

    ESP-IDF runs the embedded file's NAME through CMake's MAKE_C_IDENTIFIER,
    so every character that is not a letter, a digit or an underscore becomes
    an underscore - the dot in `model.a3d` included. Guessing this wrong is a
    link error rather than a silent fault, but it is a link error at the end of
    a five-minute build, so it is derived here instead.
    """
    # The substitution runs over the WHOLE symbol, prefix included - CMake
    # calls MAKE_C_IDENTIFIER on "_binary_<name>_start", not on <name> - so a
    # file starting with a digit needs no special case. Getting that backwards
    # produced `_binary__2fast_a3d_start` for `2fast.a3d`, one underscore too
    # many, and was caught by asking cmake -P rather than by reasoning.
    #
    # A name with non-ASCII characters collapses to underscores, so two
    # different models can end up sharing one symbol - a link error that names
    # neither of them. Checked for by the caller.
    return re.sub(r"[^0-9a-zA-Z_]", "_", "_binary_%s" % filename)


def app_partition_bytes(model_bytes: int, headroom: int = 1_200_000) -> int:
    """App partition big enough for the firmware AND the model inside it.

    The model is embedded in .rodata, so it is part of the app image; a default
    1 MB factory partition takes a firmware of ~530 KB and about 400 KB of
    model before the build fails at the very end, on `check_sizes.py`. Rounded
    up to the 64 KB an app partition is aligned to.
    """
    need = model_bytes + headroom
    block = 0x10000
    return max(0x100000, (need + block - 1) // block * block)


def resolve_psram(target: str, model_bytes: int, want: str = "auto") -> bool:
    """Whether PSRAM goes in sdkconfig.defaults.

    "auto" turns it on for a model whose decoded vertices will not sit
    comfortably in internal SRAM. That threshold is a guess about ONE number
    and it is deliberately overridable, because the part that decides is the
    board: a module with no PSRAM fitted does not want the config even for a
    large model, and a scene with several models wants it for a small one.

    It is also not free where it is wrong. With PSRAM enabled the IDF heap
    sends every allocation above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB by
    default) to the slow memory - which is how the vertex buffers a skinned
    mesh rewrites EVERY FRAME end up in PSRAM without anybody choosing it. The
    generated main.cpp works around that with setAllocator(), but the trap is
    worth knowing before turning this on for a part that does not need it.
    """
    if want == "on":
        return TARGETS[target]["psram"]
    if want == "off":
        return False
    return TARGETS[target]["psram"] and model_bytes > 150_000


def _sdkconfig(target: str, model_bytes: int, flash_mb: int, need_psram: bool) -> str:
    t = TARGETS[target]
    lines = [
        'CONFIG_IDF_TARGET="%s"' % target,
        "",
        "# The model is embedded in the app image, so the app partition has to",
        "# hold the firmware and the model together. partitions.csv is sized for",
        "# the model that was actually converted; regenerate or edit both if you",
        "# replace it with a bigger one.",
        "CONFIG_PARTITION_TABLE_CUSTOM=y",
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"',
        "CONFIG_ESPTOOLPY_FLASHSIZE_%dMB=y" % flash_mb,
        "",
        "CONFIG_COMPILER_OPTIMIZATION_PERF=y",
        "CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192",
        "CONFIG_FREERTOS_HZ=1000",
    ]
    if need_psram and t["psram"]:
        lines += [
            "",
            "# PSRAM is on. This model decodes to about %d KB of vertex data."
            % (_bind_kb(model_bytes),),
            "# IGNORE_NOTFOUND is what keeps the image booting on a board that has",
            "# none - without it a part with no PSRAM aborts during startup instead",
            "# of telling you.",
            "#",
            "# WHAT TO WATCH. With PSRAM enabled the IDF heap sends every allocation",
            "# above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB by default) to the",
            "# slow memory, so the vertex buffers a skinned mesh rewrites every frame",
            "# land there without anybody choosing it. main.cpp asks for internal RAM",
            "# explicitly through setAllocator(), which is why it does not - but a",
            "# free-memory delta will still not show you the difference, because the",
            "# big arrays never touched internal RAM to begin with.",
            "CONFIG_SPIRAM=y",
            "CONFIG_SPIRAM_IGNORE_NOTFOUND=y",
        ]
    elif t["psram"]:
        lines += [
            "",
            "# PSRAM is OFF. Turn it on here if your module has it and the model does",
            "# not fit internal SRAM - the symptom is a failed allocation at load,",
            "# not a slow frame.",
            "# CONFIG_SPIRAM=y",
            "# CONFIG_SPIRAM_IGNORE_NOTFOUND=y",
        ]
    lines += t["extra"]
    lines += [
        "",
        "# Rendering runs in one uninterrupted stretch, so a slow model would look",
        "# like a hang to the watchdog. Turn this back on once the frame rate is",
        "# where you want it.",
        "CONFIG_ESP_TASK_WDT_EN=n",
        "",
    ]
    return "\n".join(lines)


def _bind_kb(model_bytes: int) -> int:
    # Rough: the container is mostly quantized vertices and indices, and bind()
    # decodes vertices to 32 bytes each. Used only to word a comment.
    return max(16, int(model_bytes * 1.6) // 1024)


def _partitions(app_bytes: int) -> str:
    return (
        "# Name,   Type, SubType, Offset,  Size, Flags\n"
        "# The app holds the firmware AND the embedded model. Sized for a model\n"
        "# of the size that was converted, with room to grow.\n"
        "nvs,      data, nvs,     0x9000,  0x6000,\n"
        "phy_init, data, phy,     0xf000,  0x1000,\n"
        "factory,  app,  factory, ,        %dK,\n" % (app_bytes // 1024)
    )


def _root_cmake(project: str) -> str:
    return (
        "cmake_minimum_required(VERSION 3.16)\n\n"
        "include($ENV{IDF_PATH}/tools/cmake/project.cmake)\n"
        "project(%s)\n" % project
    )


def _main_cmake(a3d_root: Path, model_name: str, a3d_source: str = "path") -> str:
    """How this project finds a3d, which is one of two things and never both.

    "managed" - the ESP-IDF component manager fetches a3d from git into
    managed_components/, where its own CMakeLists registers it as a component
    and exports its include paths. The project then only names it in REQUIRES.
    This is what the web generator produces, because a zip cannot carry a path
    on somebody else's machine and should not carry a stale copy of a library
    that has a repository.

    "path" - a local checkout, named absolutely. What the command line produces,
    because somebody running tools/a3d_export.py already has one.
    """
    if a3d_source == "managed":
        return (
            "# a3d is fetched by the ESP-IDF component manager - see\n"
            "# main/idf_component.yml. It is header-only and registers its own\n"
            "# include paths, so naming it in REQUIRES is the whole of it.\n"
            "idf_component_register(\n"
            '    SRCS "main.cpp" "panel.c"\n'
            '    INCLUDE_DIRS "."\n'
            "    REQUIRES esp_timer heap freertos driver esp_lcd a3d\n"
            "    # The model goes into .rodata, which on ESP32 is memory-mapped flash:\n"
            "    # a3d opens it in place and it costs no RAM until a pixel is drawn.\n"
            '    EMBED_FILES "%s"\n'
            ")\n" % model_name
        )
    return (
        "# a3d is header-only: ONE include path and nothing to link. `src` is\n"
        "# what makes \"a3d/...\", \"viewer/a3d_viewer.h\", \"backends/...\" and\n"
        "# \"loaders/...\" all resolve from a single root.\n"
        'set(A3D_ROOT "%s")\n\n'
        "idf_component_register(\n"
        '    SRCS "main.cpp" "panel.c"\n'
        '    INCLUDE_DIRS "." "${A3D_ROOT}/src"\n'
        "    REQUIRES esp_timer heap freertos driver esp_lcd\n"
        "    # The model goes into .rodata, which on ESP32 is memory-mapped flash:\n"
        "    # a3d opens it in place and it costs no RAM until a pixel is drawn.\n"
        '    EMBED_FILES "%s"\n'
        ")\n" % (a3d_root, model_name)
    )


def _idf_component_yml(target: str, panel: str = "offscreen",
                       touch: str = "none", idf: str = ">=5.0",
                       a3d_source: str = "path") -> str:
    """The manifest, with the drivers the chosen bus actually needs.

    A version is pinned for every one of them. "latest" is not a version: this
    tree has already lost a session to a vendor component whose 2.0.0 is for
    ESP-IDF 6 and whose 1.0.6 is for 5.x, where the manifest's `^1` capped it
    BELOW the release that works. Neither was newer-and-better; each was right
    for one IDF.
    """
    deps = PANEL.components(panel, touch)
    lines = []
    if a3d_source == "managed":
        lines += [
            "## a3d itself, fetched from git. It is NOT included in this project:",
            "## the renderer lives in its own repository and this is the line that",
            "## brings it in. `idf.py build` does the fetch; nothing to install.",
            "##",
            "## `version` is a git ref, and it is a TAG - so this build is",
            "## reproducible: the ref cannot move under you. Change it to track a",
            "## newer release; a branch name would work too and is what you do NOT",
            "## want in a build you intend to repeat.",
            "dependencies:",
            '  idf: "%s"' % idf,
            "  a3d:",
            '    git: "%s"' % A3D_GIT,
            '    version: "%s"' % A3D_REF,
        ]
    else:
        lines += [
            "## a3d is included by path (see main/CMakeLists.txt), so it is not",
            "## fetched here. What follows is your panel and touch driver.",
            "dependencies:",
            '  idf: "%s"' % idf,
        ]
    for name in sorted(deps):
        lines.append('  %s: "%s"' % (name, deps[name]))
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# THE DISCLAIMER.
#
# It is repeated in three places on purpose - the top of main.cpp, the top of
# the project README, and the console output - because each of them is the only
# one somebody reads. It is not legal throat-clearing: the two things a
# generator cannot know are exactly the two things that make a board light up,
# and a person who flashes this expecting a finished driver will conclude that
# a3d is broken rather than that the pins are not theirs.
# ---------------------------------------------------------------------------

DISCLAIMER_MD = """> **This is a starting skeleton, not finished firmware.**
>
> It was generated from a few menu choices. It does not know your board, and
> two things in it are near-certainly wrong for you:
>
> 1. **The ESP32 configuration** - `sdkconfig.defaults` and `partitions.csv`
>    carry defaults chosen from your target and your model's size. Flash size,
>    PSRAM, clocks, and anything else your board needs are yours to set.
> 2. **The display and touch behaviour** - every `#define` at the top of
>    `main/panel.c` is a real board's wiring, named in that file, and almost
>    certainly not yours. Pins, panel orientation, colour inversion, byte order
>    and the init sequence all have to be checked against your schematic and
>    your panel's datasheet.
>
> **You must edit these to match your own hardware.** Read `main/panel.c`
> before you flash. A plausible default driving the wrong GPIO looks exactly
> like a dead panel, which is the most expensive way to be wrong here.
"""

DISCLAIMER_C = """// ===========================================================================
//  THIS IS A STARTING SKELETON, NOT FINISHED FIRMWARE.
//
//  It was generated from a few menu choices and it does not know your board.
//  You MUST edit two things to match your own hardware:
//
//    1. The ESP32 configuration - sdkconfig.defaults and partitions.csv.
//    2. The display and touch behaviour - every #define at the top of
//       main/panel.c is another board's wiring, and so are the orientation,
//       the colour inversion and the init sequence.
//
//  Read main/panel.c before you flash. A plausible default driving the wrong
//  GPIO looks exactly like a dead panel.
// ==========================================================================="""

DISCLAIMER_TEXT = (
    "This is a starting skeleton, not finished firmware. You MUST edit the "
    "ESP32 configuration (sdkconfig.defaults, partitions.csv) and the display "
    "and touch behaviour (main/panel.c) to match your own hardware.")

_A3D_MANAGED = """## Where a3d comes from

**a3d is not included in this project.** The renderer lives in its own
repository, and `main/idf_component.yml` is the line that brings it in:

```yaml
dependencies:
  a3d:
    git: "@GIT@"
    version: "@REF@"
```

`idf.py build` fetches it into `managed_components/a3d/` on the first build.
There is nothing to install and nothing to copy - but that first build needs
network access, and it needs the repository to actually contain the library.

`version` is a git ref, and a **branch** here, so your build can change under
you the next time that branch moves. Pin a tag once one exists.

### If you would rather vendor it

```bash
cd <this project>
mkdir -p components
git clone @GIT@ components/a3d
```

Then delete the `a3d:` block from `main/idf_component.yml`. a3d is header-only
and registers itself as an ESP-IDF component, so nothing else changes - the
`REQUIRES a3d` in `main/CMakeLists.txt` finds it either way.
"""

_A3D_PATH = """## Where a3d comes from

`main/CMakeLists.txt` points at a local checkout:

```cmake
set(A3D_ROOT "{a3d_root}")
```

That path is on the machine that generated this project. Change it, or switch
to the component manager: put this in `main/idf_component.yml`, and replace the
`INCLUDE_DIRS` entry that names `A3D_ROOT` with `a3d` in `REQUIRES`.

```yaml
dependencies:
  a3d:
    git: "@GIT@"
    version: "@REF@"
```
"""

_A3D_MANAGED = _A3D_MANAGED.replace("@GIT@", A3D_GIT).replace("@REF@", A3D_REF)
_A3D_PATH = _A3D_PATH.replace("@GIT@", A3D_GIT).replace("@REF@", A3D_REF)


MAIN_CPP = '''{disclaimer}
// Generated by tools/a3d_project.py. Yours to edit.
//
// WHAT THIS DOES
//   Loads {model_name}, animates it, and sends it to: {panel_label}
//   It logs the frame time once a second either way. Build and flash it before
//   changing anything: the number in the log is the real cost of YOUR model on
//   YOUR part, which is worth more than any estimate this tree can interpolate.
//
// WHERE THE BOARD IS
//   main/panel.c, and nothing else. This file does not name a bus, a GPIO or a
//   vendor header, so changing your mind about the panel changes one file.
//   THE PINS IN THAT FILE ARE NOT YOUR BOARD'S until you have checked them.

#include "viewer/a3d_viewer.h"
#include "backends/freertos/a3d_freertos_executor.h"

extern "C" {{
#include "panel.h"
}}

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

// The model, embedded in the app image. The symbol is the file NAME with every
// character that is not a letter, digit or underscore replaced - the dot
// included - which is why it is spelled out rather than guessed.
extern const uint8_t model_start[] asm("{sym}_start");
extern const uint8_t model_end[]   asm("{sym}_end");

namespace
{{

const char* TAG = "a3d";

// a3d::Display is a plain struct of function pointers, so the panel is adapted
// rather than inherited from. Each of these forwards one call into panel.c.
void sendTile(void*, const uint16_t* px, int x, int y, int w, int h)
    {{ a3d_panel_send(px, x, y, w, h); }}
void fillScreen(void*, uint16_t colour)
    {{ a3d_panel_fill(colour); }}
bool touchRead(void*, int* x, int* y)
    {{ return a3d_panel_touch(x, y); }}
void lockBus(void*)   {{ a3d_panel_lock(); }}
void unlockBus(void*) {{ a3d_panel_unlock(); }}

void* dmaAlloc(size_t n)   {{ return heap_caps_malloc(n, MALLOC_CAP_DMA | MALLOC_CAP_8BIT); }}
void* bulkAlloc(size_t n)
    {{
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return (p != nullptr) ? p : heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }}
void  capsFree(void* p)    {{ heap_caps_free(p); }}
uint64_t nowUs(void*)      {{ return (uint64_t)esp_timer_get_time(); }}

}} // namespace

extern "C" void app_main(void)
    {{
    // The panel comes up FIRST. It is what knows the size, and a viewer sized
    // from a constant that disagrees with the glass draws a correct picture
    // into the wrong window - which looks like a projection bug.
    const esp_err_t perr = a3d_panel_init();
    if (perr != ESP_OK)
        {{
        ESP_LOGE(TAG, "panel would not start: %s", esp_err_to_name(perr));
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
        }}

    a3d::Display panel;
    panel.width     = a3d_panel_width();
    panel.height    = a3d_panel_height();
    panel.name      = "{panel_name}";
    panel.sendTile  = &sendTile;
    panel.fill      = &fillScreen;
    panel.touchRead = &touchRead;
    // A near-black background coming out PINK is this being wrong: 0x1082
    // byte-swapped is 0x8210, equal red and blue with the green pulled down.
    // It looks nothing like "the colours are slightly off".
    panel.swapBytes = a3d_panel_swap_bytes();
    panel.round     = a3d_panel_round();
{lock_wiring}
    // Built BEFORE the viewer: it is what decides how many tile framebuffers
    // get allocated. One worker allocates one set, not two.
    static a3d::FreeRtosExecutor exec({workers}, 8192, 5);

    a3d::ViewerConfig cfg;
    // A slot costs width * tileHeight * 2 bytes of colour and the same of
    // depth. Lower this first if you run out of internal SRAM.
    cfg.tileHeight = {tile_h};

    static a3d::Viewer viewer;
    viewer.setExecutor(exec);
    viewer.setAllocator(dmaAlloc, capsFree);        // tile buffers: fast, DMA-capable
    viewer.setLargeAllocator(bulkAlloc, capsFree);  // geometry: PSRAM if there is any
    viewer.setClock(&nowUs, nullptr);
    if (!viewer.begin(panel, cfg))
        {{
        ESP_LOGE(TAG, "viewer would not start: %s", viewer.error());
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
        }}

    if (!viewer.openAsset(model_start, (size_t)(model_end - model_start)))
        {{
        ESP_LOGE(TAG, "model would not load: %s", viewer.error());
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
        }}

    ESP_LOGI(TAG, "%u vertices, %u triangles, %u clip(s), %d slot(s), %d tiles",
             (unsigned)viewer.vertexCount(), (unsigned)viewer.triangleCount(),
             (unsigned)viewer.clipCount(), viewer.slotCount(), viewer.tileCount());
    ESP_LOGI(TAG, "internal free %u KB, PSRAM free %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 1024));

    uint64_t lastMs = esp_timer_get_time() / 1000;
    uint64_t reportMs = lastMs;
    int frames = 0;

    while (true)
        {{
        const uint64_t now = esp_timer_get_time() / 1000;
        float dt = (float)(now - lastMs);
        lastMs = now;
        if (dt <= 0.0f) dt = 1.0f;
        if (dt > 250.0f) dt = 250.0f;

        // Drag to turn the model. touchRead returns false when nothing is
        // down, and with no touch controller configured it always does.
        int tx = 0, ty = 0;
        static int lastX = -1, lastY = -1;
        if (panel.touchRead(panel.user, &tx, &ty))
            {{
            if (lastX >= 0)
                viewer.orbit((float)(tx - lastX) * -0.01f, (float)(ty - lastY) * 0.01f);
            lastX = tx; lastY = ty;
            }}
        else
            {{
            lastX = lastY = -1;
            viewer.orbit(dt * 0.0006f, 0.0f);   // turn slowly so every side is seen
            }}

        viewer.frame(dt);
        frames++;

        // The tail, not the first windows: frame time falls for about five
        // seconds after boot and three conclusions in this tree were drawn from
        // warm-up numbers and were wrong.
        if (now - reportMs >= 1000)
            {{
            const a3d::ViewerStats& s = viewer.stats();
            ESP_LOGI(TAG, "%d fps  frame=%.1fms  skin=%.1f bin=%.1f draw=%.1f  %u tris",
                     frames, s.frameUs / 1000.0, s.skinUs / 1000.0,
                     s.binUs / 1000.0, s.drawUs / 1000.0,
                     (unsigned)s.trianglesDrawn);
            frames = 0;
            reportMs = now;
            }}
        }}
    }}
'''


README = """# {project}

{disclaimer}

Generated from `{source}` by `tools/a3d_project.py`.

## Build it first, change it after

```bash
idf.py set-target {target}
idf.py -p <port> flash monitor
```

The first build fetches a3d, so it needs network. After that `dependencies.lock`
pins it and builds are offline.

It renders your model off screen and logs the frame time once a second:

```
I (2431) a3d: 3273 vertices, 4672 triangles, 1 clip(s), {workers} slot(s), {tiles} tiles
I (3440) a3d: 18 fps  frame=54.2ms  skin=4.6 bin=6.9 draw=42.1  2300 tris
```

That number is the real cost of this model on this part. It is worth more than
any estimate, and it is the reason this project starts off screen rather than
waiting for a display driver you have not written yet.

**Wait about five seconds before believing it.** Frame time falls for roughly
the first five one-second windows after boot, and reading a warm-up number has
misled this project three separate times.

## Then point it at your screen

Everything you change is between `--- YOUR PANEL ---` and `--- end YOUR PANEL ---`
in `main/main.cpp`: set `PANEL_W` / `PANEL_H`, and make `sendTile` push the
pixels. For an esp_lcd panel that is one call.

**The one rule:** `sendTile` must not return until the buffer may be reused.
The viewer owns one tile buffer per worker and hands it straight to the next
tile, so a driver that queues the pointer and returns will send whatever
overwrote it.

## What was chosen for you, and where to change it

| | |
|---|---|
| model | `main/{model_name}` — {model_bytes:,} bytes, embedded in the app image |
| symbol | `{sym}_start` / `{sym}_end`, derived from that file name |
| app partition | {app_kb} KB in `partitions.csv`, sized to hold the firmware and the model |
| flash size | {flash_mb} MB in `sdkconfig.defaults` — raise it if your board has more |
| workers | {workers} in `main.cpp`; the executor is what decides how many tile buffers exist |
| tile height | {tile_h} rows; lower it first if internal SRAM runs out |
| PSRAM | {psram} in `sdkconfig.defaults` |
| panel | {panel_label} — `main/panel.c` |
| touch | {touch_label} |

{a3d_how}

## The pins in `main/panel.c` are not your board's

They were copied from {panel_source} and nothing in a3d can know your wiring.
Check every `#define` at the top of that file against your schematic before you
flash. A plausible default driving the wrong GPIO looks exactly like a dead
panel, which is the most expensive way to be wrong here.

Two symptoms worth recognising before you start bisecting:

- **A near-black background comes out PINK.** That is the panel reading RGB565
  big-endian. 0x1082 byte-swapped is 0x8210 — equal red and blue with the green
  pulled down — and it looks nothing like "the colours are slightly off". Flip
  `a3d_panel_swap_bytes()`, or better, find the panel register that removes the
  swap: an ST7789 has one (RAMCTRL bit 3) and then it costs nothing.
- **Half of one tile and half of the next.** `a3d_panel_send` returned before
  the DMA finished. The viewer owns one tile buffer per worker and hands it
  straight on, so the wait inside that function is not optional.

## Replacing the model

Convert it in the browser and drop the `.a3d` into `main/`, or with a checkout
of a3d:

```bash
python3 <a3d>/tools/a3d_export.py --check new.glb -o main/{model_name}
```

Keep the file name and the symbol stays right. If the new model is much bigger,
raise the `factory` partition in `partitions.csv` to match — the build fails at
the very end otherwise, on the size check.
"""


def generate(dest: Path, model: Path, target: str, a3d_root: Path,
             panel_w: int, panel_h: int, tile_h: int, flash_mb: int,
             source_name: str, workers: int | None = None,
             *, panel: str = "offscreen", touch: str = "none",
             idf: str = ">=5.0", a3d_source: str = "path",
             psram: str = "auto") -> dict:
    """Write the project. Returns what was decided, for the caller to print.

    `panel` / `touch` name an entry in a3d_panel.py's catalogue. The default
    pair renders off screen, which is deliberate and is explained there.

    `panel_w` / `panel_h` are OVERRIDDEN by the catalogue's native size for a
    real panel, because a viewport that disagrees with the glass draws a
    correct picture into the wrong window and reads as a projection bug.
    """
    if target not in TARGETS:
        raise ValueError("unknown target %r; known: %s"
                         % (target, ", ".join(sorted(TARGETS))))
    PANEL.validate(panel, touch, target)
    t = TARGETS[target]
    if workers is None:
        workers = t["cores"]
    if panel != "offscreen":
        panel_w, panel_h = PANEL.PANELS[panel]["size"]

    model_bytes = model.stat().st_size
    app_bytes = app_partition_bytes(model_bytes)
    if app_bytes > flash_mb * 1024 * 1024 - 0x10000:
        raise ValueError(
            "the model needs a %d KB app partition, which does not fit in %d MB "
            "of flash. Pass --flash-size with what your board actually has, or "
            "shrink the model with --max-triangles / --max-texture."
            % (app_bytes // 1024, flash_mb))

    sym = embed_symbol(model.name)
    if not re.fullmatch(r"[0-9A-Za-z._-]+", model.name):
        raise ValueError(
            "the model file name %r contains characters that ESP-IDF replaces "
            "with underscores when it makes the linker symbol, which can make "
            "two different models share one name. Rename it to plain ASCII."
            % model.name)
    if psram not in ("auto", "on", "off"):
        raise ValueError("psram must be auto, on or off, not %r" % psram)
    if a3d_source not in ("path", "managed"):
        raise ValueError("a3d_source must be path or managed, not %r" % a3d_source)
    need_psram = resolve_psram(target, model_bytes, psram)
    project = dest.name.replace("-", "_")
    tiles = (panel_h + tile_h - 1) // tile_h

    (dest / "main").mkdir(parents=True, exist_ok=True)
    (dest / "CMakeLists.txt").write_text(_root_cmake(project))
    (dest / "partitions.csv").write_text(_partitions(app_bytes))
    (dest / "sdkconfig.defaults").write_text(
        _sdkconfig(target, model_bytes, flash_mb, need_psram)
        + "\n".join(PANEL.sdkconfig_extra(panel, target)) + "\n")
    (dest / "main" / "CMakeLists.txt").write_text(
        _main_cmake(a3d_root, model.name, a3d_source))
    (dest / "main" / "idf_component.yml").write_text(
        _idf_component_yml(target, panel, touch, idf, a3d_source))

    # Two workers on one bus have to take turns. Wired only when there are two,
    # so a single-core build does not pay for a mutex it can never contend.
    lock_wiring = ("" if workers < 2 else
                   "    // Two workers share one bus and it is not re-entrant.\n"
                   "    panel.lockBus   = &lockBus;\n"
                   "    panel.unlockBus = &unlockBus;\n")
    (dest / "main" / "main.cpp").write_text(MAIN_CPP.format(
        disclaimer=DISCLAIMER_C,
        model_name=model.name, sym=sym, panel_w=panel_w, panel_h=panel_h,
        workers=workers, tile_h=tile_h,
        panel_label=PANEL.PANELS[panel]["label"], panel_name=panel,
        lock_wiring=lock_wiring))
    (dest / "main" / "panel.h").write_text(PANEL.panel_h(panel))
    (dest / "main" / "panel.c").write_text(
        PANEL.panel_c(panel, touch, target, panel_w, panel_h))
    (dest / "main" / model.name).write_bytes(model.read_bytes())
    (dest / "README.md").write_text(README.format(
        disclaimer=DISCLAIMER_MD,
        project=project, source=source_name, target=target, workers=workers,
        tiles=tiles, model_name=model.name, model_bytes=model_bytes, sym=sym,
        app_kb=app_bytes // 1024, flash_mb=flash_mb, tile_h=tile_h,
        a3d_root=a3d_root,
        psram="on" if need_psram else "off",
        a3d_how=(_A3D_MANAGED if a3d_source == "managed"
                 else _A3D_PATH.format(a3d_root=a3d_root)),
        panel_label=PANEL.PANELS[panel]["label"],
        touch_label=PANEL.TOUCH[touch]["label"],
        panel_source=PANEL.PANELS[panel]["source"]))

    return dict(target=target, sym=sym, app_bytes=app_bytes, workers=workers,
                model_bytes=model_bytes, psram=need_psram, tiles=tiles,
                flash_mb=flash_mb, panel=(panel_w, panel_h), tile_h=tile_h,
                panel_id=panel, touch_id=touch, a3d_source=a3d_source,
                psram_mode=psram, a3d_git=A3D_GIT, a3d_ref=A3D_REF,
                components=PANEL.components(panel, touch))
