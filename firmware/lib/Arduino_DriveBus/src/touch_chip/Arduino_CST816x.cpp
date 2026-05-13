/*
 * @Description: Arduino_CST816x.cpp
 * @version: V1.0.1
 * @Author: Xk_w
 * @Date: 2023-11-25 09:14:02
 * @LastEditors: Xk_w
 * @LastEditTime: 2024-02-28 15:00:37
 * @License: GPL 3.0
 */
#include "Arduino_CST816x.h"

Arduino_CST816x::Arduino_CST816x(
    std::shared_ptr<Arduino_IIC_DriveBus> bus, uint8_t device_address, int8_t rst, int8_t iqr,
    void (*Interrupt_Function)())
    : Arduino_IIC(bus, device_address, rst, iqr, Interrupt_Function)
{
}

bool Arduino_CST816x::begin(int32_t speed)
{
    return Arduino_IIC::begin(speed);
}

bool Arduino_CST816x::IIC_Initialization(void)
{
    if (_rst != DRIVEBUS_DEFAULT_VALUE)
    {
        pinMode(_rst, OUTPUT);
        digitalWrite(_rst, HIGH);
        delay(100);
        digitalWrite(_rst, LOW);
        delay(10);
        digitalWrite(_rst, HIGH);
        delay(200);
    }
    else
    {
        // Software Rest
    }

    if (_bus->BufferOperation(_device_address, CST816x_Initialization_BufferOperations,
                              sizeof(CST816x_Initialization_BufferOperations)) == false)
    {
        return false;
    }

    return true;
}

int32_t Arduino_CST816x::IIC_Read_Device_ID(void)
{
    uint8_t temp_buf = 0;
    if (_bus->IIC_ReadC8D8(_device_address, CST816x_RD_DEVICE_ID, &temp_buf) == true)
    {
        return (int32_t)temp_buf;
    }
    return -1;
}

bool Arduino_CST816x::IIC_Write_Device_State(uint32_t device, uint8_t state)
{
    uint8_t temp_buf = 0;

    switch (device)
    {
    case Arduino_IIC_Touch::Device::TOUCH_DEVICE_SLEEP_MODE:
        switch (state)
        {
        case Arduino_IIC_Touch::Device_State::TOUCH_DEVICE_ON:
            temp_buf = 0B00000011;
            if (_bus->IIC_WriteC8D8(_device_address, CST816x_WR_DEVICE_SLEEPMODE, temp_buf) == true)
            {
                return true;
            }
            break;
        case Arduino_IIC_Touch::Device_State::TOUCH_DEVICE_OFF: // 目前休眠功能只能进入不能退出
            temp_buf = 0B00000000;
            if (_bus->IIC_WriteC8D8(_device_address, CST816x_WR_DEVICE_SLEEPMODE, temp_buf) == true)
            {
                return true;
            }
            break;

        default:
            break;
        }
        break;
    case Arduino_IIC_Touch::Device::TOUCH_DEVICE_INTERRUPT_MODE:
        switch (state)
        {
        case Arduino_IIC_Touch::Device_Mode::TOUCH_DEVICE_INTERRUPT_TEST:
            temp_buf = 0B10000000;
            if (_bus->IIC_WriteC8D8(_device_address, CST816x_WR_DEVICE_INTERRUPT_MODE, temp_buf) == true)
            {
                return true;
            }
            break;
        case Arduino_IIC_Touch::Device_Mode::TOUCH_DEVICE_INTERRUPT_PERIODIC:
            temp_buf = 0B01000000;
            if (_bus->IIC_WriteC8D8(_device_address, CST816x_WR_DEVICE_INTERRUPT_MODE, temp_buf) == true)
            {
                return true;
            }
            break;
        case Arduino_IIC_Touch::Device_Mode::TOUCH_DEVICE_INTERRUPT_CHANGE:
            temp_buf = 0B00100000;
            if (_bus->IIC_WriteC8D8(_device_address, CST816x_WR_DEVICE_INTERRUPT_MODE, temp_buf) == true)
            {
                return true;
            }
            break;
        case Arduino_IIC_Touch::Device_Mode::TOUCH_DEVICE_INTERRUPT_MOTION:
            temp_buf = 0B00010000;
            if (_bus->IIC_WriteC8D8(_device_address, CST816x_WR_DEVICE_INTERRUPT_MODE, temp_buf) == true)
            {
                return true;
            }
            break;
        case Arduino_IIC_Touch::Device_Mode::TOUCH_DEVICE_INTERRUPT_ONCEWLP:
            temp_buf = 0B00000001;
            if (_bus->IIC_WriteC8D8(_device_address, CST816x_WR_DEVICE_INTERRUPT_MODE, temp_buf) == true)
            {
                return true;
            }
            break;

        default:
            break;
        }
        break;

    default:
        break;
    }
    return false;
}

String Arduino_CST816x::IIC_Read_Device_State(uint32_t information)
{
    uint8_t temp_buf = 0;

    switch (information)
    {
    case Arduino_IIC_Touch::Status_Information::TOUCH_GESTURE_ID:
        if (_bus->IIC_ReadC8D8(_device_address, CST816x_RD_DEVICE_GESTUREID, &temp_buf) == true)
        {
            switch (temp_buf)
            {
            case 0x00:
                return "NONE";
                break;
            case 0x01:
                return "Swipe Up";
                break;
            case 0x02:
                return "Swipe Down";
                break;
            case 0x03:
                return "Swipe Left";
                break;
            case 0x04:
                return "Swipe Right";
                break;
            case 0x05:
                return "Single Click";
                break;
            case 0x0B:
                return "Double Click";
                break;
            case 0x0C:
                return "Long Press";
                break;

            default:
                return "->Read TOUCH_GESTURE_ID fail";
                break;
            }
        }

        return "->Read CST816x_RD_DEVICE_GESTUREID fail";
        break;

    default:
        return "->No such information was found Read Touch Status information fail ";
        break;
    }
    return "->Error reading IIC_Read_Information";
}

double Arduino_CST816x::IIC_Read_Device_Value(uint32_t information)
{
    uint8_t temp_buf = 0;
    uint8_t temp_buf_2 = 0;

    switch (information)
    {
    case Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER:
        if (_bus->IIC_ReadC8D8(_device_address, CST816x_RD_DEVICE_FINGERNUM, &temp_buf) == true)
        {
            switch (temp_buf)
            {
            case 0:
                return 0;
                break;
            case 1:
                return 1;
                break;
            case 2:
                return 2;
                break;

            default:
                break;
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X:
        if (_bus->IIC_ReadC8D8(_device_address, CST816x_RD_DEVICE_XPOSH, &temp_buf) == true)
        {
            temp_buf &= 0B00001111;

            if (_bus->IIC_ReadC8D8(_device_address, CST816x_RD_DEVICE_XPOSL, &temp_buf_2) == true)
            {
                return int32_t(((int16_t)temp_buf << 8) | (int16_t)temp_buf_2);
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y:
        if (_bus->IIC_ReadC8D8(_device_address, CST816x_RD_DEVICE_YPOSH, &temp_buf) == true)
        {
            temp_buf &= 0B00001111;

            if (_bus->IIC_ReadC8D8(_device_address, CST816x_RD_DEVICE_YPOSL, &temp_buf_2) == true)
            {
                return int32_t(((int16_t)temp_buf << 8) | (int16_t)temp_buf_2);
            }
        }
        break;

    default:
        break;
    }
    return -1;
}
