#include "board.h"
#include "pm1.h"
#include <Arduino.h>
#include <Wire.h>

// Bring up the internal I2C bus and wake the M5PM1 PMIC. The LCD rail is
// gated by PM1 GPIO2 — without this dance the panel stays dark no matter
// what the SPI bus does. Sequence mirrors M5GFX's board_M5StickS3 bring-up.
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);

    // Keep the PMIC awake on I2C and its watchdog off. The PM1 is always-on
    // while a battery is in, so stale values survive ESP32 resets — set them
    // explicitly every boot.
    pm1_write8(PM1_REG_I2C_CFG, 0x00);
    pm1_write8(PM1_REG_WDT_CNT, 0x00);

    // PM1 GPIO2 → gpio function, output, push-pull, high = LCD power on.
    pm1_bit_off(PM1_REG_GPIO_FUNC0, 1 << 2);
    pm1_bit_on (PM1_REG_GPIO_MODE,  1 << 2);
    pm1_bit_off(PM1_REG_GPIO_DRV,   1 << 2);
    pm1_bit_on (PM1_REG_GPIO_OUT,   1 << 2);
    delay(100);   // rail settle before display_hal_init() touches the panel
}
