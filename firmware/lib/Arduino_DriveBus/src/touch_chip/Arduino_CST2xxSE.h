/*
 * @Description(CN):
 *      基于Arduino_DriveBus库的CST2xxSE芯片库
 *      编写了主要功能 部分功能未列出
 *      以下是操作CST2xxSE的相关枚举（控制CST2xxSE芯片时请调用以下枚举的参数）：
 *
 *     enum Device
 *     {
 *         TOUCH_DEVICE_INTERRUPT_MODE, // 触摸中断模式
 *         TOUCH_DEVICE_SLEEP_MODE,     // 休眠模式
 *     };
 * 
 *     enum Status_Information
 *     {
 *         TOUCH_GESTURE_ID, // 触摸手势ID
 *     };
 *
 *    enum Value_Information
 *    {
 *         TOUCH_FINGER_NUMBER, // 触摸手指个数
 *         TOUCH_COORDINATE_X,  // 触摸坐标X 默认是第一个手指
 *         TOUCH_COORDINATE_Y,  // 触摸坐标Y 默认是第一个手指
 *
 *         TOUCH1_COORDINATE_X, // 触摸1坐标X
 *         TOUCH1_COORDINATE_Y, // 触摸1坐标Y
 *         TOUCH2_COORDINATE_X, // 触摸2坐标X
 *         TOUCH2_COORDINATE_Y, // 触摸2坐标Y
 *         TOUCH3_COORDINATE_X, // 触摸3坐标X
 *         TOUCH3_COORDINATE_Y, // 触摸3坐标Y
 *         TOUCH4_COORDINATE_X, // 触摸4坐标X
 *         TOUCH4_COORDINATE_Y, // 触摸4坐标Y
 *         TOUCH5_COORDINATE_X, // 触摸5坐标X
 *         TOUCH5_COORDINATE_Y, // 触摸5坐标Y
 * 
 *         TOUCH1_PRESSURE_VALUE, // 触摸1手指压力值
 *         TOUCH2_PRESSURE_VALUE, // 触摸2手指压力值
 *         TOUCH3_PRESSURE_VALUE, // 触摸3手指压力值
 *         TOUCH4_PRESSURE_VALUE, // 触摸4手指压力值
 *         TOUCH5_PRESSURE_VALUE, // 触摸5手指压力值
 *    };
 *
 *      注意事项：
 *      1. 默认启动的中断模式为检测到触摸时发出低脉冲
 *
 * @version: V1.0.0
 * @Author: Xk_w
 * @Date: 2023-11-25 09:14:23
 * @LastEditors: Xk_w
 * @LastEditTime: 2024-01-23 11:04:15
 * @License: GPL 3.0
 */
#pragma once

#include "../Arduino_IIC.h"

#define CST226SE_DEVICE_ADDRESS 0x5A

#define CST2xxSE_RD_DEVICE_FINGERNUM 0x05             // FingerNum
#define CST2xxSE_RD_DEVICE_X1POSH 0x01                // X1posH
#define CST2xxSE_RD_DEVICE_X1POSL 0x03                // X1posL
#define CST2xxSE_RD_DEVICE_Y1POSH 0x02                // Y1posH
#define CST2xxSE_RD_DEVICE_Y1POSL 0x03                // Y1posL
#define CST2xxSE_RD_DEVICE_X2POSH 0x08                // X2posH
#define CST2xxSE_RD_DEVICE_X2POSL 0x0A                // X2posL
#define CST2xxSE_RD_DEVICE_Y2POSH 0x09                // Y2posH
#define CST2xxSE_RD_DEVICE_Y2POSL 0x0A                // Y2posL
#define CST2xxSE_RD_DEVICE_X3POSH 0x0D                // X3posH
#define CST2xxSE_RD_DEVICE_X3POSL 0x0F                // X3posL
#define CST2xxSE_RD_DEVICE_Y3POSH 0x0E                // Y3posH
#define CST2xxSE_RD_DEVICE_Y3POSL 0x0F                // Y3posL
#define CST2xxSE_RD_DEVICE_X4POSH 0x12                // X4posH
#define CST2xxSE_RD_DEVICE_X4POSL 0x14                // X4posL
#define CST2xxSE_RD_DEVICE_Y4POSH 0x13                // Y4posH
#define CST2xxSE_RD_DEVICE_Y4POSL 0x14                // Y4posL
#define CST2xxSE_RD_DEVICE_X5POSH 0x17                // X5posH
#define CST2xxSE_RD_DEVICE_X5POSL 0x19                // X5posL
#define CST2xxSE_RD_DEVICE_Y5POSH 0x18                // Y5posH
#define CST2xxSE_RD_DEVICE_Y5POSL 0x19                // Y5posL
#define CST2xxSE_RD_DEVICE_TOUCH1_PRESSURE_VALUE 0x04 // 触摸1手指压力值
#define CST2xxSE_RD_DEVICE_TOUCH2_PRESSURE_VALUE 0x07 // 触摸2手指压力值
#define CST2xxSE_RD_DEVICE_TOUCH3_PRESSURE_VALUE 0x0C // 触摸3手指压力值
#define CST2xxSE_RD_DEVICE_TOUCH4_PRESSURE_VALUE 0x11 // 触摸4手指压力值
#define CST2xxSE_RD_DEVICE_TOUCH5_PRESSURE_VALUE 0x16 // 触摸5手指压力值
#define CST2xxSE_WR_DEVICE_SLEEPMODE 0xD1             // SleepMode
#define CST2xxSE_WR_DEVICE_SYSTEM_RESET 0xD1          // System Reset
#define CST2xxSE_RD_DEVICE_ID 0x06                    // Device ID Register 0xAB

static const uint8_t CST2xxSE_Initialization_BufferOperations[] = {
    BO_BEGIN_TRANSMISSION,

    BO_END_TRANSMISSION,

    BO_DELAY, 20};

class Arduino_CST2xxSE : public Arduino_IIC
{
public:
    Arduino_CST2xxSE(std::shared_ptr<Arduino_IIC_DriveBus> bus, uint8_t device_address,
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