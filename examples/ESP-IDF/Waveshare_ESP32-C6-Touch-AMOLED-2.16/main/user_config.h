#ifndef USER_CONFIG_H
#define USER_CONFIG_H

#define BSP_I2C_NUM             (I2C_NUM_0)
#define BSP_LCD_SPI_NUM         (SPI2_HOST)

#define BSP_I2C_SCL             (GPIO_NUM_7)
#define BSP_I2C_SDA             (GPIO_NUM_8)

#define BSP_I2S_SCLK            (GPIO_NUM_20)
#define BSP_I2S_MCLK            (GPIO_NUM_19)
#define BSP_I2S_LCLK            (GPIO_NUM_22)
#define BSP_I2S_DOUT            (GPIO_NUM_23)
#define BSP_I2S_DSIN            (GPIO_NUM_21)
#define BSP_POWER_AMP_IO        (GPIO_NUM_NC)

#define BSP_LCD_H_RES           (480)
#define BSP_LCD_V_RES           (480)
#define BSP_LCD_CS              (GPIO_NUM_5)
#define BSP_LCD_PCLK            (GPIO_NUM_0)
#define BSP_LCD_DATA0           (GPIO_NUM_1)
#define BSP_LCD_DATA1           (GPIO_NUM_2)
#define BSP_LCD_DATA2           (GPIO_NUM_3)
#define BSP_LCD_DATA3           (GPIO_NUM_4)
#define BSP_LCD_BITS_PER_PIXEL  (16)
#define BSP_LCD_DMASIZE         (BSP_LCD_H_RES * BSP_LCD_V_RES)

#define BSP_LCD_BACKLIGHT       (GPIO_NUM_NC)
#define BSP_LCD_RST             (GPIO_NUM_NC)
#define BSP_LCD_TOUCH_RST       (GPIO_NUM_11)
#define BSP_LCD_TOUCH_INT       (GPIO_NUM_15)


#define BSP_SD_MOSI             (GPIO_NUM_1)
#define BSP_SD_MISO             (GPIO_NUM_2)
#define BSP_SD_CLK              (GPIO_NUM_0)
#define BSP_SD_CS               (GPIO_NUM_6)

#endif // !USER_CONFIG_H