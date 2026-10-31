/**
 *******************************************************************************
 * @file    enet_platform.h
 * @brief   以太网模块的平台层 —— 时间基准
 *
 * 只干一件事: 给 lwIP 和本模块的周期任务提供毫秒时间基准。
 *
 * 不自己开定时器(别再改回去): 移植到 FreeRTOS 后 SCU 私有定时器已经是 RTOS 的
 * tick 源(ps7_scutimer_0), 谁再配置它就弄坏调度器心跳; 而且 lwIP 的 sys_now()
 * 在 FreeRTOS 的 xilinx 端口里本来就是 tick 计数, 用 tick 才唯一正确, 还省掉
 * 一个中断和一个驱动实例。
 *
 * 精度 = RTOS tick 周期: 100 Hz -> 10 ms/格, 1000 Hz -> 1 ms/格(推荐)。
 *******************************************************************************
 */

#ifndef ENET_PLATFORM_H
#define ENET_PLATFORM_H

#include "enet_types.h"

/* -------------------------------------------------------------------------- */
/* 公共接口                                                                    */
/* -------------------------------------------------------------------------- */

/** @brief  检查时间基准可用(来自 OSAL 的 tick 计数); 任何时刻都能调。 */
Enet_StatusTypeDef Enet_Platform_Init(void);

/** @brief  兼容旧接口: 现在没有中断要连, 直接返回 ENET_OK。 */
Enet_StatusTypeDef Enet_Platform_EnableInterrupts(void);

/** @brief  无资源需要释放(时间基准属于 OSAL/RTOS)。 */
void Enet_Platform_DeInit(void);

/** @brief  上电以来的毫秒数。 */
uint32_t Enet_Platform_GetTicks(void);

/** @brief  毫秒 -> 秒(取整)。 */
uint32_t Enet_Platform_TicksToSeconds(uint32_t ms);

#endif /* ENET_PLATFORM_H */
