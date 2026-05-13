/*
 * @Description(CN):
 *      基于Arduino_DriveBus库的CST816x芯片库
 *      编写了主要功能 部分功能未列出
 *      以下是操作CST816x的相关枚举（控制CST816x芯片时请调用以下枚举的参数）：
 *
 *        enum Device_State
 *     {
 *         TOUCH_DEVICE_ON,  // 设备功能关
 *         TOUCH_DEVICE_OFF, // 设备功能开
 *     };
 *     enum Device_Mode
 *     {
 *         // 触摸中断模式
 *         TOUCH_DEVICE_INTERRUPT_TEST,     //  中断引脚测试，使能后自动周期性发出低脉冲
 *         TOUCH_DEVICE_INTERRUPT_PERIODIC, // 检测到触摸时，发出低脉冲
 *         TOUCH_DEVICE_INTERRUPT_CHANGE,   // 检测到触摸状态变化时，发出低脉冲
 *         TOUCH_DEVICE_INTERRUPT_MOTION,   //  检测到手势时，发出低脉冲
 *         TOUCH_DEVICE_INTERRUPT_ONCEWLP,  //  长按手势只发出一个低脉冲信号
 *     };
 *     enum Device
 *     {
 *         TOUCH_DEVICE_INTERRUPT_MODE, // 触摸中断模式
 *         TOUCH_DEVICE_SLEEP_MODE,     // 休眠模式
 *     };
 *     enum Device_Value
 *     {
 *
 *     };
 *     enum Status_Information
 *     {
 *         TOUCH_GESTURE_ID, // 触摸手势ID
 *     };
 *     enum Value_Information
 *     {
 *         TOUCH_FINGER_NUMBER, // 触摸手指个数
 *         TOUCH_COORDINATE_X,  // 触摸坐标X
 *         TOUCH_COORDINATE_Y,  // 触摸坐标Y
 *     };
 *
 *      注意事项：
 *      1. 默认启动的中断模式为检测到手势时发出低脉冲
 *
 * @version: V1.0.1
 * @Author: Xk_w
 * @Date: 2023-11-25 09:14:23
 * @LastEditors: Xk_w
 * @LastEditTime: 2023-12-08 14:20:09
 * @License: GPL 3.0
 */
#pragma once

#include "../Arduino_IIC.h"

#define CST816D_DEVICE_ADDRESS 0x15
#define CST816T_DEVICE_ADDRESS 0x15

#define CST816x_RD_DEVICE_GESTUREID 0x01      // GestureID
#define CST816x_RD_DEVICE_FINGERNUM 0x02      // FingerNum
#define CST816x_RD_DEVICE_XPOSH 0x03          // XposH
#define CST816x_RD_DEVICE_XPOSL 0x04          // XposL
#define CST816x_RD_DEVICE_YPOSH 0x05          // YposH
#define CST816x_RD_DEVICE_YPOSL 0x06          // YposL
#define CST816x_WR_DEVICE_SLEEPMODE 0xE5      // SleepMode
#define CST816x_WR_DEVICE_INTERRUPT_MODE 0xFA // Interrupt Mode
#define CST816x_RD_DEVICE_ID 0xA7             // Device ID Register

static const uint8_t CST816x_Initialization_BufferOperations[] = {
    BO_BEGIN_TRANSMISSION,
    BO_WRITE_C8_D8, CST816x_WR_DEVICE_INTERRUPT_MODE, 0B00010000, // 中断配置为检测到手势时发出低脉冲
    BO_END_TRANSMISSION,

    BO_DELAY, 20};

class Arduino_CST816x : public Arduino_IIC
{
public:
    Arduino_CST816x(std::shared_ptr<Arduino_IIC_DriveBus> bus, uint8_t device_address,
                    int8_t rst = DRIVEBUS_DEFAULT_VALUE, int8_t iqr = DRIVEBUS_DEFAULT_VALUE,
                    void (*Interrupt_Function)() = nullptr);

    bool begin(int32_t speed = DRIVEBUS_DEFAULT_VALUE) override;
    int32_t IIC_Read_Device_ID(void) override;
    bool IIC_Write_Device_State(uint32_t device, uint8_t state) override;

    String IIC_Read_Device_State(uint32_t information) override;
    double IIC_Read_Device_Value(uint32_t information) override;

protected:
    bool IIC_Initialization(void) override;
};