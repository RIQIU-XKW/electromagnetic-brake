/**
 *******************************************************************************
 * @file    rs485.c
 * @brief   RS-485 半双工服务实现
 *
 * 职责:
 *   - 每次发送前后管好收发器方向(DE);
 *   - 把硬件 FIFO 里的字节搬进软件 FIFO(收中断里搬, 或者 Rs485_Poll() 里搬);
 *   - 记账 IP 报出来的错误;
 *   - 把"勾子/组帧"这类应用策略留在模块外面。
 *
 * 软件 FIFO 只有一个生产者(收中断, 或者临界区里的 Rs485_Poll(), 两者互斥)和一个
 * 消费者(主循环里的应用)。
 *******************************************************************************
 */

#include "rs485.h"

#include <string.h>

/* 日志标签: 本文件的 LOG_I/W/E 会带 [rs485] 前缀 */
#define LOG_ENABLE 0   /* 本模块日志默认关闭; 需要时改成 1 */
#define LOG_TAG "rs485"
#include "service/monitor_module/log/log.h"

#include "xil_printf.h"

/* ========================================================================== */
/*  内部函数(static)                                                             */
/* ========================================================================== */

/**
 * @brief  把 UART 里现在有的东西全搬进软件 FIFO。
 * @return 搬走的字节数(0 = 硬件 FIFO 是空的)。
 *
 * 收中断里调, 另外 Rs485_Poll() 也会在关中断的情况下调一次兜底。状态位卡住也不会
 * 死循环 —— 有 RS485_RX_DRAIN_MAX 兜着。
 */
static uint32_t Rs485_RxPump(Rs485_HandleTypeDef *h)
{
    uint32_t moved = 0U;
    uint8_t  byte;

    while ((moved < RS485_RX_DRAIN_MAX) && (Rs485_Hal_IsRxDataAvailable(h) != 0U)) {
        byte = Rs485_Hal_ReadByte(h);

        if (Rs485_Fifo_Put(&h->RxFifo, byte) == 0U) {
            h->Stats.RxFifoOverruns++;      /* 软件 FIFO 满了                  */
        } else {
            h->Stats.RxBytes++;
        }

        moved++;
    }

    if (moved != 0U) {
        h->Stats.RxBursts++;
        h->LastRxTimestamp = Rs485_Hal_GetTimestamp(h);

        /* 这里**故意不动 h->Bus**: 方向状态归 Rs485_Send() 管。硬件 FIFO 里残留的
         * 字节不能让服务以为"总线已经放开了", 而 DE 其实还拉着。 */
    }

    return moved;
}

/**
 * @brief  注册给 HAL 的收中断入口。
 *
 * 中断里只做两件事: 搬字节进软件 FIFO, 然后叫醒任务。组帧/解析留给任务上下文,
 * 这样中断很短, 也不用在中断里跑协议代码。
 */
static void Rs485_RxIsr(void *context)
{
    Rs485_HandleTypeDef *h = (Rs485_HandleTypeDef *)context;

    if (h == NULL) {
        return;
    }

    if (Rs485_RxPump(h) != 0U) {
        if (h->RxWakeup != NULL) {
            h->RxWakeup(h->RxWakeupContext);
        }
    }
}

/**
 * @brief  把 HAL 锁存的错误标志记到统计里。
 *
 * AXI UART Lite 的错误位是**粘滞**的(复位接收 FIFO 之前一直为 1), 所以一串坏字节
 * 只算一次事件, 不会把计数器刷爆、把真正的数据淹掉。
 */
static void Rs485_AccountErrors(Rs485_HandleTypeDef *h)
{
    uint32_t flags = Rs485_Hal_TakeErrorFlags(h);

    if ((flags & RS485_HAL_ERR_OVERRUN) != 0U) {
        h->Stats.RxHwOverruns++;
    }
    if ((flags & RS485_HAL_ERR_FRAME) != 0U) {
        h->Stats.RxFrameErrors++;
    }
    if ((flags & RS485_HAL_ERR_PARITY) != 0U) {
        h->Stats.RxParityErrors++;
    }
}

/** @brief  等发送 FIFO 腾出一个字节的位置。 @return RS485_OK / RS485_ERR_TIMEOUT. */
static Rs485_StatusTypeDef Rs485_WaitTxRoom(Rs485_HandleTypeDef *h)
{
    uint32_t waited = 0U;

    while (Rs485_Hal_IsTxFull(h) != 0U) {
        if (waited >= h->Config.TxTimeoutUs) {
            return RS485_ERR_TIMEOUT;
        }

        Rs485_Hal_DelayUs(h, 1U);
        waited++;
    }

    return RS485_OK;
}

/** @brief  等发送 FIFO 排空。 @return RS485_OK / RS485_ERR_TIMEOUT. */
static Rs485_StatusTypeDef Rs485_WaitTxDrained(Rs485_HandleTypeDef *h)
{
    uint32_t waited = 0U;

    while (Rs485_Hal_IsTxEmpty(h) == 0U) {
        if (waited >= h->Config.TxTimeoutUs) {
            return RS485_ERR_TIMEOUT;
        }

        Rs485_Hal_DelayUs(h, 1U);
        waited++;
    }

    return RS485_OK;
}

/* ========================================================================== */
/*  配置                                                                       */
/* ========================================================================== */

void Rs485_Config_LoadDefaults(Rs485_ConfigTypeDef *cfg)
{
    if (cfg == NULL) {
        return;
    }

    (void)memset(cfg, 0, sizeof(*cfg));

    cfg->BaudRate          = RS485_UART_BAUDRATE;   /* 硬件定死, 只是照抄     */
    cfg->RxMode            = RS485_RX_INTERRUPT;
    cfg->TxTailGuardChars  = RS485_TX_TAIL_GUARD_CHARS;
    cfg->TxTimeoutUs       = RS485_TX_TIMEOUT_US;
}

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

Rs485_StatusTypeDef Rs485_Init(Rs485_HandleTypeDef *h, const Rs485_ConfigTypeDef *cfg)
{
    Rs485_StatusTypeDef status;

    if (h == NULL) {
        return RS485_ERR_PARAM;
    }

    (void)memset(h, 0, sizeof(*h));
    Rs485_Config_LoadDefaults(&h->Config);

    if (cfg != NULL) {
        h->Config = *cfg;                   /* 调用者的配置完全覆盖默认值      */
    }

    h->State = RS485_STATE_RESET;
    h->Bus   = RS485_BUS_LISTEN;
    Rs485_Fifo_Reset(&h->RxFifo);

    LOG_I("--- AXI UARTLite + DE GPIO (RS-485 half duplex) ---\r\n");

    status = Rs485_Hal_Init(h);
    if (status != RS485_OK) {
        h->State = RS485_STATE_ERROR;
        return status;
    }

    h->State = RS485_STATE_READY;

    return RS485_OK;
}

Rs485_StatusTypeDef Rs485_Start(Rs485_HandleTypeDef *h)
{
    Rs485_StatusTypeDef status;

    if (h == NULL) {
        return RS485_ERR_PARAM;
    }

    if (h->State != RS485_STATE_READY) {
        LOG_I("Rs485_Start() called in state %s\r\n", Rs485_StateName(h->State));
        return RS485_ERR_STATE;
    }

    if (h->Config.RxMode == RS485_RX_POLLED) {
        LOG_I("polled receive: call Rs485_Poll() often enough "
                   "(the UART FIFO holds only 16 bytes)\r\n");
        return RS485_OK;
    }

    status = Rs485_Hal_ConnectRxInterrupt(h, Rs485_RxIsr, h);
    if (status != RS485_OK) {
        /*
         * GIC 很可能是在这颗 UART 之后才初始化的(见 rs485.h)。轮询收还能用, 所以
         * 这里**降级而不是失败**: 句柄保持 READY, 由 Rs485_Poll() 收字节。
         */
        LOG_I("falling back to polled receive\r\n");
        h->Config.RxMode = RS485_RX_POLLED;
        return status;
    }

    LOG_I("receive interrupt connected, %u B software FIFO\r\n",
               (unsigned)RS485_RX_FIFO_SIZE);

    return RS485_OK;
}

void Rs485_Poll(Rs485_HandleTypeDef *h)
{
    uint8_t  chunk[RS485_RX_CHUNK_SIZE];
    uint32_t state;
    uint32_t len;

    if ((h == NULL) || (h->State != RS485_STATE_READY)) {
        return;
    }

    h->Stats.Polls++;

    /*
     * 兜底(也是轮询模式的正路)。中断正常跑的时候这里一般什么都捞不到(ISR 已经把硬件
     * FIFO 掏空了), 但万一中断没连上或者丢了, 端口还活着。
     *
     * 临界区保证软件 FIFO 只有一个生产者: 不然 ISR 可能插在两次 Rs485_Fifo_Put()
     * 之间, 把写下标搞坏。
     */
    if (Rs485_Hal_IsRxDataAvailable(h) != 0U) {
        state = Rs485_Hal_EnterCritical();
        (void)Rs485_RxPump(h);
        Rs485_Hal_ExitCritical(state);
    }

    Rs485_AccountErrors(h);

    if ((h->OnRxData != NULL) && (Rs485_Fifo_Available(&h->RxFifo) != 0U)) {
        while ((len = Rs485_Fifo_Read(&h->RxFifo, chunk, sizeof(chunk))) != 0U) {
            h->OnRxData(h->UserContext, chunk, (uint16_t)len);
        }
    }
}

void Rs485_DeInit(Rs485_HandleTypeDef *h)
{
    if (h == NULL) {
        return;
    }

    Rs485_Hal_DeInit(h);
    Rs485_Fifo_Reset(&h->RxFifo);

    h->State = RS485_STATE_RESET;
    h->Bus   = RS485_BUS_LISTEN;
}

/* ========================================================================== */
/*  发送                                                                       */
/* ========================================================================== */

Rs485_StatusTypeDef Rs485_Send(Rs485_HandleTypeDef *h, const uint8_t *data, uint16_t len)
{
    Rs485_StatusTypeDef status = RS485_OK;
    uint32_t            written = 0U;
    uint32_t            charTimeUs;
    uint32_t            guardUs;
    uint32_t            i;

    if ((h == NULL) || (data == NULL) || (len == 0U)) {
        return RS485_ERR_PARAM;
    }

    if (h->State != RS485_STATE_READY) {
        return RS485_ERR_STATE;
    }

    if (h->Bus == RS485_BUS_TRANSMIT) {
        return RS485_ERR_STATE;             /* 已经在驱动总线了                */
    }

    /* ---- 1. 接管总线 ----------------------------------------------------- */
    h->Bus = RS485_BUS_TRANSMIT;
    Rs485_Hal_SetTransmit(h, 1U);
    Rs485_Hal_DelayUs(h, RS485_DE_SETTLE_US);

    /* ---- 2. 推字节 ------------------------------------------------------- */
    for (i = 0U; i < (uint32_t)len; i++) {
        status = Rs485_WaitTxRoom(h);
        if (status != RS485_OK) {
            break;
        }

        Rs485_Hal_WriteByte(h, data[i]);
        written++;
    }

    /* ---- 3. 等 FIFO 排空 ------------------------------------------------- */
    if (status == RS485_OK) {
        status = Rs485_WaitTxDrained(h);
    }

    /* ---- 4. 释放总线 ----------------------------------------------------
     * AXI UART Lite 在最后一个字符还留在移位寄存器里时就报 FIFO 空, DE 必须**多保持
     * 至少一个字符时间**, 否则帧尾会被切掉 —— 就是 RS-485 那个经典的"最后一个字节
     * 总是坏的"。
     */
    charTimeUs = Rs485_Hal_CharTimeUs(h);
    guardUs    = charTimeUs * ((h->Config.TxTailGuardChars != 0U) ? h->Config.TxTailGuardChars : 1U);

    Rs485_Hal_DelayUs(h, guardUs);
    Rs485_Hal_SetTransmit(h, 0U);

    h->Bus = RS485_BUS_LISTEN;

    /* ---- 5. 记账 --------------------------------------------------------- */
    h->Stats.TxBytes += written;

    if (status == RS485_OK) {
        h->Stats.TxTransfers++;
    } else {
        h->Stats.TxTimeouts++;
    }

    return status;
}

/* ========================================================================== */
/*  接收                                                                       */
/* ========================================================================== */

void Rs485_SetRxHook(Rs485_HandleTypeDef *h, Rs485_RxHook hook, void *context)
{
    if (h == NULL) {
        return;
    }

    h->OnRxData    = hook;
    h->UserContext = context;
}

void Rs485_SetRxWakeup(Rs485_HandleTypeDef *h, Rs485_WakeupFn fn, void *context)
{
    if (h == NULL) {
        return;
    }

    h->RxWakeup        = fn;
    h->RxWakeupContext = context;
}

/* ========================================================================== */
/*  查询                                                                       */
/* ========================================================================== */

Rs485_BusStateTypeDef Rs485_GetBusState(const Rs485_HandleTypeDef *h)
{
    if (h == NULL) {
        return RS485_BUS_LISTEN;
    }

    return h->Bus;
}

uint32_t Rs485_GetIdleTimeUs(const Rs485_HandleTypeDef *h)
{
    if (h == NULL) {
        return UINT32_MAX;
    }

    return Rs485_Hal_ElapsedUs(h, h->LastRxTimestamp);
}

Rs485_StatusTypeDef Rs485_GetStats(const Rs485_HandleTypeDef *h, Rs485_StatsTypeDef *stats)
{
    if ((h == NULL) || (stats == NULL)) {
        return RS485_ERR_PARAM;
    }

    *stats = h->Stats;

    return RS485_OK;
}

Rs485_StatusTypeDef Rs485_ForceDirection(Rs485_HandleTypeDef *h, uint8_t transmit)
{
    if (h == NULL) {
        return RS485_ERR_PARAM;
    }

    if (h->State != RS485_STATE_READY) {
        return RS485_ERR_STATE;
    }

    Rs485_Hal_SetTransmit(h, (transmit != 0U) ? 1U : 0U);

    h->Bus = (transmit != 0U) ? RS485_BUS_TRANSMIT : RS485_BUS_LISTEN;

    return RS485_OK;
}

const char *Rs485_StateName(Rs485_StateTypeDef state)
{
    switch (state) {
        case RS485_STATE_RESET: return "reset";
        case RS485_STATE_READY: return "ready";
        case RS485_STATE_ERROR: return "error";
        default:                return "?";
    }
}

void Rs485_PrintDirection(Rs485_HandleTypeDef *h)
{
    Rs485_DeInfoTypeDef info;

    if (h == NULL) {
        return;
    }

    if (Rs485_GetDirectionInfo(h, &info) != RS485_OK) {
        return;
    }

    LOG_I("DE %s (active-%s) pin=%u | GPIO: data 0x%08X tri 0x%08X | "
               "transitions %u\r\n",
               (info.DeTransmit != 0U) ? "TRANSMIT" : "listen",
               (info.DeActiveHigh != 0U) ? "high" : "low",
               (unsigned)info.DeLevel,
               (unsigned)info.GpioData,
               (unsigned)info.GpioTri,
               (unsigned)info.DeWrites);
}

Rs485_StatusTypeDef Rs485_SetDeActiveHigh(Rs485_HandleTypeDef *h, uint8_t activeHigh)
{
    if (h == NULL) {
        return RS485_ERR_PARAM;
    }

    if (h->State != RS485_STATE_READY) {
        return RS485_ERR_STATE;
    }

    Rs485_Hal_SetDeActiveHigh(h, activeHigh);

    return RS485_OK;
}

Rs485_StatusTypeDef Rs485_GetDirectionInfo(Rs485_HandleTypeDef *h,
                                           Rs485_DeInfoTypeDef *info)
{
    if (h == NULL) {
        return RS485_ERR_PARAM;
    }

    return Rs485_Hal_GetDeInfo(h, info);
}
