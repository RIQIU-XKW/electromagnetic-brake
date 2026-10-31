/**
 *******************************************************************************
 * @file    service_motor.h
 * @brief   直线电机(Modbus RTU / CiA402) 发帧组件 —— 只发不收
 *
 * 只做"发一帧": 用 modbus_rtu.c 组帧, 经本模块的 service_rs485 发上总线(发帧期间拿住
 * 总线锁)。不认识命令、不认识时序、没有任务 —— 发射小车的时序和命令都在 app/Mover_app。
 *
 *   app/Mover_app        命令(0xD0..0xD6) + 发射时序状态机 + 任务   <- 业务
 *   service/Mover_module 本文件(组帧 + CiA402 动作) / service_rs485(总线)
 *   bsp/rs485            纯硬件(UART + DE 方向)
 *
 * 驱动器只接收不回复: 不等回复、不解析、不统计。
 * 不做位置闭环/轨迹规划(交给驱动器, 用 PP 模式丢目标)。
 *******************************************************************************
 */

#ifndef SERVICE_MOTOR_H
#define SERVICE_MOTOR_H

#include <stdint.h>

#include "service/service.h"

/* -------------------------------------------------------------------------- */
/*  组件参数                                                                   */
/* -------------------------------------------------------------------------- */

/** 两帧之间至少留的空隙(ms)。规定是 3.5 字符(115200 下约 0.3 ms), 这里给足 2 ms。 */
#define SVC_MOTOR_FRAME_GAP_MS      2U

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/** @brief  组件初始化(从站地址复位成默认值, 清"初始化过"标记)。幂等, 本层没有任务。 */
Svc_StatusTypeDef Svc_Motor_Init(void);

/**
 * @brief  电机初始化序列: 0x6060=1(PP 模式) -> 0x6040=6(Shutdown) -> 0x6040=7(上电)。
 * @return 1 = 三条都发出去了; 0 = 有帧没发出去。
 */
int Svc_Motor_DoInit(void);

/** @brief  使能运行(控制字 0x000F)。 */
int Svc_Motor_DoEnable(void);

/** @brief  停机(控制字 0x0006): 去掉使能, 电机自由。 */
int Svc_Motor_DoStop(void);

/** @brief  设轮廓速度(mm/s -> 0x6081, 1 mm/s = 1000, 见 motor_cia402.h)。 */
int Svc_Motor_DoSpeed(int32_t Mmps);

/** @brief  写目标位置 0x607A, 参数就是寄存器值(pulse, 不做换算)。 */
int Svc_Motor_DoPosition(int32_t Value);

/**
 * @brief  启动定位(控制字 0x001F: Enable operation + 新设定点)。
 * @note   bit4 要先清再置: 连续两次定位之间必须先发 0x000F 再发 0x001F。
 */
int Svc_Motor_DoGo(void);

#endif /* SERVICE_MOTOR_H */
