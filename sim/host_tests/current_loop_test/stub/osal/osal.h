/**
 *******************************************************************************
 * @file    osal.h  (HOST STUB)
 * @brief   主机测试用的 OSAL 桩: 只留 Osal_DelayMs, 并把它变成"单步执行"的把手
 *
 * 被测的是 Svc_CurLoop_Task() —— 一个 for(;;) 的死循环。桩里每被调一次
 * Osal_DelayMs 就算走完一拍, 走够 Host_Step() 要求的拍数就 longjmp 回驱动代码,
 * 于是可以一拍一拍地驱动它、每拍之后做断言。
 *******************************************************************************
 */

#ifndef STUB_OSAL_H
#define STUB_OSAL_H

#include <stdint.h>

/** 延时; 桩里 = 走完一个控制周期(到拍数就 longjmp 回测试)。 */
void Osal_DelayMs(uint32_t Ms);

#endif /* STUB_OSAL_H */
