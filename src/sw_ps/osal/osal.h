/**
 *******************************************************************************
 * @file    osal.h
 * @brief   OSAL —— 操作系统抽象层(第二层): 任务 / 队列 / 互斥量 / 时间 / 中断
 *
 * 全工程只有 osal_freertos.c 允许出现 FreeRTOS 符号, 其它层只调这里的接口, 于是:
 *   - 换 RTOS 或换裸机只要重写那一个文件;
 *   - 分层检查可以量化: xTaskCreate / xQueueCreate 等只出现在 osal_freertos.c。
 *
 * 约定:
 *   1. 句柄一律是不透明指针, 调用者不许解引用;
 *   2. 时间单位统一用毫秒, WaitMs 用 OSAL_WAIT_FOREVER / OSAL_WAIT_NOWAIT;
 *   3. 只有 osal_freertos.c 能 include FreeRTOS 的头文件。
 *
 * @note 中断相关接口是给 bsp 的扩展: rs485 要连 UART 中断, 而装中断的 API 属于
 *       RTOS 端口, 由 OSAL 收口, 免得 bsp 直接依赖 FreeRTOS。
 *******************************************************************************
 */

#ifndef OSAL_H
#define OSAL_H

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/*  返回值与常量                                                               */
/* -------------------------------------------------------------------------- */

typedef enum {
    OSAL_OK          =  0,
    OSAL_ERR_PARAM   = -1,
    OSAL_ERR_FULL    = -2,
    OSAL_ERR_TIMEOUT = -3,
    OSAL_ERR_NOMEM   = -4,
    OSAL_ERR_HW      = -5       /**< 平台资源出错(例如中断连接失败)         */
} Osal_StatusTypeDef;

/** 永久等待(传给 WaitMs)。 */
#define OSAL_WAIT_FOREVER   0xFFFFFFFFU

/** 不等待, 立即返回。 */
#define OSAL_WAIT_NOWAIT    0U

/* -------------------------------------------------------------------------- */
/*  不透明句柄与函数指针                                                       */
/* -------------------------------------------------------------------------- */

/** 任务入口: Arg 就是 Osal_TaskCreate() 传进来的指针。 */
typedef void (*Osal_TaskFuncTypeDef)(void *Arg);

/** 中断服务程序入口: Arg 是 Osal_InterruptConnect() 传进来的指针。 */
typedef void (*Osal_IsrHandlerTypeDef)(void *Arg);

typedef void *Osal_TaskHandleTypeDef;       /**< 不透明: 任务句柄             */
typedef void *Osal_QueueHandleTypeDef;      /**< 不透明: 队列句柄             */
typedef void *Osal_MutexHandleTypeDef;      /**< 不透明: 互斥量句柄           */

/* -------------------------------------------------------------------------- */
/*  任务函数                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  建一个任务。
 * @param  Name       : 任务名(仅调试用, 不拷贝, 必须是常量字符串)。
 * @param  StackWords : 栈大小, 单位是字(1 字 = 4 字节), 和 FreeRTOS 一致。
 * @param  Priority   : 优先级, 数字越大越高。
 * @param  Out        : 输出句柄, 可以为 NULL。
 */
Osal_StatusTypeDef Osal_TaskCreate(const char *Name, Osal_TaskFuncTypeDef Func, void *Arg,
                                   uint32_t StackWords, uint32_t Priority,
                                   Osal_TaskHandleTypeDef *Out);

/** @brief  当前任务自己的句柄(用于把中断或别的任务叫醒自己)。 */
Osal_TaskHandleTypeDef Osal_TaskSelf(void);

/** @brief  任务通知: 给某个任务加一个计数(无需队列的轻量唤醒); Task 必须非 NULL。 */
void Osal_TaskNotifyGive(Osal_TaskHandleTypeDef Task);

/** @brief  任务通知的中断安全版本; Woken 输出是否唤醒了更高优先级任务。 */
void Osal_TaskNotifyGiveFromIsr(Osal_TaskHandleTypeDef Task, int *HigherPriorityTaskWoken);

/** @brief  等一次任务通知(把计数清零); 返回收到通知前累计的次数(0 = 超时)。 */
uint32_t Osal_TaskNotifyTake(uint32_t WaitMs);

/* -------------------------------------------------------------------------- */
/*  队列                                                                       */
/* -------------------------------------------------------------------------- */

/** @brief  建一个队列(元素按值拷贝); 失败返回 NULL。 */
Osal_QueueHandleTypeDef Osal_QueueCreate(uint32_t Length, uint32_t ItemSize);

/** @brief  发一个元素。 */
Osal_StatusTypeDef Osal_QueueSend(Osal_QueueHandleTypeDef Q, const void *Item, uint32_t WaitMs);

/** @brief  收一个元素。 */
Osal_StatusTypeDef Osal_QueueReceive(Osal_QueueHandleTypeDef Q, void *Item, uint32_t WaitMs);

/* -------------------------------------------------------------------------- */
/*  互斥量                                                                     */
/* -------------------------------------------------------------------------- */

/** @brief  建一个递归互斥量(日志模块会持锁再取锁, 普通互斥量会死锁); 失败返回 NULL。 */
Osal_MutexHandleTypeDef Osal_MutexCreateRecursive(void);

/** @brief  取锁(永久等待); 句柄为 NULL 时是空操作。 */
void Osal_MutexLock(Osal_MutexHandleTypeDef M);

/** @brief  放锁; 句柄为 NULL 时是空操作。 */
void Osal_MutexUnlock(Osal_MutexHandleTypeDef M);

/* -------------------------------------------------------------------------- */
/*  时间                                                                       */
/* -------------------------------------------------------------------------- */

/** @brief  睡眠多少毫秒(至少 1 个 tick, 避免 0 睡眠把低优先级任务饿死)。 */
void Osal_DelayMs(uint32_t Ms);

/** @brief  毫秒 -> tick(向上取整)。 */
uint32_t Osal_MsToTicks(uint32_t Ms);

/** @brief  tick -> 毫秒。 */
uint32_t Osal_TicksToMs(uint32_t Ticks);

/** @brief  系统 tick 频率(Hz)。 */
uint32_t Osal_TickRateHz(void);

/** @brief  上电到现在的毫秒数(调度器启动前返回 0)。 */
uint32_t Osal_UptimeMs(void);

/** @brief  调度器是否已经在跑(没跑时不能调用会阻塞的接口)。 */
int Osal_SchedulerRunning(void);

/* -------------------------------------------------------------------------- */
/*  调度器                                                                     */
/* -------------------------------------------------------------------------- */

/** @brief  启动调度器; 正常情况下不返回, 返回了说明堆不够建 idle 任务。 */
void Osal_StartScheduler(void);

/* -------------------------------------------------------------------------- */
/*  中断(本工程扩展, 给 bsp 连中断用)                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief  把一个中断处理函数装到中断控制器上, 并登记到 RTOS 端口的中断表里。
 * @note   端口会把优先级设成"最低可用", 这样在该中断里调用
 *         Osal_TaskNotifyGiveFromIsr() 才合法。
 * @param  IntrId  : 中断号(来自 XUartLite_LookupConfig()->IntrId 等)。
 * @param  Handler : 中断入口(运行在中断上下文)。
 */
Osal_StatusTypeDef Osal_InterruptConnect(uint32_t IntrId, Osal_IsrHandlerTypeDef Handler,
                                         void *Arg);

/** @brief  使能某个已连接的中断。 */
void Osal_InterruptEnable(uint32_t IntrId);

/** @brief  关闭某个中断源(不摘掉处理函数)。 */
void Osal_InterruptDisable(uint32_t IntrId);

/** @brief  中断里让出 CPU(等价于 portYIELD_FROM_ISR); 参数是 FromIsr 接口的输出。 */
void Osal_IsrYield(int HigherPriorityTaskWoken);

#endif /* OSAL_H */
