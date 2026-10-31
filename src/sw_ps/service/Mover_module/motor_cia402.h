/**
 *******************************************************************************
 * @file    motor_cia402.h
 * @brief   直线电机驱动器寄存器表 —— Modbus RTU + CiA402(DS402) 伺服行规
 *
 *   物理层  RS-485 半双工(bsp/rs485, 115200 8N1)
 *   链路层  Modbus RTU, 从站 0x01, CRC16 低字节在前; 只发不收(驱动器不回复)
 *   应用层  CiA402 对象字典(下面这张表)
 *
 * 单位(手册 1.1.5 的 607Ah / 6064h 是 pulse, 当量取决于负载分辨率, 没确认过):
 *   0x6081 轮廓速度 = mm/s * 1000
 *   0x607A 目标位置 = 原始脉冲值, 不换算
 *******************************************************************************
 */

#ifndef MOTOR_CIA402_H
#define MOTOR_CIA402_H

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/*  从站地址                                                                   */
/* -------------------------------------------------------------------------- */

#define MOTOR_SLAVE_ADDR_DEFAULT    0x01U   /**< 驱动器从站地址                  */

/* Modbus 功能码(0x03/0x06/0x10)定义在 modbus_rtu.h —— 那是链路层的事。 */

/* -------------------------------------------------------------------------- */
/*  CiA402 对象字典(本应用用到的部分)                                          */
/* -------------------------------------------------------------------------- */

#define CIA402_OD_CONTROLWORD       0x6040U /**< 控制字      16 位, R/W         */
#define CIA402_OD_STATUSWORD        0x6041U /**< 状态字      16 位, R           */
#define CIA402_OD_MODES_OF_OP       0x6060U /**< 运行模式     8 位, R/W         */
#define CIA402_OD_MODES_DISPLAY     0x6061U /**< 当前模式     8 位, R           */
#define CIA402_OD_TARGET_POSITION   0x607AU /**< 目标位置    32 位, R/W  单位 pulse */
#define CIA402_OD_POSITION_ACTUAL   0x6064U /**< 实际位置    32 位, R    单位 pulse */
#define CIA402_OD_PROFILE_VELOCITY  0x6081U /**< 轮廓速度    32 位, R/W        */
#define CIA402_OD_TARGET_VELOCITY   0x60FFU /**< 目标速度    32 位, R/W        */

/* 32 位对象占 2 个寄存器, 写它们一律用 0x10(写多个):
 *   01 10 <reg_hi> <reg_lo> 00 02 04 <b31..b24> <b23..b16> <b15..b8> <b7..b0> <crc_lo> <crc_hi>
 * 例(0.5 m/s): 01 10 60 81 00 02 04 00 07 A1 20 5B 88 */

/* -------------------------------------------------------------------------- */
/*  CiA402 运行模式(0x6060)                                                    */
/* -------------------------------------------------------------------------- */

#define CIA402_MODE_PROFILE_POSITION    1   /**< PP: 轮廓位置模式(本工程用这个) */
#define CIA402_MODE_PROFILE_VELOCITY    3   /**< PV: 轮廓速度模式               */
#define CIA402_MODE_HOMING              6   /**< HM: 回零模式                   */
#define CIA402_MODE_CSP                 8   /**< 周期同步位置                   */

/* -------------------------------------------------------------------------- */
/*  CiA402 控制字(0x6040) —— 标准状态机                                        */
/* -------------------------------------------------------------------------- */

#define CIA402_CW_SWITCH_ON         0x0001U /**< bit0 Switch on                 */
#define CIA402_CW_ENABLE_VOLTAGE    0x0002U /**< bit1 Enable voltage            */
#define CIA402_CW_QUICK_STOP        0x0004U /**< bit2 Quick stop                */
#define CIA402_CW_ENABLE_OPERATION  0x0008U /**< bit3 Enable operation          */
#define CIA402_CW_NEW_SETPOINT      0x0010U /**< bit4 新设定点(PP 模式: 上升沿触发) */
#define CIA402_CW_CHANGE_SETPOINT   0x0020U /**< bit5 立即改变设定点            */
#define CIA402_CW_FAULT_RESET       0x0080U /**< bit7 故障复位                  */

/* 常用命令值 */
#define CIA402_CW_CMD_SHUTDOWN      0x0006U /**< Shutdown:            准备上电   */
#define CIA402_CW_CMD_SWITCH_ON     0x0007U /**< Switch on:           上电       */
#define CIA402_CW_CMD_ENABLE_OP     0x000FU /**< Enable operation:    使能运行   */
#define CIA402_CW_CMD_START_MOVE    0x001FU /**< Enable op + 新设定点: 启动定位  */
#define CIA402_CW_CMD_DISABLE_VOLT  0x0000U /**< 去掉使能电压(自由停车)          */
#define CIA402_CW_CMD_FAULT_RESET   0x0080U /**< 清故障                         */

/* -------------------------------------------------------------------------- */
/*  CiA402 状态字(0x6041) 常用位                                               */
/* -------------------------------------------------------------------------- */

#define CIA402_SW_READY_TO_SWITCH_ON    0x0001U
#define CIA402_SW_SWITCHED_ON           0x0002U
#define CIA402_SW_OPERATION_ENABLED     0x0004U
#define CIA402_SW_FAULT                 0x0008U
#define CIA402_SW_VOLTAGE_ENABLED       0x0010U
#define CIA402_SW_QUICK_STOP            0x0020U
#define CIA402_SW_TARGET_REACHED        0x0400U
#define CIA402_SW_INTERNAL_LIMIT_ACTIVE 0x0800U

/* -------------------------------------------------------------------------- */
/*  单位换算                                                                   */
/* -------------------------------------------------------------------------- */

/** 1 mm/s 对应 0x6081 的数值(上层用 mm/s 口径, 这个换算躲不掉)。 */
#define MOTOR_VELOCITY_PER_MMPS     1000L

/** mm/s -> 0x6081 数值。不检查溢出, 调用方先限幅。 */
#define MOTOR_MMPS_TO_VELOCITY(mmps)    ((int32_t)(mmps) * MOTOR_VELOCITY_PER_MMPS)

#endif /* MOTOR_CIA402_H */
