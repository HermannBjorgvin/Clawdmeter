/*
 * @Description: Arduino_CST2xxSE.cpp
 * @version: V1.0.0
 * @Author: Xk_w
 * @Date: 2023-11-25 09:14:02
 * @LastEditors: Xk_w
 * @LastEditTime: 2024-02-28 15:00:18
 * @License: GPL 3.0
 */
#include "Arduino_CST2xxSE.h"

Arduino_CST2xxSE::Arduino_CST2xxSE(
    std::shared_ptr<Arduino_IIC_DriveBus> bus, uint8_t device_address, int8_t rst, int8_t iqr,
    void (*Interrupt_Function)())
    : Arduino_IIC(bus, device_address, rst, iqr, Interrupt_Function)
{
}

bool Arduino_CST2xxSE::begin(int32_t speed)
{
    return Arduino_IIC::begin(speed);
}

bool Arduino_CST2xxSE::IIC_Initialization(void)
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

    if (_bus->BufferOperation(_device_address, CST2xxSE_Initialization_BufferOperations,
                              sizeof(CST2xxSE_Initialization_BufferOperations)) == false)
    {
        return false;
    }

    return true;
}

int32_t Arduino_CST2xxSE::IIC_Read_Device_ID(void)
{
    uint8_t temp_buf = 0;
    if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_ID, &temp_buf) == true)
    {
        return (int32_t)temp_buf;
    }
    return -1;
}

bool Arduino_CST2xxSE::IIC_Write_Device_State(uint32_t device, uint8_t state)
{
    uint8_t temp_buf = 0;

    switch (device)
    {
    case Arduino_IIC_Touch::Device::TOUCH_DEVICE_SLEEP_MODE:
        switch (state)
        {
        case Arduino_IIC_Touch::Device_State::TOUCH_DEVICE_ON:
            temp_buf = 0B00000101;
            if (_bus->IIC_WriteC8D8(_device_address, CST2xxSE_WR_DEVICE_SLEEPMODE, temp_buf) == true)
            {
                return true;
            }
            break;
        case Arduino_IIC_Touch::Device_State::TOUCH_DEVICE_OFF: // 目前休眠功能只能进入不能退出 要退出只能复位芯片
            temp_buf = 0B00000010;
            if (_bus->IIC_WriteC8D8(_device_address, CST2xxSE_WR_DEVICE_SYSTEM_RESET, temp_buf) == true)
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

String Arduino_CST2xxSE::IIC_Read_Device_State(uint32_t information)
{
    return "->Error reading IIC_Read_Information";
}

double Arduino_CST2xxSE::IIC_Read_Device_Value(uint32_t information)
{
    uint8_t temp_buf = 0;
    uint8_t temp_buf_2 = 0;

    switch (information)
    {
    case Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_FINGERNUM, &temp_buf) == true)
        {
            switch (temp_buf & 0B00001111)
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
            case 3:
                return 3;
                break;
            case 4:
                return 4;
                break;
            case 5:
                return 5;
                break;

            default:
                break;
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_X1POSH, &temp_buf) == true)
        {
            if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_X1POSL, &temp_buf_2) == true)
            {
                return int32_t(((int32_t)temp_buf << 4) | (int32_t)(temp_buf_2 & 0B11110000 >> 4));
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_Y1POSH, &temp_buf) == true)
        {
            if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_Y1POSL, &temp_buf_2) == true)
            {
                return int32_t(((int32_t)temp_buf << 4) | (int32_t)(temp_buf_2 & 0B00001111));
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH1_COORDINATE_X:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_X1POSH, &temp_buf) == true)
        {
            if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_X1POSL, &temp_buf_2) == true)
            {
                return int32_t(((int32_t)temp_buf << 4) | (int32_t)(temp_buf_2 & 0B11110000 >> 4));
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH1_COORDINATE_Y:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_Y1POSH, &temp_buf) == true)
        {
            if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_Y1POSL, &temp_buf_2) == true)
            {
                return int32_t(((int32_t)temp_buf << 4) | (int32_t)(temp_buf_2 & 0B00001111));
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH2_COORDINATE_X:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_X2POSH, &temp_buf) == true)
        {
            if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_X2POSL, &temp_buf_2) == true)
            {
                return int32_t(((int32_t)temp_buf << 4) | (int32_t)(temp_buf_2 & 0B11110000 >> 4));
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH2_COORDINATE_Y:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_Y2POSH, &temp_buf) == true)
        {
            if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_Y2POSL, &temp_buf_2) == true)
            {
                return int32_t(((int32_t)temp_buf << 4) | (int32_t)(temp_buf_2 & 0B00001111));
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH3_COORDINATE_X:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_X3POSH, &temp_buf) == true)
        {
            if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_X3POSL, &temp_buf_2) == true)
            {
                return int32_t(((int32_t)temp_buf << 4) | (int32_t)(temp_buf_2 & 0B11110000 >> 4));
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH3_COORDINATE_Y:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_Y3POSH, &temp_buf) == true)
        {
            if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_Y3POSL, &temp_buf_2) == true)
            {
                return int32_t(((int32_t)temp_buf << 4) | (int32_t)(temp_buf_2 & 0B00001111));
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH4_COORDINATE_X:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_X4POSH, &temp_buf) == true)
        {
            if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_X4POSL, &temp_buf_2) == true)
            {
                return int32_t(((int32_t)temp_buf << 4) | (int32_t)(temp_buf_2 & 0B11110000 >> 4));
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH4_COORDINATE_Y:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_Y4POSH, &temp_buf) == true)
        {
            if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_Y4POSL, &temp_buf_2) == true)
            {
                return int32_t(((int32_t)temp_buf << 4) | (int32_t)(temp_buf_2 & 0B00001111));
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH5_COORDINATE_X:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_X5POSH, &temp_buf) == true)
        {
            if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_X5POSL, &temp_buf_2) == true)
            {
                return int32_t(((int32_t)temp_buf << 4) | (int32_t)(temp_buf_2 & 0B11110000 >> 4));
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH5_COORDINATE_Y:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_Y5POSH, &temp_buf) == true)
        {
            if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_Y5POSL, &temp_buf_2) == true)
            {
                return int32_t(((int32_t)temp_buf << 4) | (int32_t)(temp_buf_2 & 0B00001111));
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH1_PRESSURE_VALUE:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_TOUCH1_PRESSURE_VALUE, &temp_buf) == true)
        {
            return int32_t(temp_buf);
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH2_PRESSURE_VALUE:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_TOUCH2_PRESSURE_VALUE, &temp_buf) == true)
        {
            return int32_t(temp_buf);
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH3_PRESSURE_VALUE:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_TOUCH3_PRESSURE_VALUE, &temp_buf) == true)
        {
            return int32_t(temp_buf);
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH4_PRESSURE_VALUE:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_TOUCH4_PRESSURE_VALUE, &temp_buf) == true)
        {
            return int32_t(temp_buf);
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH5_PRESSURE_VALUE:
        if (_bus->IIC_ReadC8D8(_device_address, CST2xxSE_RD_DEVICE_TOUCH5_PRESSURE_VALUE, &temp_buf) == true)
        {
            return int32_t(temp_buf);
        }
        break;

    default:
        break;
    }
    return -1;
}
