/*
 * @Description(CN):
 *      基于Arduino_DriveBus库的FT3x68芯片库
 *      编写了主要功能 部分功能未列出
 *      以下是操作FT3x68的相关枚举（控制FT3x68芯片时请调用以下枚举的参数）：
 *
 *        enum Device_State
 *        {
 *            TOUCH_DEVICE_ON,  // 设备功能关
 *            TOUCH_DEVICE_OFF, // 设备功能开
 *        };
 *        enum Device_Mode
 *        {
 *            // 触摸功耗模式
 *            TOUCH_POWER_ACTIVE,
 *            TOUCH_POWER_MONITOR,
 *            TOUCH_POWER_STANDBY,
 *            TOUCH_POWER_HIBERNATE,
 *        };
 *        enum Device
 *        {
 *            TOUCH_POWER_MODE,             // 触摸芯片工作功耗模式
 *            TOUCH_PROXIMITY_SENSING_MODE, // 进距离感应模式
 *            TOUCH_GESTUREID_MODE,         // 特殊手势模式
 *        };
 *        enum Status_Information
 *        {
 *            TOUCH_GESTURE_ID, // 触摸手势ID
 *        };
 *        enum Value_Information
 *        {
 *            TOUCH_FINGER_NUMBER, // 触摸手指个数
 *            TOUCH_COORDINATE_X,  // 触摸坐标X 默认是第一个手指
 *            TOUCH_COORDINATE_Y,  // 触摸坐标Y 默认是第一个手指
 *
 *            TOUCH1_COORDINATE_X, // 触摸1坐标X
 *            TOUCH1_COORDINATE_Y, // 触摸1坐标Y
 *            TOUCH2_COORDINATE_X, // 触摸2坐标X
 *            TOUCH2_COORDINATE_Y, // 触摸2坐标Y
 *        };
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

#define FT3168_DEVICE_ADDRESS 0x38
#define FT3268_DEVICE_ADDRESS 0x38

#define FT3x68_RD_DEVICE_GESTUREID 0xD3                 // GestureID
#define FT3x68_RD_DEVICE_FINGERNUM 0x02                 // FingerNum
#define FT3x68_RD_DEVICE_X1POSH 0x03                    // X1posH
#define FT3x68_RD_DEVICE_X1POSL 0x04                    // X1posL
#define FT3x68_RD_DEVICE_Y1POSH 0x05                    // Y1posH
#define FT3x68_RD_DEVICE_Y1POSL 0x06                    // Y1posL
#define FT3x68_RD_DEVICE_X2POSH 0x09                    // X2posH
#define FT3x68_RD_DEVICE_X2POSL 0x0A                    // X2posL
#define FT3x68_RD_DEVICE_Y2POSH 0x0B                    // Y2posH
#define FT3x68_RD_DEVICE_Y2POSL 0x0C                    // Y2posL
#define FT3x68_RD_WR_DEVICE_GESTUREID_MODE 0xD0         // GestureID
#define FT3x68_RD_WR_DEVICE_POWER_MODE 0xA5             // Power Mode
#define FT3x68_RD_WR_DEVICE_PROXIMITY_SENSING_MODE 0xB0 // Proximity Sensing Mode
#define FT3x68_RD_DEVICE_ID 0xA0                        // Device ID Register (0x00:FT6456 0x04:FT3268 0x01:FT3067 0x05:FT3368 0x02:FT3068 0x03:FT3168)

static const uint8_t FT3x68_Initialization_BufferOperations[] = {
    BO_BEGIN_TRANSMISSION,
    BO_WRITE_C8_D8, FT3x68_RD_WR_DEVICE_POWER_MODE, 0B00000001, // 功耗模式选择监听触发模式
    BO_END_TRANSMISSION,

    BO_DELAY, 20};

class Arduino_FT3x68 : public Arduino_IIC
{
public:
    Arduino_FT3x68(std::shared_ptr<Arduino_IIC_DriveBus> bus, uint8_t device_address,
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