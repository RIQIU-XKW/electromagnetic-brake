/**
 *******************************************************************************
 * @file    enet_platform.c
 * @brief   Ethernet 模块的平台层 —— 毫秒时间基准的实现
 *
 * 全部实现就是 "上电以来的毫秒数", 由 OSAL 提供(Osal_UptimeMs()).  这里刻意不碰
 * 任何定时器: SCU 私有定时器是 RTOS 的 tick 源(见 enet_platform.h), 而 bsp 也
 * 不许直接依赖 RTOS —— 时间/任务/中断一律走 osal/。
 *******************************************************************************
 */

#include "enet_platform.h"

#include "osal/osal.h"

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

Enet_StatusTypeDef Enet_Platform_Init(void)
{
    /* 时间基准由 OSAL 提供, 没有硬件需要配置, 也不会失败. */
    return ENET_OK;
}

Enet_StatusTypeDef Enet_Platform_EnableInterrupts(void)
{
    return ENET_OK;             /* 没有需要连接的中断(见文件头说明) */
}

void Enet_Platform_DeInit(void)
{
    /* 时间基准属于 OSAL, 这里没有资源需要释放. */
}

uint32_t Enet_Platform_GetTicks(void)
{
    /* 调度器启动前返回 0, 正好符合 "上电以来 0 毫秒"(换算在 OSAL 里用 64 位做). */
    return Osal_UptimeMs();
}

uint32_t Enet_Platform_TicksToSeconds(uint32_t ms)
{
    return ms / 1000U;
}
