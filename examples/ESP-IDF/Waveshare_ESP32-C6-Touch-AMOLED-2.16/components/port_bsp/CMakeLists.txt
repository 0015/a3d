# The split driver components are named EXPLICITLY. Up to ESP-IDF 5.x the
# umbrella `driver` component pulled them in and "driver/gpio.h" resolved by
# luck; on 6.0 it does not, and the build stops at "No such file or directory".
# `driver` itself stays because XPowersCommon.tpp still includes the LEGACY
# "driver/i2c.h", which is the one header that component still provides.
idf_component_register(
    SRCS 
    "i2c_bsp.cpp" 
    "display_bsp.cpp" 
    PRIV_REQUIRES 
    driver
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_spi
    pmicpower
    espressif__esp_lcd_sh8601
    waveshare__esp_lcd_touch_cst9217
    esp_timer
    INCLUDE_DIRS 
    "./")
