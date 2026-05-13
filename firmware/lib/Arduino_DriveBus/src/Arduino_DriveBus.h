/*
 * @Description(CN):
 *      Arduino_DriveBus用于管理所有驱动总线 他与文件夹hardware里的底层驱动有关
 *
 *  @Description(EN):
 *      Arduino_DriveBus is used for managing all drive buses. It is related to the bottom-level
 *  drivers in the hardware folder.
 *
 * @version: V1.1.5
 * @Author: Xk_w
 * @Date: 2023-11-16 15:53:46
 * @LastEditors: Xk_w
 * @LastEditTime: 2024-02-28 14:50:20
 * @License: GPL 3.0
 */
#pragma once

#include <Arduino.h>
#include <iostream>
#include <memory>
#include <vector>
#include <numeric>

#define DRIVEBUS_DEFAULT_VALUE -1

enum Arduino_DriveBus_BufferOperation
{
    BO_BEGIN_TRANSMISSION,
    BO_WRITE,
    BO_WRITE_C8_D8,
    BO_END_TRANSMISSION,
    BO_DELAY,
};

class Arduino_IIC_DriveBus
{
public:
    Arduino_IIC_DriveBus();

    virtual bool begin(int32_t speed = DRIVEBUS_DEFAULT_VALUE) = 0;

    virtual void BeginTransmission(uint8_t device_address) = 0;
    virtual bool EndTransmission(void) = 0;
    virtual bool Write(uint8_t d) = 0;
    virtual bool Write(const uint8_t *data, size_t length) = 0;
    virtual uint8_t Read(void) = 0;
    virtual bool RequestFrom(uint8_t device_address, size_t length) = 0;
    virtual bool WriteC8D8(uint8_t c, uint8_t d);

    bool BufferOperation(uint8_t device_address, const uint8_t *operations, size_t length);

    bool IIC_Device_7Bit_Scan(std::vector<unsigned char> *device_address);

    bool IIC_Write_Data(uint8_t device_address, const uint8_t *data, size_t length);
    bool IIC_WriteC8D8(uint8_t device_address, uint8_t c, uint8_t d);
    bool IIC_ReadC8_Data(uint8_t device_address, uint8_t c, uint8_t *d, size_t length);
    bool IIC_ReadC8_Delay_Data(uint8_t device_address, uint8_t c, uint32_t delay_ms, uint8_t *d, size_t length);
    bool IIC_ReadC8D8(uint8_t device_address, uint8_t c, uint8_t *d);

protected:
    int32_t _speed;
};