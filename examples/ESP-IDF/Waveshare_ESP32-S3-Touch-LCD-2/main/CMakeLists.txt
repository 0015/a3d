# main -> Waveshare_ESP32-S3-Touch-LCD-2 -> ESP-IDF -> examples -> a3d
set(A3D_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

# a3d is header-only: one include path, `src`, and nothing to link.
idf_component_register(
    SRCS "main.cpp" "s3_panel.cpp"
    INCLUDE_DIRS "." "${A3D_ROOT}/src"
    # esp_driver_ledc is named EXPLICITLY. On ESP-IDF 5.x the umbrella `driver`
    # component pulls it in and `#include "driver/ledc.h"` resolves by luck; on
    # 6.0 it does not, and the build fails with "driver/ledc.h: No such file or
    # directory". Naming what you actually use works on both.
    REQUIRES driver esp_driver_ledc esp_lcd
)
