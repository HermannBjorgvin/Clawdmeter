/*
 * @Description: Arduino_FT3x68.cpp
 * @version: V1.0.0
 * @Author: Xk_w
 * @Date: 2023-11-25 09:14:02
 * @LastEditors: Xk_w
 * @LastEditTime: 2024-02-28 15:01:12
 * @License: GPL 3.0
 */
#include "Arduino_FT3x68.h"

Arduino_FT3x68::Arduino_FT3x68(
    std::shared_ptr<Arduino_IIC_DriveBus> bus, uint8_t device_address, int8_t rst, int8_t iqr,
    void (*Interrupt_Function)())
    : Arduino_IIC(bus, device_address, rst, iqr, Interrupt_Function)
{
}

bool Arduino_FT3x68::begin(int32_t speed)
{
    return Arduino_IIC::begin(speed);
}

bool Arduino_FT3x68::IIC_Initialization(void)
{
    if (_rst != DRIVEBUS_DEFAULT_VALUE)
    {
        pinMode(_rst, OUTPUT);
        digitalWrite(_rst, HIGH);
        delay(1);
        digitalWrite(_rst, LOW);
        delay(20);
        digitalWrite(_rst, HIGH);
        delay(50);
    }
    else
    {
        // Software Rest
    }

    if (_bus->BufferOperation(_device_address, FT3x68_Initialization_BufferOperations,
                              sizeof(FT3x68_Initialization_BufferOperations)) == false)
    {
        return false;
    }

    return true;
}

int32_t Arduino_FT3x68::IIC_Read_Device_ID(void)
{
    uint8_t temp_buf = 0;
    if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_ID, &temp_buf) == true)
    {
        return (int32_t)temp_buf;
    }
    return -1;
}

bool Arduino_FT3x68::IIC_Write_Device_State(uint32_t device, uint8_t state)
{
    uint8_t temp_buf = 0;

    switch (device)
    {
    case Arduino_IIC_Touch::Device::TOUCH_POWER_MODE:
        switch (state)
        {
        case Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_ACTIVE:
            temp_buf = 0B00000000;
            if (_bus->IIC_WriteC8D8(_device_address, FT3x68_RD_WR_DEVICE_POWER_MODE, temp_buf) == true)
            {
                return true;
            }
            break;
        case Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR:
            temp_buf = 0B00000001;
            if (_bus->IIC_WriteC8D8(_device_address, FT3x68_RD_WR_DEVICE_POWER_MODE, temp_buf) == true)
            {
                return true;
            }
            break;
        case Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_STANDBY:
            temp_buf = 0B00000010;
            if (_bus->IIC_WriteC8D8(_device_address, FT3x68_RD_WR_DEVICE_POWER_MODE, temp_buf) == true)
            {
                return true;
            }
            break;
        case Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_HIBERNATE:
            temp_buf = 0B00000011;
            if (_bus->IIC_WriteC8D8(_device_address, FT3x68_RD_WR_DEVICE_POWER_MODE, temp_buf) == true)
            {
                return true;
            }
            break;

        default:
            break;
        }
        break;
    case Arduino_IIC_Touch::Device::TOUCH_PROXIMITY_SENSING_MODE:
        switch (state)
        {
        case Arduino_IIC_Touch::Device_State::TOUCH_DEVICE_ON:
            temp_buf = 0B00000001;
            if (_bus->IIC_WriteC8D8(_device_address, FT3x68_RD_WR_DEVICE_PROXIMITY_SENSING_MODE, temp_buf) == true)
            {
                return true;
            }
            break;
        case Arduino_IIC_Touch::Device_State::TOUCH_DEVICE_OFF:
            temp_buf = 0B00000000;
            if (_bus->IIC_WriteC8D8(_device_address, FT3x68_RD_WR_DEVICE_PROXIMITY_SENSING_MODE, temp_buf) == true)
            {
                return true;
            }
            break;

        default:
            break;
        }
        break;
    case Arduino_IIC_Touch::Device::TOUCH_GESTUREID_MODE:
        switch (state)
        {
        case Arduino_IIC_Touch::Device_State::TOUCH_DEVICE_ON:
            temp_buf = 0B00000001;
            if (_bus->IIC_WriteC8D8(_device_address, FT3x68_RD_WR_DEVICE_GESTUREID_MODE, temp_buf) == true)
            {
                return true;
            }
            break;
        case Arduino_IIC_Touch::Device_State::TOUCH_DEVICE_OFF:
            temp_buf = 0B00000000;
            if (_bus->IIC_WriteC8D8(_device_address, FT3x68_RD_WR_DEVICE_GESTUREID_MODE, temp_buf) == true)
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

String Arduino_FT3x68::IIC_Read_Device_State(uint32_t information)
{
    uint8_t temp_buf = 0;

    switch (information)
    {
    case Arduino_IIC_Touch::Status_Information::TOUCH_GESTURE_ID:
        if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_GESTUREID, &temp_buf) == true)
        {
            switch (temp_buf)
            {
            case 0x00:
                return "No Gesture";
                break;
            case 0x20:
                return "Swipe Left";
                break;
            case 0x21:
                return "Swipe Right";
                break;
            case 0x22:
                return "Swipe Up";
                break;
            case 0x23:
                return "Swipe Down";
                break;
            case 0x24:
                return "Double Click";
                break;

            default:
                return "->Read TOUCH_GESTURE_ID fail";
                break;
            }
        }

        return "->Read FT3x68_RD_DEVICE_GESTUREID fail";
        break;

    default:
        return "->No such information was found Read Touch Status information fail ";
        break;
    }
    return "->Error reading IIC_Read_Information";
}

double Arduino_FT3x68::IIC_Read_Device_Value(uint32_t information)
{
    uint8_t temp_buf = 0;
    uint8_t temp_buf_2 = 0;

    switch (information)
    {
    case Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER:
        if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_FINGERNUM, &temp_buf) == true)
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
        if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_X1POSH, &temp_buf) == true)
        {
            temp_buf &= 0B00001111;

            if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_X1POSL, &temp_buf_2) == true)
            {
                return int32_t(((int16_t)temp_buf << 8) | (int16_t)temp_buf_2);
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y:
        if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_Y1POSH, &temp_buf) == true)
        {
            temp_buf &= 0B00001111;

            if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_Y1POSL, &temp_buf_2) == true)
            {
                return int32_t(((int16_t)temp_buf << 8) | (int16_t)temp_buf_2);
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH1_COORDINATE_X:
        if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_X1POSH, &temp_buf) == true)
        {
            temp_buf &= 0B00001111;

            if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_X1POSL, &temp_buf_2) == true)
            {
                return int32_t(((int16_t)temp_buf << 8) | (int16_t)temp_buf_2);
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH1_COORDINATE_Y:
        if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_Y1POSH, &temp_buf) == true)
        {
            temp_buf &= 0B00001111;

            if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_Y1POSL, &temp_buf_2) == true)
            {
                return int32_t(((int16_t)temp_buf << 8) | (int16_t)temp_buf_2);
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH2_COORDINATE_X:
        if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_X2POSH, &temp_buf) == true)
        {
            temp_buf &= 0B00001111;

            if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_X2POSL, &temp_buf_2) == true)
            {
                return int32_t(((int16_t)temp_buf << 8) | (int16_t)temp_buf_2);
            }
        }
        break;
    case Arduino_IIC_Touch::Value_Information::TOUCH2_COORDINATE_Y:
        if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_Y2POSH, &temp_buf) == true)
        {
            temp_buf &= 0B00001111;

            if (_bus->IIC_ReadC8D8(_device_address, FT3x68_RD_DEVICE_Y2POSL, &temp_buf_2) == true)
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
