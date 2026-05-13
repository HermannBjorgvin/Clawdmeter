/*
 * @Description: Arduino_DriveBus.cpp
 * @version: V1.1.2
 * @Author: Xk_w
 * @Date: 2023-11-16 15:53:46
 * @LastEditors: Xk_w
 * @LastEditTime: 2024-02-28 14:43:56
 * @License: GPL 3.0
 */
#include "Arduino_DriveBus.h"

Arduino_IIC_DriveBus::Arduino_IIC_DriveBus() {}

bool Arduino_IIC_DriveBus::WriteC8D8(uint8_t c, uint8_t d)
{
    if (Write(c) == false)
    {
        log_e("->Write(c) fail");
        return false;
    }
    if (Write(d) == false)
    {
        log_e("->Write(d) fail");
        return false;
    }
    return true;
}

bool Arduino_IIC_DriveBus::BufferOperation(uint8_t device_address, const uint8_t *operations, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        switch (operations[i])
        {
        case Arduino_DriveBus_BufferOperation::BO_BEGIN_TRANSMISSION:
            BeginTransmission(device_address);
            break;
        case Arduino_DriveBus_BufferOperation::BO_WRITE:
            i++;
            if (Write(operations[i]) == false)
            {
                log_e("->Write(operations[i]) fail");
                return false;
            }
            break;
        case Arduino_DriveBus_BufferOperation::BO_WRITE_C8_D8:
            i++;
            if (Write(operations[i]) == false)
            {
                log_e("->Write(operations[i]) fail");
                return false;
            }
            i++;
            if (Write(operations[i]) == false)
            {
                log_e("->Write(operations[i]) fail");
                return false;
            }
            break;
        case Arduino_DriveBus_BufferOperation::BO_END_TRANSMISSION:
            if (EndTransmission() == false)
            {
                log_e("->EndTransmission() fail");
                return false;
            }
            break;
        case Arduino_DriveBus_BufferOperation::BO_DELAY:
            i++;
            delay(operations[i]);
            break;
        default:
            return false;
            break;
        }
    }

    return true;
}

bool Arduino_IIC_DriveBus::IIC_Device_7Bit_Scan(std::vector<unsigned char> *device_address)
{
    std::vector<unsigned char> device_address_temp; // 地址存储器

    for (uint8_t i = 1; i < 128; i++)
    {
        BeginTransmission(i);
        if (EndTransmission() == true)
        {
            device_address_temp.push_back(i);
        }
    }

    if (device_address_temp.empty() == false)
    {
        device_address->assign(device_address_temp.begin(), device_address_temp.end());
        return true;
    }
    return false;
}

bool Arduino_IIC_DriveBus::IIC_Write_Data(uint8_t device_address, const uint8_t *data, size_t length)
{
    BeginTransmission(device_address);
    if (Write(data, length) == false)
    {
        log_e("->Write(data, length) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_e("->EndTransmission() fail");
        return false;
    }
    return true;
}

bool Arduino_IIC_DriveBus::IIC_WriteC8D8(uint8_t device_address, uint8_t c, uint8_t d)
{
    BeginTransmission(device_address);
    if (WriteC8D8(c, d) == false)
    {
        log_e("->WriteC8D8(c, d) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_e("->EndTransmission() fail");
        return false;
    }
    return true;
}

bool Arduino_IIC_DriveBus::IIC_ReadC8_Data(uint8_t device_address, uint8_t c, uint8_t *d, size_t length)
{
    BeginTransmission(device_address);
    if (Write(c) == false)
    {
        log_e("->Write(c) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_e("->EndTransmission() fail");
        return false;
    }
    if (RequestFrom(device_address, length) == false)
    {
        log_e("->RequestFrom(device_address, length) fail");
        return false;
    }
    *d = Read();

    return true;
}

bool Arduino_IIC_DriveBus::IIC_ReadC8_Delay_Data(uint8_t device_address, uint8_t c, uint32_t delay_ms, uint8_t *d, size_t length)
{
    BeginTransmission(device_address);
    if (Write(c) == false)
    {
        log_e("->Write(c) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_e("->EndTransmission() fail");
        return false;
    }
    delay(delay_ms);
    if (RequestFrom(device_address, length) == false)
    {
        log_e("->RequestFrom(device_address, length) fail");
        return false;
    }
    *d = Read();

    return true;
}

bool Arduino_IIC_DriveBus::IIC_ReadC8D8(uint8_t device_address, uint8_t c, uint8_t *d)
{
    BeginTransmission(device_address);
    if (Write(c) == false)
    {
        log_e("->Write(c) fail");
        return false;
    }
    if (EndTransmission() == false)
    {
        log_e("->EndTransmission() fail");
        return false;
    }
    if (RequestFrom(device_address, 1) == false)
    {
        log_e("->RequestFrom(device_address, 1) fail");
        return false;
    }
    *d = Read();

    return true;
}