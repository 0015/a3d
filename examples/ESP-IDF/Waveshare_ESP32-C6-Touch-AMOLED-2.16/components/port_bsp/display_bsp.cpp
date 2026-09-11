#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include "display_bsp.h"
#include "esp_lcd_sh8601.h"
#include "power_bsp.h"

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600}, 
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0x30}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 100}, 
};

DisplayPort::DisplayPort(I2cMasterBus &i2cbus,int width,int height,int scl, int d0, int d1, int d2, int d3, int cs, int tp_int, int tp_reset, spi_host_device_t spihost) : 
i2cbus_(i2cbus),
spihost_(spihost),
width_(width),
height_(height),
tp_int_(tp_int),
tp_reset_(tp_reset)
{
	int max_transfer_sz = width_ * 50 * 2;
	ESP_LOGI(TAG, "SPI BUS init");
	spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num  = scl,              
    buscfg.data0_io_num = d0,                   
    buscfg.data1_io_num = d1,                   
    buscfg.data2_io_num = d2,                   
    buscfg.data3_io_num = d3,                      
    buscfg.max_transfer_sz = max_transfer_sz;
    ESP_ERROR_CHECK(spi_bus_initialize(spihost, &buscfg, SPI_DMA_CH_AUTO));

	ESP_LOGI(TAG, "Install panel IO");

    esp_lcd_panel_io_spi_config_t io_config = {};
    // ESP-IDF 6.0 changed these from `int` to `gpio_num_t`. C converts int to an
    // enum implicitly; C++ does not, so the cast is required there - and it is
    // harmless on 5.x, where enum -> int is a standard conversion.
    io_config.cs_gpio_num = (gpio_num_t)cs;
    io_config.dc_gpio_num = GPIO_NUM_NC;   // QSPI: no separate D/C line
    io_config.spi_mode = 0;               
    io_config.pclk_hz = 40 * 1000 * 1000; 
    io_config.trans_queue_depth = 1;     
    io_config.on_color_trans_done = NULL;   
    io_config.user_ctx = NULL;          
    io_config.lcd_cmd_bits = 32;          
    io_config.lcd_param_bits = 8;         
    io_config.flags.quad_mode = true;                       
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spihost, &io_config, &io_handle));

    sh8601_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    vendor_config.flags.use_qspi_interface = 1;

	esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;
	
	ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
	ESP_ERROR_CHECK(DisplayPort_DispReset());
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
}

DisplayPort::~DisplayPort() {
}

int DisplayPort::DisplayPort_DispReset(void) {
	Axp2101_SetAldo3(1);
	vTaskDelay(pdMS_TO_TICKS(100));
	Axp2101_SetAldo3(0);
	vTaskDelay(pdMS_TO_TICKS(100));
	Axp2101_SetAldo3(1);
	vTaskDelay(pdMS_TO_TICKS(100));
	return ESP_OK;
} 

void DisplayPort::DisplayPort_TouchInit(void) {
    // Zero-initialised and then filled, NOT a designated-initialiser list.
    // esp_lcd_touch_config_t has members after the ones set here, and ESP-IDF
    // 6.0 builds with -Werror=missing-field-initializers, which rejects a
    // partial list outright. `= {}` sets the rest to zero, which is what the
    // partial list meant anyway.
    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max            = (uint16_t)width_;
    tp_cfg.y_max            = (uint16_t)height_;
    tp_cfg.rst_gpio_num     = (gpio_num_t)tp_reset_;
    tp_cfg.int_gpio_num     = (gpio_num_t)tp_int_;
    tp_cfg.levels.reset     = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy    = 1;
    tp_cfg.flags.mirror_x   = 0;
    tp_cfg.flags.mirror_y   = 1;

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    // Field by field rather than ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG(). The
    // vendor macro is a C designated-initialiser list that stops short of the
    // struct's later members, and ESP-IDF 6.0 compiles C++ with
    // -Werror=missing-field-initializers, which rejects that outright inside a
    // header this tree must not fork. Same values, written out; `= {}` zeroes
    // the rest, which is what the partial list meant.
    esp_lcd_panel_io_i2c_config_t tp_io_config = {};
    tp_io_config.dev_addr                     = ESP_LCD_TOUCH_IO_I2C_CST9217_ADDRESS;
    tp_io_config.control_phase_bytes          = 1;
    tp_io_config.dc_bit_offset                = 0;
    tp_io_config.lcd_cmd_bits                 = 8;
    tp_io_config.flags.disable_control_phase  = 1;
    tp_io_config.scl_speed_hz                 = 400000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2cbus_.Get_I2cBusHandle(), &tp_io_config, &tp_io_handle));
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, &ret_touch));
    is_touchinitialized_ = true;
}

void DisplayPort::Set_Backlight(uint8_t brightness) {
    if(brightness > 100) brightness = 100;
    brightness_ = brightness;
    uint8_t bl_val = (uint8_t)((brightness_ * 255) / 100);
    uint32_t CMD = 0x51;
    CMD &= 0xFF;
    CMD <<= 8;
    CMD |= 0x02 << 24;
    esp_lcd_panel_io_tx_param(io_handle, CMD, &bl_val,1);
}

void DisplayPort::Set_Rotate(uint8_t Rotate) {
    uint8_t rot = 0x00;
    uint32_t CMD = 0x36;
    CMD &= 0xFF;
    CMD <<= 8;
    CMD |= 0x02 << 24;
    if(Rotate == 1) {
        rot = 0x30;
    }
    else {
        rot = 0x00;
    }
    esp_lcd_panel_io_tx_param(io_handle, CMD, &rot,1);
}