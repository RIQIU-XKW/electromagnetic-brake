/**
 *******************************************************************************
 * @file    osal_freertos.c
 * @brief   OSAL 的 FreeRTOS 实现 —— **全工程唯一允许出现 FreeRTOS 符号的文件**
 *
 * 换 RTOS / 换裸机时只重写这一个文件, 其它层一行都不用动:
 *
 *   - 任务      -> xTaskCreate / vTaskDelay / notify(从 ISR 与任务两个方向)
 *   - 队列      -> xQueueCreate / xQueueSend / xQueueReceive
 *   - 互斥量    -> xSemaphoreCreateRecursiveMutex / Take / Give
 *   - 时间      -> xTaskGetTickCount / configTICK_RATE_HZ / pdMS_TO_TICKS
 *   - 中断连接  -> xPortInstallInterruptHandler / vPortEnableInterrupt ...
 *
 * @note  为什么中断连接也在 OSAL: 保留的 bsp/rs485 驱动要连 UART 中断, 而
 *        FreeRTOS 端口装中断的 API 属于 RTOS 侧 —— 收口到这里, bsp 就不必
 *        include FreeRTOS。
 *******************************************************************************
 */

#include "osal/osal.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "portmacro.h"          /* xPortInstallInterruptHandler / vPortEnableInterrupt */

/* ========================================================================== */
/*  任务函数                                                                     */
/* ========================================================================== */

Osal_StatusTypeDef Osal_TaskCreate(const char *Name, Osal_TaskFuncTypeDef Func, void *Arg,
                                   uint32_t StackWords, uint32_t Priority,
                                   Osal_TaskHandleTypeDef *Out)
{
    BaseType_t rc;

    if ((Name == NULL) || (Func == NULL) || (StackWords == 0U) || (Priority == 0U)) {
        return OSAL_ERR_PARAM;
    }

    rc = xTaskCreate(Func, Name, (configSTACK_DEPTH_TYPE)StackWords, Arg,
                     (UBaseType_t)Priority, (TaskHandle_t *)Out);

    return (rc == pdPASS) ? OSAL_OK : OSAL_ERR_NOMEM;
}

Osal_TaskHandleTypeDef Osal_TaskSelf(void)
{
    return (Osal_TaskHandleTypeDef)xTaskGetCurrentTaskHandle();
}

void Osal_TaskNotifyGive(Osal_TaskHandleTypeDef Task)
{
    if (Task != NULL) {
        (void)xTaskNotifyGive((TaskHandle_t)Task);
    }
}

void Osal_TaskNotifyGiveFromIsr(Osal_TaskHandleTypeDef Task, int *HigherPriorityTaskWoken)
{
    BaseType_t woken = pdFALSE;

    if (Task != NULL) {
        vTaskNotifyGiveFromISR((TaskHandle_t)Task, &woken);
    }

    if (HigherPriorityTaskWoken != NULL) {
        *HigherPriorityTaskWoken = (woken != pdFALSE) ? 1 : 0;
    }
}

uint32_t Osal_TaskNotifyTake(uint32_t WaitMs)
{
    TickType_t ticks = (WaitMs == OSAL_WAIT_FOREVER) ? portMAX_DELAY : pdMS_TO_TICKS(WaitMs);

    return (uint32_t)ulTaskNotifyTake(pdTRUE, ticks);
}

/* ========================================================================== */
/*  队列                                                                       */
/* ========================================================================== */

Osal_QueueHandleTypeDef Osal_QueueCreate(uint32_t Length, uint32_t ItemSize)
{
    if ((Length == 0U) || (ItemSize == 0U)) {
        return NULL;
    }

    return (Osal_QueueHandleTypeDef)xQueueCreate((UBaseType_t)Length, (UBaseType_t)ItemSize);
}

Osal_StatusTypeDef Osal_QueueSend(Osal_QueueHandleTypeDef Q, const void *Item, uint32_t WaitMs)
{
    TickType_t ticks;
    BaseType_t rc;

    if ((Q == NULL) || (Item == NULL)) {
        return OSAL_ERR_PARAM;
    }

    ticks = (WaitMs == OSAL_WAIT_FOREVER) ? portMAX_DELAY : pdMS_TO_TICKS(WaitMs);
    rc    = xQueueSend((QueueHandle_t)Q, Item, ticks);

    return (rc == pdPASS) ? OSAL_OK : OSAL_ERR_FULL;
}

Osal_StatusTypeDef Osal_QueueReceive(Osal_QueueHandleTypeDef Q, void *Item, uint32_t WaitMs)
{
    TickType_t ticks;
    BaseType_t rc;

    if ((Q == NULL) || (Item == NULL)) {
        return OSAL_ERR_PARAM;
    }

    ticks = (WaitMs == OSAL_WAIT_FOREVER) ? portMAX_DELAY : pdMS_TO_TICKS(WaitMs);
    rc    = xQueueReceive((QueueHandle_t)Q, Item, ticks);

    return (rc == pdPASS) ? OSAL_OK : OSAL_ERR_TIMEOUT;
}

/* ========================================================================== */
/*  互斥量                                                                     */
/* ========================================================================== */

Osal_MutexHandleTypeDef Osal_MutexCreateRecursive(void)
{
    return (Osal_MutexHandleTypeDef)xSemaphoreCreateRecursiveMutex();
}

void Osal_MutexLock(Osal_MutexHandleTypeDef M)
{
    if (M != NULL) {
        (void)xSemaphoreTakeRecursive((SemaphoreHandle_t)M, portMAX_DELAY);
    }
}

void Osal_MutexUnlock(Osal_MutexHandleTypeDef M)
{
    if (M != NULL) {
        (void)xSemaphoreGiveRecursive((SemaphoreHandle_t)M);
    }
}

/* ========================================================================== */
/*  时间                                                                       */
/* ========================================================================== */

void Osal_DelayMs(uint32_t Ms)
{
    TickType_t ticks = pdMS_TO_TICKS(Ms);

    /* 1 个 tick 是调度器的最小时间片; 睡 0 tick 会把低优先级任务饿死. */
    if (ticks == 0U) {
        ticks = 1U;
    }

    vTaskDelay(ticks);
}

uint32_t Osal_MsToTicks(uint32_t Ms)
{
    return (uint32_t)pdMS_TO_TICKS(Ms);
}

uint32_t Osal_TicksToMs(uint32_t Ticks)
{
    /* 64 位中间量: 任何 tick 频率下都精确, 运行几天也不会溢出. */
    return (uint32_t)(((uint64_t)Ticks * 1000ULL) / (uint64_t)configTICK_RATE_HZ);
}

uint32_t Osal_TickRateHz(void)
{
    return (uint32_t)configTICK_RATE_HZ;
}

uint32_t Osal_UptimeMs(void)
{
    if (Osal_SchedulerRunning() == 0) {
        return 0U;                          /* 调度器没跑, tick 不会前进       */
    }

    return Osal_TicksToMs((uint32_t)xTaskGetTickCount());
}

int Osal_SchedulerRunning(void)
{
    return (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) ? 1 : 0;
}

/* ========================================================================== */
/*  调度器                                                                     */
/* ========================================================================== */

void Osal_StartScheduler(void)
{
    vTaskStartScheduler();
}

/* ========================================================================== */
/*  中断                                                                       */
/* ========================================================================== */

Osal_StatusTypeDef Osal_InterruptConnect(uint32_t IntrId, Osal_IsrHandlerTypeDef Handler,
                                         void *Arg)
{
    if (Handler == NULL) {
        return OSAL_ERR_PARAM;
    }

    if (xPortInstallInterruptHandler((uint16_t)IntrId, (XInterruptHandler)Handler,
                                     Arg) != pdPASS) {
        return OSAL_ERR_HW;
    }

    return OSAL_OK;
}

void Osal_InterruptEnable(uint32_t IntrId)
{
    vPortEnableInterrupt((uint16_t)IntrId);
}

void Osal_InterruptDisable(uint32_t IntrId)
{
    vPortDisableInterrupt((uint16_t)IntrId);
}

void Osal_IsrYield(int HigherPriorityTaskWoken)
{
    portYIELD_FROM_ISR(HigherPriorityTaskWoken);
}
