/**
 *******************************************************************************
 * @file    log.c
 * @brief   简单日志 — 互斥量与时间戳(全部通过 OSAL, 本文件不认识 FreeRTOS)
 *
 * 整个文件只有两个作用:
 *   - 提供一个互斥量, 保证一条日志不会被别的任务切断;
 *   - 提供 "上电多少毫秒" 这个时间基准.
 *
 * 两者都由 osal/ 提供, 所以以后换 RTOS 只要重写 osal_freertos.c —— 日志模块
 * 一行都不用改.
 *******************************************************************************
 */

#include "service/monitor_module/log/log.h"

#include "osal/osal.h"

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

/**
 * @brief  日志互斥量, Log_Init() 建立.  NULL 表示还没建立(启动早期).
 *
 * @note   必须是**递归**互斥量(Osal_MutexCreateRecursive).  典型场景: 监视任务
 *         用 Log_Lock() 把 "几行详细状态" 圈成一段, 而这段里的 Enet_PrintStatus()
 *         等函数内部又会调用 LOG_I() —— 也就是同一个任务再取一次这把锁.  普通
 *         互斥量在这里会永久死锁(任务自己等自己), 现象就是"打印到某一行后整个
 *         系统不再输出".
 */
static Osal_MutexHandleTypeDef LogMutex = NULL;

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

void Log_Init(void)
{
    if (LogMutex == NULL) {
        LogMutex = Osal_MutexCreateRecursive();
    }
}

void Log_Lock(void)
{
    /* 调度器没起来时不能取锁(会阻塞在还不存在的调度器上), 退化成空操作. */
    if ((LogMutex != NULL) && (Osal_SchedulerRunning() != 0)) {
        Osal_MutexLock(LogMutex);
    }
}

void Log_Unlock(void)
{
    if ((LogMutex != NULL) && (Osal_SchedulerRunning() != 0)) {
        Osal_MutexUnlock(LogMutex);
    }
}

uint32_t Log_UptimeMs(void)
{
    return Osal_UptimeMs();
}

void Log_Banner(const char *title)
{
    Log_Lock();
    xil_printf("\r\n");
    xil_printf("========================================================\r\n");
    xil_printf(" %s\r\n", title);
    xil_printf("========================================================\r\n");
    Log_Unlock();
}
