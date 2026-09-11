idf_component_register(
    SRCS "amoled175.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd esp_driver_i2c
             espressif__esp_lcd_co5300 waveshare__esp_lcd_touch_cst9217
)
