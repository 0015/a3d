set(src_dirs  ./
              ./src)
set(include_dirs  ./ 
                  ./src
                  ./src/REG)
idf_component_register(
  SRC_DIRS ${src_dirs}
  INCLUDE_DIRS ${include_dirs}
  # The split driver components are named EXPLICITLY. Up to ESP-IDF 5.x the
  # umbrella `driver` component pulled them in and "driver/gpio.h" resolved by
  # luck; on 6.0 it does not, and the build stops at "No such file or directory".
  # `driver` itself stays because XPowersCommon.tpp still includes the LEGACY
  # "driver/i2c.h", which is the one header that component still provides.
  REQUIRES port_bsp driver esp_driver_gpio esp_driver_i2c
)
add_compile_definitions(XPOWERS_CHIP_AXP2101 CONFIG_XPOWERS_ESP_IDF_NEW_API)
##REQUIRES