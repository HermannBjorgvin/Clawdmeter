/*
 * @Description(CN):
 *      这是一个使用硬件IIC的IIC底层驱动文件
 *
 * @Description(EN):
 *      This is a bottom-layer IIC driver file using hardware IIC.
 *
 * @version: V1.0.1
 * @Author: Xk_w
 * @Date: 2023-11-16 15:45:17
 * @LastEditors: Xk_w
 * @LastEditTime: 2023-12-27 14:06:54
 * @License: GPL 3.0
 */
#pragma once

#include <Wire.h>
#include "../Arduino_DriveBus.h"

#define IIC_DEFAULT_SPEED 100000UL

class Arduino_HWIIC : public Arduino_IIC_DriveBus
{
public:
    Arduino_HWIIC(int8_t sda = DRIVEBUS_DEFAULT_VALUE, int8_t scl = DRIVEBUS_DEFAULT_VALUE,
                  TwoWire *wire = &Wire);

    bool begin(int32_t speed = DRIVEBUS_DEFAULT_VALUE) override;
    void BeginTransmission(uint8_t device_address) override;
    bool EndTransmission(void) override;
    bool Write(uint8_t d) override;
    bool Write(const uint8_t *data, size_t length) override;
    uint8_t Read(void) override;
    bool RequestFrom(uint8_t device_address, size_t length) override;

private:
    int8_t _sda, _scl;

    TwoWire *_wire;
};