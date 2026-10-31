/**
 *******************************************************************************
 * @file    bsp_oc.h
 * @brief   过流(OC)采集驱动 —— 5 个桥的过流输入, 纯硬件, 无业务逻辑
 *
 * 5 路过流信号 oc_n[4:0](一桥一路, 低有效)在硬件上直接进 PL 的 PWM IP, PS 侧没有
 * 这 5 路的 GPIO, 所以本驱动读的是 PWM IP 的 STATUS:
 *   STATUS[4]     OC_NOW      此刻有没有 oc_n 为低(全桥合并)
 *   STATUS[5]     OC_LATCHED  发生过过流, 粘滞, 直到 acknowledge
 *   STATUS[12:8]  OC_SRC      哪几个桥报的过流(bit i = 桥 i)
 *
 * 硬件不给"每个桥此刻单独的电平", 只给合并的 now + 分桥的 latched, 所以
 * Bsp_Oc_NowMask() 是尽力而为的组合。想要精确的逐路实时电平需要改 PL。
 *
 * 本层只回答"状态位是多少"和"清粘滞", 不做滤波/保护/上报(那在 service 和 app)。
 *******************************************************************************
 */

#ifndef BSP_OC_H
#define BSP_OC_H

#include <stdint.h>

#include "bsp/pwm/pwm.h"

/** 路数 = H 桥个数(= PL PWM IP 的 oc_n 位数)。 */
#define BSP_OC_CHANNELS         5U

/** 位图掩码(bit i = 第 i 路 / 第 i 个桥)。 */
#define BSP_OC_MASK_ALL         ((1U << BSP_OC_CHANNELS) - 1U)

/**
 * @brief  绑定到 PWM IP(本驱动不拥有那个 IP, 只借它的状态寄存器)。
 * @note   必须在 Pwm_Init() 之后调。PwmHandle = NULL 时本驱动变成"永远无过流"。
 */
void Bsp_Oc_Init(Pwm_HandleTypeDef *PwmHandle);

/** @brief  初始化过没有(0 = 没绑定句柄, 所有读数都当"无过流")。 */
int Bsp_Oc_IsReady(void);

/** @brief  此刻有没有任何一路 oc_n 为低(STATUS[4])。 */
int Bsp_Oc_AnyNow(void);

/** @brief  5 路"此刻"的位图(尽力而为: 有过流就拿 OC_SRC 当正在报的桥)。 */
uint32_t Bsp_Oc_NowMask(void);

/** @brief  发生过过流的桥(粘滞位, 不 acknowledge 就一直有)。 */
uint32_t Bsp_Oc_LatchedMask(void);

/** @brief  有没有发生过过流(STATUS[5], 粘滞)。 */
int Bsp_Oc_AnyLatched(void);

/**
 * @brief  清过流粘滞标志。
 * @note   会顺手把 PWM 全局使能关一下再开(Pwm_AcknowledgeFault), 只在确认安全后调。
 * @return 1 = 清了, 0 = 没句柄 / 失败。
 */
int Bsp_Oc_Acknowledge(void);

/** @brief  读原始 STATUS 寄存器(排障用)。 */
uint32_t Bsp_Oc_RawStatus(void);

/** @brief  把位图格式化成 "B1 B3" 这样的可读串(dst 至少 24 字节)。 */
void Bsp_Oc_FormatMask(uint32_t Mask, char *Dst, uint16_t Size);

#endif /* BSP_OC_H */
