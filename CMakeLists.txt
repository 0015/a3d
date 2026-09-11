###############################################################################
# a3d - a self-contained 3D renderer for embedded targets
#
# Header-only. There is nothing to compile and nothing to link: adding this
# directory adds include paths.
#
# ESP-IDF:      registered as a component; add "a3d" to your REQUIRES.
# Plain CMake:  target_link_libraries(app PRIVATE a3d_viewer)
#               - or a3d + a3d_backend_soft to drive the raw API yourself.
###############################################################################

if(ESP_PLATFORM OR DEFINED IDF_TARGET)

    # ONE include root, `src`, and both prefixes live under it: <a3d/...> for
    # the core, "backends/soft/a3d_soft_backend.h" and "loaders/..." for the
    # rest. That is not tidiness - Arduino adds exactly `<lib>/src` to the
    # include path and nothing else, so a single root is what lets this same
    # tree be an ESP-IDF component and an Arduino library at once.
    # REQUIRES, on a header-only component, is how the consumer gets the
    # include paths of anything a3d's own headers pull in. These three are
    # small and present in essentially every build:
    #   freertos, log  - backends/freertos/a3d_freertos_executor.h
    #   esp_partition  - loaders/a3d_load_esp_partition.h
    #
    # loaders/a3d_load_sd.h needs more than that, and it is NOT listed here:
    # making every user of a 3D library link FATFS and the SD host driver to
    # get a rasterizer is the wrong trade. An app that includes it adds
    #   REQUIRES sdmmc fatfs esp_driver_sdmmc
    # to its own component. This is documented in the README and at the top of
    # the header itself.
    idf_component_register(INCLUDE_DIRS "src"
                           REQUIRES freertos log esp_partition)

else()

    cmake_minimum_required(VERSION 3.16)
    project(a3d CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    # --- core: types, math, container reader, scene runtime, animation ------
    add_library(a3d INTERFACE)
    target_include_directories(a3d INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(a3d INTERFACE cxx_std_17)

    # --- the rasterizer, the canvas and the bitmap fonts -------------------
    add_library(a3d_backend_soft INTERFACE)
    target_include_directories(a3d_backend_soft INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(a3d_backend_soft INTERFACE a3d)

    # --- the wrapper: a model on a screen in four calls ---------------------
    # Separate from the core because it NAMES the rasterizer - it owns one per
    # tile worker - and the core's first rule is that no renderer type appears
    # in it. Link this one target and you have the other two.
    add_library(a3d_viewer INTERFACE)
    target_include_directories(a3d_viewer INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(a3d_viewer INTERFACE a3d a3d_backend_soft)

    # --- host preview tool: renders a .a3d container to a PPM --------------
    option(A3D_BUILD_TOOLS "Build the host viewer" OFF)
    if(A3D_BUILD_TOOLS)
        add_executable(a3d_view tools/a3d_view.cpp)
        target_link_libraries(a3d_view PRIVATE a3d a3d_backend_soft)
    endif()

endif()
