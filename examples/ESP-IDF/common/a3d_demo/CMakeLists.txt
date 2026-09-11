# The board-independent half of the demo. Every example project registers this
# same component, so the benchmark they report is the same object code and not
# several copies that have drifted.
#
# a3d_demo -> common -> ESP-IDF -> examples -> a3d
#
# FOUR levels, not three: this copy of the demo lives one directory deeper than
# the original in examples/common. Getting it wrong is a configure-time
# FATAL_ERROR about a missing fox.a3d, which points at the assets rather than
# at the path that went looking for them.
set(A3D_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

# WHERE THE MODELS ARE, AND WHY IT IS TWO PLACES.
#
# In the development tree they are the test fixtures, and pointing at those
# means the demo cannot be benchmarking a different Fox from the one the tests
# check. A RELEASE does not carry tests/, so make_dist.sh copies the two
# containers to examples/common/assets/ and this finds them there instead.
#
# Getting this wrong is a configure-time "cannot find EMBED_FILES", which is
# loud - but only to somebody who configures the project, and a release is
# usually assembled without doing that once.
if(EXISTS "${A3D_ROOT}/tests/fixtures/fox.a3d")
    set(A3D_DEMO_ASSETS "${A3D_ROOT}/tests/fixtures")
else()
    set(A3D_DEMO_ASSETS "${CMAKE_CURRENT_LIST_DIR}/../assets")
endif()
if(NOT EXISTS "${A3D_DEMO_ASSETS}/fox.a3d")
    message(FATAL_ERROR
        "a3d_demo: no fox.a3d in ${A3D_DEMO_ASSETS}. In a release these live in "
        "examples/common/assets/; regenerate them with "
        "python3 tools/a3d_export.py --check <model>.glb")
endif()

idf_component_register(
    SRCS "a3d_demo_app.cpp"
    INCLUDE_DIRS "." "${A3D_ROOT}/src"
    REQUIRES esp_timer heap freertos
    # Embedding puts the containers in .rodata, which on ESP32 is memory-mapped
    # flash. The rasterizer samples the textures straight out of flash - no
    # copy, no partition edit, no esptool step.
    EMBED_FILES "${A3D_DEMO_ASSETS}/fox.a3d"
                "${A3D_DEMO_ASSETS}/cesiumman.a3d"
)
