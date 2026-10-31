/**
 *******************************************************************************
 * @file    service_rs485.c
 * @brief   rs485 业务组件 —— 端口起来 + **只发** + 总线锁
 *
 * 2026-10-15 裁剪: 这条总线上唯一的协议组件(Mover_module 里的电机)是**只发不收**的,
 * 所以收字节/订阅/统计/快照/DE 诊断命令全删, 剩下的就是"把端口带起来 + 发一段字节 +
 * 给协议组件一把递归总线锁"。
 * ⚠ 2026-10-16: 本文件随电机一起挪进 service/Mover_module/(它就是这条总线的载体)。
 *******************************************************************************
 */

#define LOG_ENABLE 0   /* 本模块日志默认关闭; 需要时改成 1 */
#define LOG_TAG "485"
#include "service/monitor_module/log/log.h"

#include "service/Mover_module/service_rs485.h"

#include "bsp/rs485/rs485.h"    /* 本组件直接驱动 RS-485(service -> bsp) */

#include "osal/osal.h"

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static Rs485_HandleTypeDef     gRs485;
static Osal_MutexHandleTypeDef gBusMutex;       /**< 总线锁(递归): 一次事务的原子性 */
static uint8_t                 gReady;
static uint8_t                 gStarted;        /**< 1 = 任务已经把端口 Start 过了   */

/* ========================================================================== */
/*  内部函数(static)                                                           */
/* ========================================================================== */

/**
 * @brief  rs485 任务: 把端口 Start 起来, 然后**一直睡**(创建由应用层负责)。
 */
void Svc_Rs485_Task(void *Arg)
{
    Rs485_StatusTypeDef status;

    (void)Arg;

    status = Rs485_Start(&gRs485);
    if (status != RS485_OK) {
        LOG_W("port start returned %d (rx side is unused anyway)\r\n", (int)status);
    }

    gStarted = 1U;

    LOG_I("task started (%u baud, tx only)\r\n", (unsigned)gRs485.Config.BaudRate);

    for (;;) {
        /* 没有任何事要做: 发帧由调用者自己走 Svc_Rs485_Send()(它拿总线锁) */
        (void)Osal_TaskNotifyTake(OSAL_WAIT_FOREVER);
    }
}

/* ========================================================================== */
/*  对外接口                                                                */
/* ========================================================================== */

Svc_StatusTypeDef Svc_Rs485_Init(void)
{
    Rs485_ConfigTypeDef cfg;

    Rs485_Config_LoadDefaults(&cfg);

    if (Rs485_Init(&gRs485, &cfg) != RS485_OK) {
        return SVC_ERR_HW;
    }

    /* 总线锁: 递归, 因为持锁方在自己事务里还会再调 Svc_Rs485_Send() */
    gBusMutex = Osal_MutexCreateRecursive();
    if (gBusMutex == NULL) {
        LOG_E("cannot create the bus mutex\r\n");
        return SVC_ERR_HW;
    }

    gReady = 1U;

    LOG_I("ready: %u baud (tx only, no rx hook)\r\n", (unsigned)cfg.BaudRate);

    return SVC_OK;
}

uint8_t Svc_Rs485_IsReady(void)
{
    return gReady;
}

Svc_StatusTypeDef Svc_Rs485_Send(const uint8_t *Data, uint16_t Len)
{
    Rs485_StatusTypeDef status;

    if ((Data == NULL) || (Len == 0U)) {
        return SVC_ERR_PARAM;
    }

    if (gReady == 0U) {
        return SVC_ERR_STATE;
    }

    /* 拿总线锁: 发的时候不能让别的调用者也来抢(半双工, DE 方向是共享的)。
     * 锁是递归的, 所以持锁方(同目录 service_motor 的 Modbus 事务)进来重入不会死锁。 */
    Svc_Rs485_BusLock();

    status = Rs485_Send(&gRs485, Data, Len);

    Svc_Rs485_BusUnlock();

    switch (status) {
        case RS485_OK:              return SVC_OK;
        case RS485_ERR_PARAM:       return SVC_ERR_PARAM;
        case RS485_ERR_STATE:       return SVC_ERR_STATE;
        default:                    return SVC_ERR_HW;
    }
}

/* ========================================================================== */
/*  总线借用(给要在这条总线上跑协议的组件: service_motor)                         */
/* ========================================================================== */

void Svc_Rs485_BusLock(void)
{
    Osal_MutexLock(gBusMutex);
}

void Svc_Rs485_BusUnlock(void)
{
    Osal_MutexUnlock(gBusMutex);
}
