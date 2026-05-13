/*
 * @Description(CN):
 *      基于Arduino_DriveBus库的ETA4662芯片库
 *      编写了主要功能 部分功能未列出
 *      以下是操作ETA4662的相关枚举（控制ETA4662芯片时请调用以下枚举的参数）：
 *
 *      enum Device_State
 *      {
 *          POWER_DEVICE_ON,  // 设备功能关
 *          POWER_DEVICE_OFF, // 设备功能开
 *      };
 *
 *      enum Device
 *      {
 *          POWER_DEVICE_CHARGING_MODE,        // 充电模式
 *          POWER_DEVICE_WATCHDOG_MODE,        // 看门狗模式
 *          POWER_DEVICE_NTC_MODE,             // NTC模式
 *          POWER_DEVICE_PCB_OTP_MODE,         // PCB过温保护模式
 *      };
 *
 *      enum Device_Value
 *      {
 *          POWER_DEVICE_MINIMUM_INPUT_VOLTAGE_LIMIT,        // 设置最小输入电压限制值
 *          POWER_DEVICE_CHARGING_TARGET_VOLTAGE_LIMIT,   // 设置充电目标电压限制值
 *          POWER_DEVICE_SYSTEM_VOLTAGE_LIMIT,                         // 设置系统电压限制值
 *          POWER_DEVICE_INPUT_CURRENT_LIMIT,                // 设置输入电流限制值
 *          POWER_DEVICE_FAST_CHARGING_CURRENT_LIMIT,        // 设置快速充电电流限制值
 *          POWER_DEVICE_TERMINATION_PRECHARGE_CHARGING_CURRENT_LIMIT, // 设置终止充电和预充电电流限制值
 *          POWER_DEVICE_BAT_TO_SYS_DISCHARGE_CURRENT_LIMIT  // BAT到SYS的放电电流限制值
 *
 *          POWER_DEVICE_WATCHDOG_TIMER,               // 设置看门狗定时器值
 *          POWER_DEVICE_THERMAL_REGULATION_THRESHOLD, // 设置热调节阈值值
 *          POWER_DEVICE_BATTERY_UVLO_THRESHOLD        // 设置电池低电压锁定阈值
 *      };
 *
 *      enum Status_Information
 *      {
 *          POWER_CHARGING_STATUS,           // 充电状态
 *          POWER_INPUT_SOURCE_STATUS,       // 输入源的状态
 *          POWER_SYSTEM_VOLTAGE_STATUS,     // 系统电压状态
 *          POWER_THERMAL_REGULATION_STATUS, // 热调节状态
 *
 *          POWER_WATCHDOG_FAULT_STATUS, // 看门狗故障状态（故障后自动重置限制类的功能值）
 *          POWER_INPUT_FAULT_STATUS,      // 输入故障状态
 *          POWER_THERMAL_SHUTDOWN_FAULT_STATUS, // 热关闭故障状态
 *          POWER_BATTERY_FAULT_STATUS,          // 电池故障状态
 *          POWER_SAFETY_TIMER_STATUS_FAULT_STATUS, // 安全定时器故障状态
 *          POWER_NTC_FAULT_STATUS,                 // NTC故障状态
 * };
 *
 *      enum Value_Information
 *      {
 *          POWER_THERMAL_REGULATION_THRESHOLD, // 热调节阈值值
 *
 *          POWER_MINIMUM_INPUT_VOLTAGE_LIMIT,       // 最小输入电压限制值
 *          POWER_CHARGING_TARGET_VOLTAGE_LIMIT,  // 充电目标电压限制值
 *          POWER_SYSTEM_VOLTAGE_LIMIT,                         // 系统电压限制值
 *          POWER_INPUT_CURRENT_LIMIT,                // 输入电流限制值
 *          POWER_FAST_CHARGING_CURRENT_LIMIT,        // 快速充电电流限制
 *          POWER_TERMINATION_PRECHARGE_CHARGING_CURRENT_LIMIT, // 终止充电和预充电电流限制
 *          POWER_BAT_TO_SYS_DISCHARGE_CURRENT_LIMIT, // BAT到SYS的放电电流限制
 *      };
 *
 *      注意事项：
 *      1. 当启动ETA4662的看门狗时，看门狗的定时器到达指定值后将断开电源重新连接，
 *  与ETA4662通信的MCU将失去电源重启
 *      2. 当ETA4662开启充电，但是又未接入电池，此时ETA4662会将输出电压断开一次，
 *  与ETA4662通信的MCU将失去电源重启，所以如果没有接入电池时请不要开启ETA4662的充电
 * 
 * @version: V1.0.0
 * @Author: Xk_w
 * @Date: 2023-11-16 15:42:22
 * @LastEditors: Xk_w
 * @LastEditTime: 2023-12-09 10:16:05
 * @License: GPL 3.0
 */
#pragma once

#include "../Arduino_IIC.h"

#define ETA4662_DEVICE_ADDRESS 0x07

#define ETA4662_RD_WR_INPUT_SOURCE_CONTROL 0x00                    // Input Source Control Register
#define ETA4662_RD_WR_POWER_ON_CONFIGURATION 0x01                  // Power on configuration register
#define ETA4662_RD_WR_CHARGE_CURRENT_CONTROL 0x02                  // Charge Current Control Register
#define ETA4662_RD_WR_DISCHARGE_TERMINATION_CURRENT 0x03           // Dis-charge/ Termination Current 1001 0001
#define ETA4662_RD_WR_CHARGE_VOLTAGE_CONTROL 0x04                  // Charge Voltage Control Register
#define ETA4662_RD_WR_CHARGE_TERMINATION_TIMER_CONTROL 0x05        // Charge Termination/Timer Control Register 0111 1010
#define ETA4662_RD_WR_MISCELLANEOUS_OPERATION_CONTROL 0x06         // Miscellaneous Operation Control Register 1100 0000
#define ETA4662_RD_WR_SYSTEM_VOLTAGE_REGULATION 0x07               // System Voltage Regulation Register
#define ETA4662_RD_SYSTEM_STATUS 0x08                              // System Status Register
#define ETA4662_RD_FAULT 0x09                                      // Fault Register
#define ETA4662_RD_WR_IIC_ADDRESS_MISCELLANEOUS_CONFIGURATION 0x0A // IIC Address and Miscellaneous Configuration Register
#define ETA4662_RD_DEVICE_ID 0x0B                                  // Device ID Register

static const uint8_t ETA4662_Initialization_BufferOperations[] = {
    // BO_BEGIN_TRANSMISSION,
    // BO_WRITE_C8_D8, ETA4662_RD_WR_POWER_ON_CONFIGURATION, 0B10100100, // 开启电池充电功能
    // BO_END_TRANSMISSION,

    BO_BEGIN_TRANSMISSION,
    BO_WRITE_C8_D8, ETA4662_RD_WR_CHARGE_TERMINATION_TIMER_CONTROL, 0B00011010, // 关闭看门狗功能
    BO_END_TRANSMISSION,

    BO_DELAY, 100};

class Arduino_ETA4662 : public Arduino_IIC
{
public:
    Arduino_ETA4662(std::shared_ptr<Arduino_IIC_DriveBus> bus, uint8_t device_address,
                    int8_t rst = DRIVEBUS_DEFAULT_VALUE, int8_t iqr = DRIVEBUS_DEFAULT_VALUE);

    bool begin(int32_t speed = DRIVEBUS_DEFAULT_VALUE) override;
    int32_t IIC_Read_Device_ID(void) override;
    bool IIC_Write_Device_State(uint32_t device, uint8_t state) override;
    bool IIC_Write_Device_Value(uint32_t device, uint32_t value) override;
    String IIC_Read_Device_State(uint32_t information) override;
    double IIC_Read_Device_Value(uint32_t information) override;

protected:
    bool IIC_Initialization(void) override;
};