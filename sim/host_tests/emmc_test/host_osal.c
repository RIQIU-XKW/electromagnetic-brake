/**
 * 宿主机测试用: OSAL 的替身 + 日志桩。
 *
 * bsp/emmc/emmc.c 只用到互斥量(把 FatFs 串起来), 而这个测试是**单线程**顺序调用,
 * 所以互斥量给个空实现就够了 —— 这里要验的是"文件内容对不对", 不是并发。
 *
 * 真正的并发保护(写数据的任务 vs 回读命令)只能上板验, 见 tools/emmc_test/README.md
 * 里"本测试不覆盖什么"那一节。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "osal/osal.h"

/* ========================================================================== */
/*  互斥量: 空实现(单线程测试)                                                 */
/* ========================================================================== */

Osal_MutexHandleTypeDef Osal_MutexCreateRecursive(void)
{
    /* 返回一个非 NULL 的假句柄: emmc.c 用 NULL 判断"建锁失败" */
    return (Osal_MutexHandleTypeDef)malloc(1);
}

void Osal_MutexLock(Osal_MutexHandleTypeDef M)
{
    (void)M;
}

void Osal_MutexUnlock(Osal_MutexHandleTypeDef M)
{
    (void)M;
}

/* ========================================================================== */
/*  时间/延时                                                                  */
/* ========================================================================== */

uint32_t Osal_UptimeMs(void) { return 0U; }
void     Osal_DelayMs(uint32_t Ms) { (void)Ms; }

/* ========================================================================== */
/*  日志桩                                                                     */
/* ========================================================================== */

void     Log_Init(void) { }
void     Log_Lock(void) { }
void     Log_Unlock(void) { }
uint32_t Log_UptimeMs(void) { return 0U; }
void     Log_Banner(const char *title) { (void)title; }

void xil_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)vprintf(fmt, ap);
    va_end(ap);
}
