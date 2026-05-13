/*
 * @Description: SY6970.cpp
 * @version: V1.0.0
 * @Author: Xk_w
 * @Date: 2023-11-16 15:42:22
 * @LastEditors: Xk_w
 * @LastEditTime: 2024-02-28 15:00:06
 * @License: GPL 3.0
 */
#include "Arduino_SY6970.h"

Arduino_SY6970::Arduino_SY6970(
    std::shared_ptr<Arduino_IIC_DriveBus> bus, uint8_t device_address, int8_t rst, int8_t iqr)
    : Arduino_IIC(bus, device_address, rst, iqr)
{
}

bool Arduino_SY6970::begin(int32_t speed)
{
    return Arduino_IIC::begin(speed);
}

bool Arduino_SY6970::IIC_Initialization(void)
{
    if (_rst != DRIVEBUS_DEFAULT_VALUE)
    {
    }
    else
    {
        // Software Rest Or NULL
    }

    if (_bus->BufferOperation(_device_address, SY6970_Initialization_BufferOperations,
                              sizeof(SY6970_Initialization_BufferOperations)) == false)
    {
        return false;
    }

    return true;
}

int32_t Arduino_SY6970::IIC_Read_Device_ID(void)
{
    uint8_t temp_buf = 0;
    if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_14, &temp_buf) == true)
    {
        return (int32_t)temp_buf;
    }
    return -1;
}

bool Arduino_SY6970::IIC_Write_Device_State(uint32_t device, uint8_t state)
{
    uint8_t temp_buf = 0;

    switch (device)
    {
    case Arduino_IIC_Power::Device::POWER_DEVICE_ADC_MEASURE:
        switch (state)
        {
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_ON:
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_02, &temp_buf) == true)
            {
                temp_buf |= 0B11000000;

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_02, temp_buf) == true)
                {
                    return true;
                }
            }
            break;
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_OFF:
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_02, &temp_buf) == true)
            {
                temp_buf &= 0B00111111;

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_02, temp_buf) == true)
                {
                    return true;
                }
            }
            break;

        default:
            break;
        }

        break;
    case Arduino_IIC_Power::Device::POWER_DEVICE_HIZ_MODE:
        switch (state)
        {
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_ON:
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_00, &temp_buf) == true)
            {
                temp_buf |= 0B10000000;

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_00, temp_buf) == true)
                {
                    return true;
                }
            }
            break;
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_OFF:
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_00, &temp_buf) == true)
            {
                temp_buf &= 0B01111111;

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_00, temp_buf) == true)
                {
                    return true;
                }
            }
            break;

        default:
            break;
        }

        break;
    case Arduino_IIC_Power::Device::POWER_DEVICE_WATCHDOG_TIMER_RESET:
        switch (state)
        {
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_ON:
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_03, &temp_buf) == true)
            {
                temp_buf |= 0B01000000;

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_03, temp_buf) == true)
                {
                    return true;
                }
            }
            break;
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_OFF:
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_03, &temp_buf) == true)
            {
                temp_buf &= 0B10111111;

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_03, temp_buf) == true)
                {
                    return true;
                }
            }
            break;

        default:
            break;
        }

        break;
    case Arduino_IIC_Power::Device::POWER_DEVICE_OTG_MODE:
        switch (state)
        {
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_ON:
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_03, &temp_buf) == true)
            {
                temp_buf |= 0B00100000;

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_03, temp_buf) == true)
                {
                    return true;
                }
            }
            break;
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_OFF:
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_03, &temp_buf) == true)
            {
                temp_buf &= 0B11011111;

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_03, temp_buf) == true)
                {
                    return true;
                }
            }
            break;

        default:
            break;
        }

        break;
    case Arduino_IIC_Power::Device::POWER_DEVICE_CHARGING_MODE:
        switch (state)
        {
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_ON:
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_03, &temp_buf) == true)
            {
                temp_buf |= 0B00010000;

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_03, temp_buf) == true)
                {
                    return true;
                }
            }
            break;
        case Arduino_IIC_Power::Device_State::POWER_DEVICE_OFF:
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_03, &temp_buf) == true)
            {
                temp_buf &= 0B11101111;

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_03, temp_buf) == true)
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

bool Arduino_SY6970::IIC_Write_Device_Value(uint32_t device, uint32_t value)
{
    uint8_t temp_buf = 0;

    switch (device)
    {
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_CHARGING_TARGET_VOLTAGE_LIMIT: // 3840mV-4608mV(步进值：16mV)
        if (value >= 3840 && value <= 4608)
        {
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_06, &temp_buf) == true)
            {
                temp_buf &= 0B00000011; // 清空
                temp_buf = temp_buf | (((value - 3840) / 16) << 2);

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_06, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_MINIMUM_SYSTEM_VOLTAGE_LIMIT: // 3000mV-3700mV(步进值：100mV)
        if (value >= 3000 && value <= 3700)
        {
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_03, &temp_buf) == true)
            {
                temp_buf &= 0B11110001; // 清空
                temp_buf = temp_buf | (((value - 3000) / 100) << 1);

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_03, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_OTG_VOLTAGE_LIMIT: // 4550mV-5510mV(步进值：64mV)
        if (value >= 4550 && value <= 5510)
        {
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, &temp_buf) == true)
            {
                temp_buf &= 0B00001111; // 清空
                temp_buf = temp_buf | (((value - 4550) / 64) << 4);

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_INPUT_CURRENT_LIMIT: // 100mA-3250mA(步进值：50mA)
        if (value >= 100 && value <= 3250)
        {
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_00, &temp_buf) == true)
            {
                temp_buf &= 0B11000000; // 清空
                temp_buf = temp_buf | ((value - 100) / 50);

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_00, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_FAST_CHARGING_CURRENT_LIMIT: // 0mA-5056mA(步进值：64mA)
        if (value <= 5056)
        {
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_04, &temp_buf) == true)
            {
                temp_buf &= 0B10000000; // 清空
                temp_buf = temp_buf | (value / 64);

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_04, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_PRECHARGE_CHARGING_CURRENT_LIMIT: // 64mA-1024mA(步进值：64mA)
        if (value >= 64 && value <= 1024)
        {
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_05, &temp_buf) == true)
            {
                temp_buf &= 0B00001111; // 清空
                temp_buf = temp_buf | (((value - 64) / 64) << 4);

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_05, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_TERMINATION_CHARGING_CURRENT_LIMIT: // 64mA-1024mA(步进值：64mA)
        if (value >= 64 && value <= 1024)
        {
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_05, &temp_buf) == true)
            {
                temp_buf &= 0B11110000; // 清空
                temp_buf = temp_buf | ((value - 64) / 64);

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_05, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_WATCHDOG_TIMER: // 0s、40s、80s、160s
        if (value <= 160)
        {
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_07, &temp_buf) == true)
            {
                temp_buf &= 0B11001111; // 清空
                switch (value)
                {
                case 0:
                    // 上面已经清空了所以这里什么都不做
                    if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_07, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 40:
                    temp_buf = temp_buf | 0B00010000;

                    if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_07, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 80:
                    temp_buf = temp_buf | 0B00100000;

                    if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_07, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 160:
                    temp_buf = temp_buf | 0B00110000;

                    if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_07, temp_buf) == true)
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
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_THERMAL_REGULATION_THRESHOLD: // 60度-120度(步进值：20度)
        if (value >= 60 && value <= 120)
        {
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_08, &temp_buf) == true)
            {
                temp_buf &= 0B11111100; // 清空
                temp_buf = temp_buf | ((value - 60) / 20);

                if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_08, temp_buf) == true)
                {
                    return true;
                }
            }
        }
        break;
    case Arduino_IIC_Power::Device_Value::POWER_DEVICE_OTG_CHARGING_LIMIT: // 500mA、750mA、1200mA、1400mA、1650mA、1875mA、2150mA、2450mA
        if (value >= 500 && value <= 2450)
        {
            if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, &temp_buf) == true)
            {
                temp_buf &= 0B11111000; // 清空
                switch (value)
                {
                case 500:
                    // 上面已经清空了所以这里什么都不做
                    if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 750:
                    temp_buf = temp_buf | 0B11111001;

                    if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 1200:
                    temp_buf = temp_buf | 0B11111010;

                    if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 1400:
                    temp_buf = temp_buf | 0B11111011;

                    if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 1650:
                    temp_buf = temp_buf | 0B11111100;

                    if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 1875:
                    temp_buf = temp_buf | 0B11111101;

                    if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 2150:
                    temp_buf = temp_buf | 0B11111110;

                    if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, temp_buf) == true)
                    {
                        return true;
                    }
                    break;
                case 2450:
                    temp_buf = temp_buf | 0B11111111;

                    if (_bus->IIC_WriteC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, temp_buf) == true)
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

String Arduino_SY6970::IIC_Read_Device_State(uint32_t information)
{
    uint8_t temp_buf = 0;

    switch (information)
    {
    case Arduino_IIC_Power::Status_Information::POWER_BUS_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0B, &temp_buf) == true)
        {
            temp_buf &= 0B11100000;

            switch (temp_buf)
            {
            case 0B00000000:
                return "No Input";
                break;
            case 0B00100000:
                return "USB Host SDP";
                break;
            case 0B01000000:
                return "USB CDP";
                break;
            case 0B01100000:
                return "USB DCP";
                break;
            case 0B10000000:
                return "HVDCP";
                break;
            case 0B10100000:
                return "Unknown Adapter";
                break;
            case 0B11000000:
                return "Non-Standard Adapter";
                break;
            case 0B11100000:
                return "OTG";
                break;

            default:
                return "->Read POWER_INPUT_STATUS fail";
                break;
            }
        }

        return "->Read SY6970_RD_DEVICE_0B fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_BUS_CONNECTION_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_11, &temp_buf) == true)
        {
            temp_buf &= 0B10000000;

            switch (temp_buf)
            {
            case 0B00000000:
                return "No BUS Connection";
                break;
            case 0B10000000:
                return "BUS Connection";
                break;

            default:
                return "->Read POWER_BUS_CONNECTION_STATUS fail";
                break;
            }
        }

        return "->Read SY6970_RD_DEVICE_11 fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_CHARGING_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0B, &temp_buf) == true)
        {
            temp_buf &= 0B00011000;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Not Charging";
                break;
            case 0B00001000:
                return "Pre Chargeing";
                break;
            case 0B00010000:
                return "Fast Charging";
                break;
            case 0B00011000:
                return "Charge Termination Done";
                break;

            default:
                return "->Read POWER_CHARGING_STATUS fail";
                break;
            }
        }

        return "->Read SY6970_RD_DEVICE_0B fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_INPUT_SOURCE_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0B, &temp_buf) == true)
        {
            temp_buf &= 0B00000100;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Not Power Good";
                break;
            case 0B00000100:
                return "Power Good";
                break;

            default:
                return "->Read POWER_INPUT_GOOD_SOURCE fail";
                break;
            }
        }

        return "->Read SY6970_RD_DEVICE_0B fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_INPUT_USB_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0B, &temp_buf) == true)
        {
            temp_buf &= 0B00000010;

            switch (temp_buf)
            {
            case 0B00000000:
                return "USB100 Input Is Detected";
                break;
            case 0B00000010:
                return "USB500 Input Is Detected";
                break;

            default:
                return "->Read POWER_INPUT_USB_STATUS fail";
                break;
            }
        }

        return "->Read SY6970_RD_DEVICE_0B fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_SYSTEM_VOLTAGE_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0B, &temp_buf) == true)
        {
            temp_buf &= 0B00000001;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Not In VSYSMIN Regulation";
                break;
            case 0B00000001:
                return "In VSYSMIN Regulation";
                break;

            default:
                return "->Read POWER_SYSTEM_VOLTAGE_STATUS fail";
                break;
            }
        }

        return "->Read SY6970_RD_DEVICE_0B fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_THERMAL_REGULATION_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0E, &temp_buf) == true)
        {
            temp_buf &= 0B10000000;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Normal";
                break;
            case 0B10000000:
                return "In Thermal Regulation";
                break;

            default:
                return "->Read POWER_THERMAL_REGULATION_STATUS fail";
                break;
            }
        }

        return "->Read SY6970_RD_DEVICE_0E fail";
        break;

    case Arduino_IIC_Power::Status_Information::POWER_WATCHDOG_FAULT_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0C, &temp_buf) == true)
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

        return "->Read SY6970_RD_DEVICE_0C fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_OTG_FAULT_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0C, &temp_buf) == true)
        {
            temp_buf &= 0B01000000;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Normal";
                break;
            case 0B01000000:
                return "BUS Overloaded In OTG Or BUS OVP Or Battery Is Too Low";
                break;

            default:
                return "->Read POWER_BOOST_FAULT_STATUS fail";
                break;
            }
        }

        return "->Read SY6970_RD_DEVICE_0C fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_CHARGING_FAULT_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0C, &temp_buf) == true)
        {
            temp_buf &= 0B00110000;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Normal";
                break;
            case 0B00010000:
                return "Input Fault";
                break;
            case 0B00100000:
                return "Thermal Shutdown";
                break;
            case 0B00110000:
                return "Charge Safety Timer Expiration";
                break;

            default:
                return "->Read POWER_CHARGING_FAULT_STATUS fail";
                break;
            }
        }

        return "->Read SY6970_RD_DEVICE_0C fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_BATTERY_FAULT_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0C, &temp_buf) == true)
        {
            temp_buf &= 0B00001000;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Normal";
                break;
            case 0B00001000:
                return "BATOVP";
                break;

            default:
                return "->Read POWER_BATTERY_FAULT_STATUS fail";
                break;
            }
        }

        return "->Read SY6970_RD_DEVICE_0C fail";
        break;
    case Arduino_IIC_Power::Status_Information::POWER_NTC_FAULT_STATUS:
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0C, &temp_buf) == true)
        {
            temp_buf &= 0B00000111;

            switch (temp_buf)
            {
            case 0B00000000:
                return "Normal";
                break;
            case 0B00000010:
                return "NTC Warm";
                break;
            case 0B00000011:
                return "NTC Cool";
                break;
            case 0B00000101:
                return "NTC Cold";
                break;
            case 0B00000110:
                return "NTC Hot";
                break;

            default:
                return "->Read POWER_NTC_FAULT_STATUS fail";
                break;
            }
        }

        return "->Read SY6970_RD_DEVICE_0C fail";
        break;

    default:
        return "->No such information was found Read Power Status information fail ";
        break;
    }

    return "->Error reading IIC_Read_Information";
}

double Arduino_SY6970::IIC_Read_Device_Value(uint32_t information)
{
    uint8_t temp_buf = 0;

    switch (information)
    {
    case Arduino_IIC_Power::Value_Information::POWER_INPUT_VOLTAGE: // 2600mV-15300mV
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_11, &temp_buf) == true)
        {
            temp_buf &= 0B01111111;

            return int32_t(2600 + (temp_buf * 100));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_BATTERY_VOLTAGE: // 2304mV-4844mV
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0E, &temp_buf) == true)
        {
            temp_buf &= 0B01111111;

            return int32_t(2304 + (temp_buf * 20));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_SYSTEM_VOLTAGE: // 2304mV-4844mV
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_0F, &temp_buf) == true)
        {
            temp_buf &= 0B01111111;

            return int32_t(2304 + (temp_buf * 20));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_NTC_VOLTAGE_PERCENTAGE: // 21000-80055(需要除以1000单位才是%)
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_10, &temp_buf) == true)
        {
            temp_buf &= 0B01111111;

            return int32_t(21000 + (temp_buf * 465));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_CHARGING_CURRENT: // 0mA-6350mA
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_12, &temp_buf) == true)
        {
            temp_buf &= 0B01111111;

            return int32_t(temp_buf * 50);
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_THERMAL_REGULATION_THRESHOLD: // 60度、80度、100度、120度
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_08, &temp_buf) == true)
        {
            temp_buf &= 0B00000011;

            return int32_t(60 + (temp_buf * 20));
        }
        break;

    case Arduino_IIC_Power::Value_Information::POWER_CHARGING_TARGET_VOLTAGE_LIMIT: // 3840mV-4608mV
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_06, &temp_buf) == true)
        {
            temp_buf = (temp_buf & 0B11111100) >> 2;

            return int32_t(3840 + (temp_buf * 16));
        }
        break;

    case Arduino_IIC_Power::Value_Information::POWER_MINIMUM_SYSTEM_VOLTAGE_LIMIT: // 3000mV-3700mV
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_03, &temp_buf) == true)
        {
            temp_buf = (temp_buf & 0B00001110) >> 1;

            return int32_t(3000 + (temp_buf * 100));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_OTG_VOLTAGE_LIMIT: // 4550mV-5510mV
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, &temp_buf) == true)
        {
            temp_buf = (temp_buf & 0B11110000) >> 4;

            return int32_t(4550 + (temp_buf * 64));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_INPUT_CURRENT_LIMIT: // 100mA-3250mA
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_DEVICE_13, &temp_buf) == true)
        {
            temp_buf &= 0B00111111;

            return int32_t(100 + (temp_buf * 50));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_FAST_CHARGING_CURRENT_LIMIT: // 0mA-5056mA
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_04, &temp_buf) == true)
        {
            temp_buf &= 0B01111111;

            return int32_t(temp_buf * 64);
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_PRECHARGE_CHARGING_CURRENT_LIMIT: // 64mA-1024mA
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_05, &temp_buf) == true)
        {
            temp_buf = (temp_buf & 0B11110000) >> 4;

            return int32_t(64 + (temp_buf * 64));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_TERMINATION_CHARGING_CURRENT_LIMIT: // 64mA-1024mA
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_05, &temp_buf) == true)
        {
            temp_buf &= 0B00001111;

            return int32_t(64 + (temp_buf * 64));
        }
        break;
    case Arduino_IIC_Power::Value_Information::POWER_OTG_CURRENT_LIMIT: // 64mA-1024mA
        if (_bus->IIC_ReadC8D8(_device_address, SY6970_RD_WR_DEVICE_0A, &temp_buf) == true)
        {
            temp_buf &= 0B00000111;

            switch (temp_buf)
            {
            case 0B00000000:
                return 500;
                break;
            case 0B00000001:
                return 750;
                break;
            case 0B00000010:
                return 1200;
                break;
            case 0B00000011:
                return 1400;
                break;
            case 0B00000100:
                return 1650;
                break;
            case 0B00000101:
                return 1875;
                break;
            case 0B00000110:
                return 2150;
                break;
            case 0B00000111:
                return 2450;
                break;

            default:
                break;
            }
        }
        break;

    default:
        break;
    }
    return -1;
}
