/*
 * @Description(CN):
 *      属于Arduino_IIC的一个分支 用于存储IIC相关的Power芯片数据
 *
 * @Description(EN):
 *      A sub-branch of Arduino_IIC, used for storing IIC-related Power chip data
 *
 * @version: V1.1.5
 * @Author: Xk_w
 * @Date: 2023-11-16 16:58:05
 * @LastEditors: Xk_w
 * @LastEditTime: 2024-02-28 14:45:20
 * @License: GPL 3.0
 */
#pragma once

#include <Arduino.h>

class Arduino_IIC_Power
{
public:
    enum Device_State
    {
        POWER_DEVICE_ON,  // 设备功能关
        POWER_DEVICE_OFF, // 设备功能开
    };

    enum Device
    {
        POWER_DEVICE_ADC_MEASURE,          // ADC测量功能
        POWER_DEVICE_HIZ_MODE,             // 高阻态模式（内部总线输出切换为只能电池输出）
        POWER_DEVICE_WATCHDOG_TIMER_RESET, // 看门狗定时器重置（喂狗）
        POWER_DEVICE_OTG_MODE,             // OTG模式
        POWER_DEVICE_CHARGING_MODE,        // 充电模式
        POWER_DEVICE_WATCHDOG_MODE,        // 看门狗模式
        POWER_DEVICE_NTC_MODE,             // 电池NTC热检测模式
        POWER_DEVICE_PCB_OTP_MODE,         // PCB过温保护模式
    };

    enum Device_Value
    {
        POWER_DEVICE_MINIMUM_INPUT_VOLTAGE_LIMIT,                  // 设置最小输入电压限制值
        POWER_DEVICE_CHARGING_TARGET_VOLTAGE_LIMIT,                // 设置充电目标电压限制值
        POWER_DEVICE_MINIMUM_SYSTEM_VOLTAGE_LIMIT,                 // 设置最小系统电压限制值
        POWER_DEVICE_SYSTEM_VOLTAGE_LIMIT,                         // 设置系统电压限制值
        POWER_DEVICE_OTG_VOLTAGE_LIMIT,                            // 设置OTG电压输出值
        POWER_DEVICE_INPUT_CURRENT_LIMIT,                          // 设置输入电流限制值
        POWER_DEVICE_FAST_CHARGING_CURRENT_LIMIT,                  // 设置快速充电电流限制值
        POWER_DEVICE_PRECHARGE_CHARGING_CURRENT_LIMIT,             // 设置预充电电流限制值
        POWER_DEVICE_TERMINATION_CHARGING_CURRENT_LIMIT,           // 设置终止充电电流限制值
        POWER_DEVICE_TERMINATION_PRECHARGE_CHARGING_CURRENT_LIMIT, // 设置终止充电和预充电电流限制值
        POWER_DEVICE_OTG_CHARGING_LIMIT,                           // 设置OTG电流输出值
        POWER_DEVICE_BAT_TO_SYS_DISCHARGE_CURRENT_LIMIT,           // BAT到SYS的放电电流限制值

        POWER_DEVICE_WATCHDOG_TIMER,               // 设置看门狗定时器值
        POWER_DEVICE_THERMAL_REGULATION_THRESHOLD, // 设置热调节阈值值
        POWER_DEVICE_BATTERY_UVLO_THRESHOLD        // 设置电池低电压锁定阈值
    };

    enum Status_Information
    {
        POWER_BUS_STATUS,                // 总线状态
        POWER_BUS_CONNECTION_STATUS,     // 总线连接状态
        POWER_CHARGING_STATUS,           // 充电状态
        POWER_INPUT_SOURCE_STATUS,       // 输入源的状态
        POWER_INPUT_USB_STATUS,          // 输入USB状态
        POWER_SYSTEM_VOLTAGE_STATUS,     // 系统电压状态
        POWER_THERMAL_REGULATION_STATUS, // 热调节状态

        POWER_WATCHDOG_FAULT_STATUS,            // 看门狗故障状态（故障后自动重置限制类的功能值）
        POWER_INPUT_FAULT_STATUS,               // 输入故障状态
        POWER_THERMAL_SHUTDOWN_FAULT_STATUS,    // 热关闭故障状态
        POWER_OTG_FAULT_STATUS,                 // 升压OTG模式故障状态
        POWER_CHARGING_FAULT_STATUS,            // 充电故障状态
        POWER_BATTERY_FAULT_STATUS,             // 电池故障状态
        POWER_SAFETY_TIMER_STATUS_FAULT_STATUS, // 安全定时器故障状态
        POWER_NTC_FAULT_STATUS,                 // NTC故障状态
    };

    enum Value_Information
    {
        POWER_INPUT_VOLTAGE,                // 输入电压值
        POWER_BATTERY_VOLTAGE,              // 电池电压值
        POWER_SYSTEM_VOLTAGE,               // 系统电压值
        POWER_NTC_VOLTAGE_PERCENTAGE,       // NTC电压百分比值
        POWER_CHARGING_CURRENT,             // 充电电流值
        POWER_THERMAL_REGULATION_THRESHOLD, // 热调节阈值值

        POWER_MINIMUM_INPUT_VOLTAGE_LIMIT,                  // 最小输入电压限制值
        POWER_CHARGING_TARGET_VOLTAGE_LIMIT,                // 充电目标电压限制值
        POWER_MINIMUM_SYSTEM_VOLTAGE_LIMIT,                 // 最小系统电压限制值
        POWER_SYSTEM_VOLTAGE_LIMIT,                         // 系统电压限制值
        POWER_OTG_VOLTAGE_LIMIT,                            // OTG模式电压限制
        POWER_INPUT_CURRENT_LIMIT,                          // 输入电流限制值
        POWER_FAST_CHARGING_CURRENT_LIMIT,                  // 快速充电电流限制
        POWER_PRECHARGE_CHARGING_CURRENT_LIMIT,             // 预充电电流限制
        POWER_TERMINATION_CHARGING_CURRENT_LIMIT,           // 终止充电电流限制
        POWER_TERMINATION_PRECHARGE_CHARGING_CURRENT_LIMIT, // 终止充电和预充电电流限制
        POWER_OTG_CURRENT_LIMIT,                            // OTG模式电流限制
        POWER_BAT_TO_SYS_DISCHARGE_CURRENT_LIMIT,           // BAT到SYS的放电电流限制
    };

protected:
};

class Arduino_IIC_Touch
{
public:
    enum Device_State
    {
        TOUCH_DEVICE_ON,  // 设备功能关
        TOUCH_DEVICE_OFF, // 设备功能开
    };
    enum Device_Mode
    {
        // 触摸中断模式
        TOUCH_DEVICE_INTERRUPT_TEST,     //  中断引脚测试，使能后自动周期性发出低脉冲
        TOUCH_DEVICE_INTERRUPT_PERIODIC, // 检测到触摸时，发出低脉冲
        TOUCH_DEVICE_INTERRUPT_CHANGE,   // 检测到触摸状态变化时，发出低脉冲
        TOUCH_DEVICE_INTERRUPT_MOTION,   //  检测到手势时，发出低脉冲
        TOUCH_DEVICE_INTERRUPT_ONCEWLP,  //  长按手势只发出一个低脉冲信号

        // 触摸功耗模式
        TOUCH_POWER_ACTIVE,
        TOUCH_POWER_MONITOR,
        TOUCH_POWER_STANDBY,
        TOUCH_POWER_HIBERNATE,
    };
    enum Device
    {
        TOUCH_DEVICE_INTERRUPT_MODE, // 触摸中断模式
        TOUCH_DEVICE_SLEEP_MODE,     // 休眠模式

        TOUCH_POWER_MODE,             // 触摸芯片工作功耗模式
        TOUCH_PROXIMITY_SENSING_MODE, // 进距离感应模式
        TOUCH_GESTUREID_MODE,         // 特殊手势模式
    };
    enum Device_Value
    {
    };
    enum Status_Information
    {
        TOUCH_GESTURE_ID, // 触摸手势ID
    };
    enum Value_Information
    {
        TOUCH_FINGER_NUMBER, // 触摸手指个数
        TOUCH_COORDINATE_X,  // 触摸坐标X 默认是第一个手指
        TOUCH_COORDINATE_Y,  // 触摸坐标Y 默认是第一个手指

        TOUCH1_COORDINATE_X, // 触摸1坐标X
        TOUCH1_COORDINATE_Y, // 触摸1坐标Y
        TOUCH2_COORDINATE_X, // 触摸2坐标X
        TOUCH2_COORDINATE_Y, // 触摸2坐标Y
        TOUCH3_COORDINATE_X, // 触摸3坐标X
        TOUCH3_COORDINATE_Y, // 触摸3坐标Y
        TOUCH4_COORDINATE_X, // 触摸4坐标X
        TOUCH4_COORDINATE_Y, // 触摸4坐标Y
        TOUCH5_COORDINATE_X, // 触摸5坐标X
        TOUCH5_COORDINATE_Y, // 触摸5坐标Y

        TOUCH1_PRESSURE_VALUE, // 触摸1手指压力值
        TOUCH2_PRESSURE_VALUE, // 触摸2手指压力值
        TOUCH3_PRESSURE_VALUE, // 触摸3手指压力值
        TOUCH4_PRESSURE_VALUE, // 触摸4手指压力值
        TOUCH5_PRESSURE_VALUE, // 触摸5手指压力值
    };
};

class Arduino_IIC_IMU
{
public:
    enum Device_State
    {
    };
    enum Device
    {
        IMU_ACCELERATION_POWER_MODE, // 加速度传感器电源功耗模式
        IMU_GYROSCOPE_POWER_MODE,    // 陀螺仪传感器电源功耗模式

        IMU_FIFO_POWER_MODE, // 传感器FIFO的电源功耗模式
        IMU_FIFO_WORK_MODE,  // 传感器FIFO的工作模式

        IMU_FIFO_DATA_TYPE, // 传感器FIFO选择需要获取的数据类型
    };
    enum Device_Mode
    {
        // 传感器设备功耗模式
        IMU_DEVICE_OFF,              // 下电模式
        IMU_DEVICE_ULTRA_LOW_POWER,  // 超低功耗模式
        IMU_DEVICE_LOW_POWER,        // 低功耗模式
        IMU_DEVICE_NORMAL_POWER,     // 正常模式
        IMU_DEVICE_HIGH_POWER,       // 高性能模式
        IMU_DEVICE_ULTRA_HIGH_POWER, // 超高性能模式

        // 传感器FIFO模式
        IMU_FIFO_BYPASS,             // Bypass模式 FIFO禁用
        IMU_FIFO_NORMAL,             // 正常FIFO模式 当FIFO满时停止采集数据
        IMU_FIFO_CONTINUE,           // Continue模式 如果FIFO已满则新的采样将会覆盖原有值
        IMU_FIFO_CONTINUE_TO_NORMAL, // 直至触发条件满足前均为Continue模式 条件满足后进入正常FIFO模式
        IMU_FIFO_BYPASS_TO_CONTINUE, // 直至触发条件满足前均为Bypass模式 条件满足后进入Continue模式

        IMU_FIFO_DATA_ACCELERATION_GYROSCOPE, // 加速度传感器以及陀螺仪传感器的数据
        IMU_FIFO_DATA_PEDOMETER,              // 计步器的数据
        IMU_FIFO_DATA_SENSOR_HUB,             // 传感器集合的数据

    };
    enum Device_Value
    {
        IMU_ACCELERATION_SENSITIVITY, // 设置加速度传感器量程范围
        IMU_GYROSCOPE_SENSITIVITY,    // 设置陀螺仪传感器量程范围

        IMU_ACCELERATION_CORRECTION, // 加速度传感器校正
        IMU_GYROSCOPE_CORRECTION,    // 陀螺仪传感器校正

        IMU_FIFO_ACCELERATION_FACTOR, // 加速度传感器FIFO的抽取因子
        IMU_FIFO_GYROSCOPE_FACTOR,    // 加速度传感器FIFO的抽取因子
    };
    enum Status_Information
    {
    };
    enum Value_Information
    {
        IMU_TEMPERATURE_VALUE, // 传感器温度值

        IMU_ACCELERATION_X, // 加速度传感器X方向值 原始值
        IMU_ACCELERATION_Y, // 加速度传感器Y方向值 原始值
        IMU_ACCELERATION_Z, // 加速度传感器Z方向值 原始值

        IMU_GYROSCOPE_X, // 陀螺仪传感器X方向值 原始值
        IMU_GYROSCOPE_Y, // 陀螺仪传感器Y方向值 原始值
        IMU_GYROSCOPE_Z, // 陀螺仪传感器Z方向值 原始值

        IMU_ACCELERATION_X_SIGNED, // 加速度传感器X方向值 带符号
        IMU_ACCELERATION_Y_SIGNED, // 加速度传感器Y方向值 带符号
        IMU_ACCELERATION_Z_SIGNED, // 加速度传感器Z方向值 带符号

        IMU_GYROSCOPE_X_SIGNED, // 陀螺仪传感器X方向值 带符号
        IMU_GYROSCOPE_Y_SIGNED, // 陀螺仪传感器Y方向值 带符号
        IMU_GYROSCOPE_Z_SIGNED, // 陀螺仪传感器Z方向值 带符号

        IMU_FIFO_DATA_PATTERN, // 用于判断下次读取时读取哪个传感器哪个轴上的数据
        IMU_FIFO_DATA,         // FIFO 上的数据 原始值
    };
};