/**
 *******************************************************************************
 * @file    host_stubs.h
 * @brief   主机测试的把手: 伪造电流反馈、记下 PWM 输出、把死循环切成单步
 *******************************************************************************
 */

#ifndef HOST_STUBS_H
#define HOST_STUBS_H

#include <stdint.h>
#include <setjmp.h>

/** 单步执行用的跳转点/计数器(定义在 host_stubs.c)。 */
extern jmp_buf         gHostJmp;
extern volatile int    gHostStepLimit;
extern volatile int    gHostSteps;

/** 让下一段 Svc_CurLoop_Task() 只跑 N 拍就返回(配合 Host_RunLoop)。 */
void Host_Step(int Cycles);

/**
 * 跑 N 个控制周期。
 *
 * 每次 setjmp 之后重新进 Svc_CurLoop_Task(), 它从 for(;;) 顶部继续 —— 所有状态都在
 * 静态变量里, 所以"分段跑"和"一口气跑"对被测逻辑完全等价。
 */
void Host_RunLoop(int Cycles);

/* ---- 伪造的 5 路电流反馈(mA) -------------------------------------------- */
void Host_SetCurrentMa(int Coil, int32_t MilliAmps);
void Host_SetAllCurrentMa(int32_t MilliAmps);

/** 模拟上位机在 PWM 栏发一条死区帧(0xDC)。 */
void Host_ApplyManualDeadNs(uint32_t Ns);

/* ---- 记录下来的 PWM 输出 ------------------------------------------------ */
uint32_t Host_GetDutyPermille(int Bridge);      /**< 最近一次写入的千分比(没写过=0) */
int      Host_GetBridgeEnable(void);            /**< 五桥使能最近一次写的值           */
int      Host_GetGlobalEnable(void);            /**< 全局调制最近一次写的值           */
uint32_t Host_GetDutyWrites(void);              /**< 占空比写入次数                   */
uint32_t Host_GetEnableWrites(void);            /**< 桥使能/调制使能写入次数          */
uint32_t Host_GetDeadNs(void);                  /**< 最近一次设下去的死区(ns)         */
uint32_t Host_GetDeadWrites(void);              /**< 死区写入次数                     */

/** 外部(模拟 0xA5 / 过流)直接把功率级关掉, 不动闭环状态。 */
void Host_ForcePowerStageOff(void);

/** 打开/关闭桩日志输出。 */
void Host_SetLogVerbose(int On);

#endif /* HOST_STUBS_H */
