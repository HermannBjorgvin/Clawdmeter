/*
 * @Description: Arduino_ETA4662.cpp
 * @version: V1.0.0
 * @Author: Xk_w
 * @Date: 2023-11-16 15:42:22
 * @LastEditors: Xk_w
 * @LastEditTime: 2024-02-28 14:59:57
 * @License: GPL 3.0
 */
#include "Arduino_ETA4662.h"

Arduino_ETA4662::Arduino_ETA4662(
    std::shared_ptr<Arduino_IIC_DriveBus> bus, uint8_t device_address, int8_t rst, int8_t iqr)
    : Arduino_IIC(bus, device_address, rst, iqr)
{
}

bool Arduino_ETA4662::begin(int32_t speed)
{
    return Arduino_IIC::begin(speed);
}

bool Arduino_ETA4662::IIC_Initialization(void)
{
    if (_rst != DRIVEBUS_DEFAULT_VALUE)
    {
    }
    else
    {
        // Software Rest
    }

    if (_bus->BufferOperation(_device_address, ETA4662_Initialization_BufferOperations,
                              sizeof(ETA4662_Initialization_BufferOperations)) == false)
    {
        return false;
    }

    return true;
}

int32_t Arduino_ETA4662::IIC_Read_Device_ID(void)
{
    uint8_t temp_buf = 0;
    if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_DEVICE_ID, &temp_buf) == true)
    {
        return (int32_t)temp_buf;
    }
    return -1;
}

bool Arduino_ETA4662::IIC_Write_Device_State(uint32_t device, uint8_t state)
{
    uint8_t temp_buf = 0;

    switch (device)
    {
    case Arduino_IIC_Power::Device::POWER_DEVICE_CHARGING_MODE:
        switch (state)
        {
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_ON:
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_POWER_ON_CONFIGURATION, &temp_buf) == true)
            {
                temp_buf &= 0B11110111;

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_POWER_ON_CONFIGURATION, temp_buf) == true)
                {
                    return true;
                }
            }
            break;
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_OFF:
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_POWER_ON_CONFIGURATION, &temp_buf) == true)
            {
                temp_buf |= 0B00001000;

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_POWER_ON_CONFIGURATION, temp_buf) == true)
                {
                    return true;
                }
            }
            break;

        default:
            break;
        }
        break;
    case Arduino_IIC_Power::Device::POWER_DEVICE_WATCHDOG_MODE:
        switch (state)
        {
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_ON:
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_CHARGE_TERMINATION_TIMER_CONTROL, &temp_buf) == true)
            {
                temp_buf |= 0B10000000;

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_CHARGE_TERMINATION_TIMER_CONTROL, temp_buf) == true)
                {
                    return true;
                }
            }
            break;
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_OFF:
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_CHARGE_TERMINATION_TIMER_CONTROL, &temp_buf) == true)
            {
                temp_buf &= 0B01111111;

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_CHARGE_TERMINATION_TIMER_CONTROL, temp_buf) == true)
                {
                    return true;
                }
            }
            break;
        default:
            break;
        }
        break;
    case Arduino_IIC_Power::Device::POWER_DEVICE_NTC_MODE:
        switch (state)
        {
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_ON:
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_MISCELLANEOUS_OPERATION_CONTROL, &temp_buf) == true)
            {
                temp_buf |= 0B10000000;

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_MISCELLANEOUS_OPERATION_CONTROL, temp_buf) == true)
                {
                    return true;
                }
            }
            break;
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_OFF:
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_MISCELLANEOUS_OPERATION_CONTROL, &temp_buf) == true)
            {
                temp_buf &= 0B01111111;

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_MISCELLANEOUS_OPERATION_CONTROL, temp_buf) == true)
                {
                    return true;
                }
            }
            break;

        default:
            break;
        }
        break;
    case Arduino_IIC_Power::Device::POWER_DEVICE_PCB_OTP_MODE:
        switch (state)
        {
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_ON:
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_SYSTEM_VOLTAGE_REGULATION, &temp_buf) == true)
            {
                temp_buf |= 0B10000000;

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_SYSTEM_VOLTAGE_REGULATION, temp_buf) == true)
                {
                    return true;
                }
            }
            break;
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_OFF:
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_SYSTEM_VOLTAGE_REGULATION, &temp_buf) == true)
            {
                temp_buf &= 0B01111111;

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_SYSTEM_VOLTAGE_REGULATION, temp_buf) == true)
                {
                    return true;
                }
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

bool Arduino_ETA4662::IIC_Write_Device_Value(uint32_t device, uint32_t value)
{
    uint8_t temp_buf = 0;

    switch (device)
    {
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_MINIMUM_INPUT_VOLTAGE_LIMIT: // 3880mV-5080mV(步进值：80mV)
        if (value >= 3880 && value <= 5080)
        {
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_INPUT_SOURCE_CONTROL, &temp_buf) == true)
            {
                temp_buf &= 0B00001111; // 清空
                temp_buf = temp_buf | (((value - 3880) / 80) << 4);

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_INPUT_SOURCE_CONTROL, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_CHARGING_TARGET_VOLTAGE_LIMIT: // 3600mV-4545mV(步进值：15mV)
        if (value >= 3600 && value <= 4545)
        {
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_CHARGE_VOLTAGE_CONTROL, &temp_buf) == true)
            {
                temp_buf &= 0B00000011; // 清空
                temp_buf = temp_buf | (((value - 3600) / 15) << 2);

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_CHARGE_VOLTAGE_CONTROL, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_SYSTEM_VOLTAGE_LIMIT: // 4200mV-4950mV(步进值：50mV)
        if (value >= 4200 && value <= 4950)
        {
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_SYSTEM_VOLTAGE_REGULATION, &temp_buf) == true)
            {
                temp_buf &= 0B11110000; // 清空
                temp_buf = temp_buf | ((value - 4200) / 50);

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_SYSTEM_VOLTAGE_REGULATION, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_INPUT_CURRENT_LIMIT: // 50mA-500mA(步进值：30mA)
        if (value >= 50 && value <= 500)
        {
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_INPUT_SOURCE_CONTROL, &temp_buf) == true)
            {
                temp_buf &= 0B11110000; // 清空
                temp_buf = temp_buf | ((value - 50) / 30);

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_INPUT_SOURCE_CONTROL, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_BATTERY_UVLO_THRESHOLD: // 2400mV-3030mV(步进值：90mV)
        if (value >= 2400 && value <= 3030)
        {
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_POWER_ON_CONFIGURATION, &temp_buf) == true)
            {
                temp_buf &= 0B11111000; // 清空
                temp_buf = temp_buf | ((value - 2400) / 90);

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_POWER_ON_CONFIGURATION, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_FAST_CHARGING_CURRENT_LIMIT: // 8mA-456mA(步进值：8mA)
        if (value >= 8 && value <= 456)
        {
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_CHARGE_CURRENT_CONTROL, &temp_buf) == true)
            {
                temp_buf &= 0B11000000; // 清空
                temp_buf = temp_buf | ((value - 8) / 8);

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_CHARGE_CURRENT_CONTROL, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_TERMINATION_PRECHARGE_CHARGING_CURRENT_LIMIT: // 1mA-31mA(步进值：2mA)
        if (value >= 1 && value <= 31)
        {
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_DISCHARGE_TERMINATION_CURRENT, &temp_buf) == true)
            {
                temp_buf &= 0B11110000; // 清空
                temp_buf = temp_buf | ((value - 1) / 2);

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_DISCHARGE_TERMINATION_CURRENT, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_BAT_TO_SYS_DISCHARGE_CURRENT_LIMIT: // 400mA-3200mA(步进值：200mA)
        if (value >= 400 && value <= 3200)
        {
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_DISCHARGE_TERMINATION_CURRENT, &temp_buf) == true)
            {
                temp_buf &= 0B00001111; // 清空
                temp_buf = temp_buf | (((value - 400) / 200) << 4);

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_DISCHARGE_TERMINATION_CURRENT, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_THERMAL_REGULATION_THRESHOLD: // 60度-120度(步进值：20度)
        if (value >= 60 && value <= 120)
        {
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_SYSTEM_VOLTAGE_REGULATION, &temp_buf) == true)
            {
                temp_buf &= 0B11001111; // 清空
                temp_buf = temp_buf | (((value - 60) / 20) << 4);

                if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_SYSTEM_VOLTAGE_REGULATION, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_WATCHDOG_TIMER: // 0s、40s、80s、160s
        if (value <= 160)
        {
            if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_CHARGE_TERMINATION_TIMER_CONTROL, &temp_buf) == true)
            {
                temp_buf &= 0B10011111; // 清空

                switch (value)
                {
                case 0:
                    // 上面已经清空了所以这里什么都不做
                    if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_CHARGE_TERMINATION_TIMER_CONTROL, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 40:
                    temp_buf = temp_buf | 0B00100000;
                    if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_CHARGE_TERMINATION_TIMER_CONTROL, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 80:
                    temp_buf = temp_buf | 0B01000000;
                    if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_CHARGE_TERMINATION_TIMER_CONTROL, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 160:
                    temp_buf = temp_buf | 0B01100000;
                    if (_bus->IIC_WriteC8D8(_device_address, ETA4662_RD_WR_CHARGE_TERMINATION_TIMER_CONTROL, temp_buf) == true)
                    {
                        return true;
                    }
                    break;

                default:
                    break;
                }
            }
        }
        break;

    default:
        break;
    }

    return false;
}

String Arduino_ETA4662::IIC_Read_Device_State(uint32_t information)
{
    uint8_t temp_buf = 0;

    switch (information)
    {

    case Arduino_IIC_Power::Status_Information::POWER_CHARGING_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_SYSTEM_STATUS, &temp_buf) == true)
        {
            temp_buf &= 0B00011000;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Not Charging";
                break;
            case 0B00001000:
                return "Pre Charge";
                break;
            case 0B00010000:
                return "Charge";
                break;
            case 0B00011000:
                return "Charge Done";
                break;

            default:
                return "->Read POWER_CHARGING_STATUS fail";
                break;
            }
        }

        return "->Read ETA4662_RD_SYSTEM_STATUS fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_INPUT_SOURCE_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_SYSTEM_STATUS, &temp_buf) == true)
        {
            temp_buf &= 0B00000010;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Power Fail";
                break;
            case 0B00000010:
                return "Power Good";
                break;

            default:
                return "->Read POWER_CHARGING_STATUS fail";
                break;
            }
        }

        return "->Read ETA4662_RD_SYSTEM_STATUS fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_SYSTEM_VOLTAGE_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_SYSTEM_STATUS, &temp_buf) == true)
        {
            temp_buf &= 0B00000100;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Not In PPM";
                break;
            case 0B00000100:
                return "In PPM";
                break;

            default:
                return "->Read POWER_SYSTEM_VOLTAGE_STATUS fail";
                break;
            }
        }

        return "->Read ETA4662_RD_SYSTEM_STATUS fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_THERMAL_REGULATION_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_SYSTEM_STATUS, &temp_buf) == true)
        {
            temp_buf &= 0B00000001;

            switch (temp_buf)
            {
            case 0B00000000:
                return "No Thermal Regulation";
                break;
            case 0B00000001:
                return "In Thermal Regulation";
                break;

            default:
                return "->Read POWER_THERMAL_REGULATION_STATUS fail";
                break;
            }
        }

        return "->Read ETA4662_RD_SYSTEM_STATUS fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_WATCHDOG_FAULT_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_SYSTEM_STATUS, &temp_buf) == true)
        {
            temp_buf &= 0B10000000;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Normal";
                break;
            case 0B10000000:
                return "Watchdog Timer Expiration";
                break;

            default:
                return "->Read POWER_WATCHDOG_FAULT_STATUS fail";
                break;
            }
        }

        return "->Read ETA4662_RD_SYSTEM_STATUS fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_INPUT_FAULT_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_FAULT, &temp_buf) == true)
        {
            temp_buf &= 0B00100000;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Normal";
                break;
            case 0B00100000:
                return " Input Fault";
                break;

            default:
                return "->Read POWER_INPUT_FAULT_STATUS fail";
                break;
            }
        }

        return "->Read ETA4662_RD_FAULT fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_THERMAL_SHUTDOWN_FAULT_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_FAULT, &temp_buf) == true)
        {
            temp_buf &= 0B00010000;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Normal";
                break;
            case 0B00010000:
                return "Thermal Shutdown";
                break;

            default:
                return "->Read POWER_THERMAL_SHUTDOWN_STATUS fail";
                break;
            }
        }

        return "->Read ETA4662_RD_FAULT fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_BATTERY_FAULT_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_FAULT, &temp_buf) == true)
        {
            temp_buf &= 0B00001000;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Normal";
                break;
            case 0B00001000:
                return "Battery OVP";
                break;

            default:
                return "->Read POWER_BATTERY_FAULT_STATUS fail";
                break;
            }
        }

        return "->Read ETA4662_RD_FAULT fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_SAFETY_TIMER_STATUS_FAULT_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_FAULT, &temp_buf) == true)
        {
            temp_buf &= 0B00000100;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Normal";
                break;
            case 0B00000100:
                return "Safety Timer Expiration";
                break;

            default:
                return "->Read POWER_SAFETY_TIMER_STATUS_FAULT_STATUS fail";
                break;
            }
        }

        return "->Read ETA4662_RD_FAULT fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_NTC_FAULT_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_FAULT, &temp_buf) == true)
        {
            temp_buf &= 0B00000011;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Normal";
                break;
            case 0B00000001:
                return "NTC Cold";
                break;
            case 0B00000010:
                return "NTC Hot";
                break;

            default:
                return "->Read POWER_NTC_FAULT_STATUS fail";
                break;
            }
        }

        return "->Read ETA4662_RD_FAULT fail";
        break;
    default:
        return "->No such information was found Read Power Status information fail ";
        break;
    }

    return "->Error reading IIC_Read_Information";
}

double Arduino_ETA4662::IIC_Read_Device_Value(uint32_t information)
{
    uint8_t temp_buf = 0;

    switch (information)
    {
    case Arduino_IIC_Power::Value_Information::POWER_THERMAL_REGULATION_THRESHOLD: // 60度、80度、100度、120度
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_SYSTEM_VOLTAGE_REGULATION, &temp_buf) == true)
        {
            temp_buf = (temp_buf & 0B00110000) >> 4;

            return int32_t(60 + (temp_buf * 20));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_MINIMUM_INPUT_VOLTAGE_LIMIT: // 3880mV-5080mV
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_INPUT_SOURCE_CONTROL, &temp_buf) == true)
        {
            temp_buf = (temp_buf & 0B11110000) >> 4;

            return int32_t(3880 + (temp_buf * 80));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_CHARGING_TARGET_VOLTAGE_LIMIT: // 3600mV-4545mV
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_CHARGE_VOLTAGE_CONTROL, &temp_buf) == true)
        {
            temp_buf = (temp_buf & 0B11111100) >> 2;

            return int32_t(3600 + (temp_buf * 15));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_SYSTEM_VOLTAGE_LIMIT: // 4200mV-4950mV
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_SYSTEM_VOLTAGE_REGULATION, &temp_buf) == true)
        {
            temp_buf &= 0B00001111;

            return int32_t(4200 + (temp_buf * 50));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_INPUT_CURRENT_LIMIT: // 50mA-500mA
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_INPUT_SOURCE_CONTROL, &temp_buf) == true)
        {
            temp_buf &= 0B00001111;

            return int32_t(50 + (temp_buf * 30));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_FAST_CHARGING_CURRENT_LIMIT: // 8mA-456mA
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_CHARGE_CURRENT_CONTROL, &temp_buf) == true)
        {
            temp_buf &= 0B00111111;

            return int32_t(8 + (temp_buf * 8));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_TERMINATION_PRECHARGE_CHARGING_CURRENT_LIMIT: // 1mA-31mA
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_DISCHARGE_TERMINATION_CURRENT, &temp_buf) == true)
        {
            temp_buf &= 0B00001111;

            return int32_t(1 + (temp_buf * 2));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_BAT_TO_SYS_DISCHARGE_CURRENT_LIMIT: // 400mA-3200mA
        if (_bus->IIC_ReadC8D8(_device_address, ETA4662_RD_WR_DISCHARGE_TERMINATION_CURRENT, &temp_buf) == true)
        {
            temp_buf = (temp_buf & 0B11110000) >> 4;

            return int32_t(400 + (temp_buf * 200));
        }
        break;

    default:
        break;
    }
    return -1;
}
