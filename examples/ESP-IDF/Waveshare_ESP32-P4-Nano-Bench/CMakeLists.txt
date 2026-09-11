cmake_minimum_required(VERSION 3.16)

# TWO component directories, and NEITHER is a copy.
#
#   ../common                              the benchmark and the viewer, shared
#                                          with the S3 and C6 projects, so all
#                                          of them report the same object code
#   ../Waveshare_ESP32-P4-Nano/components  the board: JD9365 DSI + GT911 touch
#
# The board component is referenced where it already lives rather than copied
# in. A second copy of a panel bring-up is a second thing to keep in step, and
# the whole reason the comparison table means anything is that nothing above
# the panel differs between boards.
# ../common is this folder's own copy of the demo - the one with no benchmark.
#
# The BOARD component lives in this project's own components/ and is found
# without being named: ESP-IDF adds <project>/components automatically. It used
# to be referenced two levels up, in examples/Waveshare_ESP32-P4-Nano, which
# made this project depend on a directory that looked like debris beside it -
# and the first person to tidy that away got
#   Directory specified in EXTRA_COMPONENT_DIRS doesn't exist
# An example that only builds when a sibling directory happens to be present is
# not an example somebody can use.
set(EXTRA_COMPONENT_DIRS "../common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ESP-IDF 5.5.4 is the toolchain this example was built, flashed and MEASURED
# on, and it is the one every number in a3d's README describes. 6.x builds and
# runs - that is checked before each release - but on the ESP32-P4 a3d measured
# about 9% slower off screen under 6.0.2 than under 5.5.4, confirmed twice on
# two different applications. The S3 and C6 were never checked, so nothing is
# claimed for them either way.
#
# A warning rather than a hard version bound: somebody who already has 6.x
# installed should not be stopped, only told which toolchain the numbers came
# from.
if(DEFINED IDF_VERSION_MAJOR AND IDF_VERSION_MAJOR GREATER_EQUAL 6)
    message(WARNING
        "a3d: this example is built and measured on ESP-IDF 5.5.4; you are on "
        "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}. It builds and runs, but "
        "a3d measured ~9% slower on the ESP32-P4 under 6.0.2. Use 5.5.x to "
        "reproduce the published frame times.")
endif()


# Only main and what it actually depends on. Without this the registry pulls
# the whole default component set into a build that needs a panel and a timer.
idf_build_set_property(MINIMAL_BUILD ON)

project(a3d_p4_nano_bench)
