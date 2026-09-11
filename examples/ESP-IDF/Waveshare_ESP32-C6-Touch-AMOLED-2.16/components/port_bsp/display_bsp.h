#pragma once

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>

#include "i2c_bsp.h"
#include "esp_lcd_touch_cst9217.h"

class DisplayPort {
private:
    const char * TAG = "DisplayPort";
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_touch_handle_t ret_touch = NULL;
    I2cMasterBus &i2cbus_;
    spi_host_device_t spihost_;
    int width_;
    int height_;
    int tp_int_;
    int tp_reset_;
    bool is_touchinitialized_ = false;
    uint8_t brightness_ = 100;

    int DisplayPort_DispReset(void);
public:
    DisplayPort(I2cMasterBus &i2cbus,int width,int height,int scl = 0, int d0 = 1, int d1 = 2, int d2 = 3, int d3 = 4, int cs = 15, int tp_int = 5, int tp_reset = 11, spi_host_device_t spihost = SPI2_HOST);
    ~DisplayPort();

    void DisplayPort_TouchInit(void);
    void Set_Backlight(uint8_t brightness);
    void Set_Rotate(uint8_t Rotate);

    spi_host_device_t Get_SpiHost() { return spihost_; }
    esp_lcd_panel_handle_t Get_PanelHandle() { return panel_handle; }
    esp_lcd_panel_io_handle_t Get_IoHandle() { return io_handle; }
    int Get_Width() { return width_; }
    int Get_Height() { return height_; }
    esp_lcd_touch_handle_t Get_TouchHandle() { return ret_touch; }
    bool Get_TouchInitialized() { return is_touchinitialized_; }
    uint8_t Get_Brightness() { return brightness_; }
};