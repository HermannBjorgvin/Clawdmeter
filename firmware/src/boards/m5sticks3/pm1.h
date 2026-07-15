#pragma once
#include <stdint.h>

// Minimal register access for M5Stack's M5PM1 PMIC (I2C 0x6E on the internal
// bus). Register map from M5Unified's M5PM1_Class plus the M5GFX / M5Unified
// StickS3 bring-up sequences (both hardware-tested upstream):
//
//   0x04 PWR_SRC      0x09 I2C_CFG (0 = never idle-sleep)   0x0A WDT_CNT
//   0x0C SYS_CMD (0xA1 = shutdown)
//   0x10 GPIO_MODE    0x11 GPIO_OUT    0x13 GPIO_DRV    0x16 GPIO_FUNC0
//   0x22/0x23 VBAT mV (LE)             0x24/0x25 VIN mV (LE)
//
// PM1 GPIO2 gates the LCD rail, PM1 GPIO3 the AW8737 speaker amp.

#define PM1_ADDR             0x6E

#define PM1_REG_PWR_SRC      0x04
#define PM1_REG_I2C_CFG      0x09
#define PM1_REG_WDT_CNT      0x0A
#define PM1_REG_SYS_CMD      0x0C
#define PM1_REG_GPIO_MODE    0x10
#define PM1_REG_GPIO_OUT     0x11
#define PM1_REG_GPIO_DRV     0x13
#define PM1_REG_GPIO_FUNC0   0x16
#define PM1_REG_VBAT_L       0x22
#define PM1_REG_VIN_L        0x24

#define PM1_SYS_CMD_SHUTDOWN 0xA1

bool pm1_write8(uint8_t reg, uint8_t val);
int  pm1_read8(uint8_t reg);      // -1 on I2C error
int  pm1_read16(uint8_t reg_l);   // little-endian register pair, -1 on error
bool pm1_bit_on(uint8_t reg, uint8_t mask);
bool pm1_bit_off(uint8_t reg, uint8_t mask);
