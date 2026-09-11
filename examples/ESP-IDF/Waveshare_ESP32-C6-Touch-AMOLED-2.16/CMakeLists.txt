cmake_minimum_required(VERSION 3.16)

# The board-independent half of the demo lives one level up and is shared with
# the ESP32-S3 example, so both boards report the same benchmark.
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

project(a3d_c6_amoled)
