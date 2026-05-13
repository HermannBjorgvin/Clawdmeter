/*
 * @Description(CN):
 *      基于Arduino_DriveBus库的SY6970芯片库
 *      编写了主要功能 部分功能未列出
 *      以下是操作SY6970的相关枚举（控制SY6970芯片时请调用以下枚举的参数）：
 *
 *      enum Device_State
 *      {
 *          POWER_DEVICE_ON,  // 设备功能关
 *          POWER_DEVICE_OFF, // 设备功能开
 *      };
 *
 *      enum Device
 *      {
 *          POWER_DEVICE_ADC_MEASURE,          // ADC测量功能
 *          POWER_DEVICE_HIZ_MODE,             // 高阻态模式（内部总线输出切换为只能电池输出）
 *          POWER_DEVICE_WATCHDOG_TIMER_RESET, // 看门狗定时器重置（喂狗）
 *          POWER_DEVICE_OTG_MODE,             // OTG模式
 *          POWER_DEVICE_CHARGING_MODE,        // 充电模式
 *      };
 *
 *      enum Device_Value
 *      {
 *          POWER_DEVICE_CHARGING_TARGET_VOLTAGE_LIMIT,      // 设置充电目标电压限制值
 *          POWER_DEVICE_MINIMUM_SYSTEM_VOLTAGE_LIMIT,       // 设置最小系统电压限制值
 *          POWER_DEVICE_OTG_VOLTAGE_LIMIT,                  // 设置OTG电压输出值
 *          POWER_DEVICE_INPUT_CURRENT_LIMIT,                // 设置输入电流限制值
 *          POWER_DEVICE_FAST_CHARGING_CURRENT_LIMIT,        // 设置快速充电电流限制值
 *          POWER_DEVICE_PRECHARGE_CHARGING_CURRENT_LIMIT,   // 设置预充电电流限制值
 *          POWER_DEVICE_TERMINATION_CHARGING_CURRENT_LIMIT, // 设置终止充电电流限制值
 *          POWER_DEVICE_OTG_CHARGING_LIMIT,                 // 设置OTG电流输出值
 *
 *          POWER_DEVICE_WATCHDOG_TIMER,               // 设置看门狗定时器值
 *          POWER_DEVICE_THERMAL_REGULATION_THRESHOLD, // 设置热调节阈值值
 *      };
 *
 *      enum Status_Information
 *      {
 *          POWER_BUS_STATUS,                // 总线状态
 *          POWER_BUS_CONNECTION_STATUS,     // 总线连接状态
 *          POWER_CHARGING_STATUS,           // 充电状态
 *          POWER_INPUT_SOURCE_STATUS,       // 输入源的状态
 *          POWER_INPUT_USB_STATUS,          // 输入USB状态
 *          POWER_SYSTEM_VOLTAGE_STATUS,     // 系统电压状态
 *          POWER_THERMAL_REGULATION_STATUS, // 热调节状态
 *
 *          POWER_WATCHDOG_FAULT_STATUS, // 看门狗故障状态（故障后自动重置限制类的功能值）
 *          POWER_OTG_FAULT_STATUS,      // 升压OTG模式故障状态
 *          POWER_CHARGING_FAULT_STATUS, // 充电故障状态
 *          POWER_BATTERY_FAULT_STATUS,  // 电池故障状态
 *          POWER_NTC_FAULT_STATUS,      // NTC故障状态
 *      };
 *
 *      enum Value_Information
 *      {
 *          POWER_INPUT_VOLTAGE,          // 输入电压值
 *          POWER_BATTERY_VOLTAGE,        // 电池电压值
 *          POWER_SYSTEM_VOLTAGE,         // 系统电压值
 *          POWER_NTC_VOLTAGE_PERCENTAGE, // NTC电压百分比值
 *          POWER_CHARGING_CURRENT,       // 充电电流值
 *          POWER_THERMAL_REGULATION_THRESHOLD, // 热调节阈值值
 *
 *          POWER_CHARGING_TARGET_VOLTAGE_LIMIT,      // 充电目标电压限制值
 *          POWER_MINIMUM_SYSTEM_VOLTAGE_LIMIT,       // 最小系统电压限制值
 *          POWER_OTG_VOLTAGE_LIMIT,                  // OTG模式电压限制
 *          POWER_INPUT_CURRENT_LIMIT,                // 输入电流限制值
 *          POWER_FAST_CHARGING_CURRENT_LIMIT,        // 快速充电电流限制
 *          POWER_PRECHARGE_CHARGING_CURRENT_LIMIT,   // 预充电电流限制
 *          POWER_TERMINATION_CHARGING_CURRENT_LIMIT, // 终止充电电流限制
 *          POWER_OTG_CURRENT_LIMIT,                  // OTG模式电流限制
 *      };
 *
 * @version: V1.0.0
 * @Author: Xk_w
 * @Date: 2023-11-16 15:42:22
 * @LastEditors: Xk_w
 * @LastEditTime: 2023-11-22 11:44:29
 * @License: GPL 3.0
 */
#pragma once

#include "../Arduino_IIC.h"

#define SY6970_DEVICE_ADDRESS 0x6A

#define SY6970_RD_WR_DEVICE_00 0x00
#define SY6970_RD_WR_DEVICE_02 0x02
#define SY6970_RD_WR_DEVICE_03 0x03
#define SY6970_RD_WR_DEVICE_04 0x04
#define SY6970_RD_WR_DEVICE_05 0x05
#define SY6970_RD_WR_DEVICE_06 0x06
#define SY6970_RD_WR_DEVICE_07 0x07
#define SY6970_RD_WR_DEVICE_08 0x08
#define SY6970_RD_WR_DEVICE_0A 0x0A
#define SY6970_RD_DEVICE_0B 0x0B
#define SY6970_RD_DEVICE_0C 0x0C
#define SY6970_RD_DEVICE_0E 0x0E
#define SY6970_RD_DEVICE_0F 0x0F
#define SY6970_RD_DEVICE_10 0x10
#define SY6970_RD_DEVICE_11 0x11
#define SY6970_RD_DEVICE_12 0x12
#define SY6970_RD_DEVICE_13 0x13
#define SY6970_RD_DEVICE_14 0x14 // Device Register

static const uint8_t SY6970_Initialization_BufferOperations[] = {
    BO_BEGIN_TRANSMISSION,
    BO_WRITE_C8_D8, SY6970_RD_WR_DEVICE_02, 0B11011101, // 开启ADC测量功能
    BO_END_TRANSMISSION,
    BO_DELAY, 10,

    BO_BEGIN_TRANSMISSION,
    BO_WRITE_C8_D8, SY6970_RD_WR_DEVICE_07, 0B10001101, // 禁用看门狗定时喂狗功能
    BO_END_TRANSMISSION,

    BO_DELAY, 100};

class Arduino_SY6970 : public Arduino_IIC
{
public:
    Arduino_SY6970(std::shared_ptr<Arduino_IIC_DriveBus> bus, uint8_t device_address,
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