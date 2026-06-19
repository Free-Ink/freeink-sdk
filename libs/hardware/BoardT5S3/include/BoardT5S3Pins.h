#pragma once

#include <Arduino.h>

// LilyGO T5S3-4.7-e-paper-PRO / Lite
// https://github.com/Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO

#define T5S3_WIDTH 960
#define T5S3_HEIGHT 540
#define T5S3_LOGICAL_WIDTH 540
#define T5S3_LOGICAL_HEIGHT 960

#define T5S3_BQ25896_ADDR 0x6B
#define T5S3_BQ27220_ADDR 0x55
#define T5S3_GT911_ADDR 0x5D
#define T5S3_PCF85063_ADDR 0x51
#define T5S3_TPS65185_ADDR 0x68
#define T5S3_PCA9535_ADDR 0x20

#define T5S3_GPS_RXD 44
#define T5S3_GPS_TXD 43

#define T5S3_SCL 40
#define T5S3_SDA 39
#define T5S3_I2C_FREQ 400000

#define T5S3_SPI_MISO 21
#define T5S3_SPI_MOSI 13
#define T5S3_SPI_SCLK 14

#define T5S3_TOUCH_INT 3
#define T5S3_TOUCH_RST 9

#define T5S3_SD_CS 12

#define T5S3_LORA_CS 46
#define T5S3_LORA_IRQ 10
#define T5S3_LORA_RST 1
#define T5S3_LORA_BUSY 47

#define T5S3_BL_EN 11
#define T5S3_PCA9535_INT 38
#define T5S3_BOOT_BTN 0

#define EP_D7 8
#define EP_D6 18
#define EP_D5 17
#define EP_D4 16
#define EP_D3 15
#define EP_D2 7
#define EP_D1 6
#define EP_D0 5
#define EP_CKV 48
#define EP_STH 41
#define EP_LEH 42
#define EP_STV 45
#define EP_CKH 4

// PCA9535 linear IO indexes, matching IO0..IO15.
// IO1x maps to port 1 bit x, so IO10 is linear index 8.
#define PCA9535_IO10_EP_OE 8
#define PCA9535_IO11_EP_MODE 9
#define PCA9535_IO12_BUTTON 10
#define PCA9535_IO13_TPS_PWRUP 11
#define PCA9535_IO14_VCOM_CTRL 12
#define PCA9535_IO15_TPS_WAKEUP 13
#define PCA9535_IO16_TPS_PWR_GOOD 14
#define PCA9535_IO17_TPS_INT 15

#define PCA9535_IO00_LORA_GPS_EN 0
