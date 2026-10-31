/**
 *******************************************************************************
 * @file    rs485_hal.c
 * @brief   RS-485 模块的硬件抽象层实现
 *
 * 跟 AXI UARTLite、驱动收发器方向的 AXI GPIO、以及 DE 计时用的 Cortex-A9 全局
 * 定时器打交道。两个平台注意事项见 rs485_hal.h(SCU 定时器的归属、TX FIFO 空的含义)。
 *******************************************************************************
 */

#include "rs485_hal.h"

#include <stddef.h>
#include <string.h>

#include "osal/osal.h"          /* 中断连接/开关: bsp 不直接依赖 RTOS          */

#include "xparameters.h"
#include "xuartlite_l.h"        /* 寄存器级 API 和状态位                        */
#include "xil_io.h"
/* 日志标签: 本文件的 LOG_I/W/E 会带 [rs485] 前缀 */
#define LOG_ENABLE 0   /* 本模块日志默认关闭; 需要时改成 1 */
#define LOG_TAG "rs485"
#include "service/monitor_module/log/log.h"

#include "xil_printf.h"
#include "xinterrupt_wrap.h"    /* XSetupInterruptSystem / XDisconnect...       */
#include "xcortexa9_config.h"   /* XGet_CpuFreq()                               */

/* ========================================================================== */
/*  平台常量                                                                   */
/* ========================================================================== */

/** Cortex-A9 MPCore 全局定时器(自由运行, 时钟 = CPU 时钟 / 2)。 */
#define RS485_GLOBAL_TIMER_BASE     0xF8F00200U
#define RS485_GLOBAL_TIMER_COUNT    0x00000000U     /**< 低 32 位              */

/** CPSR 的中断屏蔽位(I)。 */
#define RS485_CPSR_IRQ_MASK         0x80U

/** 连读几次运行中的计数器必须不一样; 这个上限用来兜住探测循环。 */
#define RS485_TIMER_PROBE_LIMIT     1000U

/** 按周期数延时那个循环里, 每次迭代至少执行的 NOP 数。 */
#define RS485_DELAY_NOPS_PER_LOOP   4U

/* ========================================================================== */
/*  内部函数(static)                                                             */
/* ========================================================================== */

/**
 * @brief  从其 XUartLite 成员的地址反推出所属的句柄。
 *
 * XSetupInterruptSystem() 会把给它的"驱动实例"原样当成回调引用传回来。注册时给的是
 * &handle->Uart(最自然的写法), 所以 ISR 收到那个地址, 句柄离它只有一个固定偏移。
 */
#define RS485_HANDLE_FROM_UART(instancePtr)                              \
    ((Rs485_HandleTypeDef *)((uint8_t *)(instancePtr) -                  \
                             offsetof(Rs485_HandleTypeDef, Uart)))

/** @brief  读自由运行的全局定时器计数器。 */
static uint32_t Rs485_Hal_TimerCount(void)
{
    return Xil_In32(RS485_GLOBAL_TIMER_BASE + RS485_GLOBAL_TIMER_COUNT);
}

/** @brief  探一下全局定时器是不是在跑。 @return 1 = 两次连读不同。 */
static uint8_t Rs485_Hal_ProbeTimer(void)
{
    uint32_t first = Rs485_Hal_TimerCount();
    uint32_t spins = 0U;

    while ((Rs485_Hal_TimerCount() == first) && (spins < RS485_TIMER_PROBE_LIMIT)) {
        spins++;
    }

    return (spins < RS485_TIMER_PROBE_LIMIT) ? 1U : 0U;
}

/**
 * @brief  按 CPU 周期数空转延时。
 *
 * 只在全局定时器用不了时兜底。每次迭代至少烧掉 RS485_DELAY_NOPS_PER_LOOP 个周期,
 * 所以实际延时只会比要的长, 不会短。
 */
static void Rs485_Hal_DelayCycles(uint32_t cycles)
{
    while (cycles > RS485_DELAY_NOPS_PER_LOOP) {
        __asm__ volatile ("nop; nop; nop; nop");
        cycles -= RS485_DELAY_NOPS_PER_LOOP;
    }
}

/**
 * @brief  UART 中断入口。
 * @param  callbackRef : 就是 &handle->Uart(XSetupInterruptSystem 传回来的那个)。
 *
 * AXI UARTLite 在发送 FIFO 排空时同样会拉中断, 所以动手之前先看状态寄存器。IP 的
 * 错误位是**粘滞**的(RX FIFO 复位前一直为 1), 所以这里只是把它锁存下来, 由服务层
 * 稍后取走。
 */
static void Rs485_Hal_Isr(void *callbackRef)
{
    Rs485_HandleTypeDef *h;
    uint32_t status;

    if (callbackRef == NULL) {
        return;
    }

    h = RS485_HANDLE_FROM_UART(callbackRef);

    if (h->Hal.Ready == 0U) {
        return;
    }

    status = (uint32_t)XUartLite_GetStatusReg(h->Uart.RegBaseAddress);

    if ((status & XUL_SR_OVERRUN_ERROR) != 0U) {
        h->Hal.ErrorFlags |= RS485_HAL_ERR_OVERRUN;
    }
    if ((status & XUL_SR_FRAMING_ERROR) != 0U) {
        h->Hal.ErrorFlags |= RS485_HAL_ERR_FRAME;
    }
    if ((status & XUL_SR_PARITY_ERROR) != 0U) {
        h->Hal.ErrorFlags |= RS485_HAL_ERR_PARITY;
    }

    if (((status & XUL_SR_RX_FIFO_VALID_DATA) != 0U) && (h->Hal.RxHandler != NULL)) {
        h->Hal.RxHandler(h->Hal.RxContext);
    }
}

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

Rs485_StatusTypeDef Rs485_Hal_Init(Rs485_HandleTypeDef *h)
{
    XUartLite_Config *uartConfig;
    XGpio_Config     *gpioConfig;
    uint32_t          cpuFreq;

    if (h == NULL) {
        return RS485_ERR_PARAM;
    }

    (void)memset(&h->Hal, 0, sizeof(h->Hal));

    /* ---- 收发器方向脚 ---------------------------------------------------- */
    gpioConfig = XGpio_LookupConfig(RS485_GPIO_BASEADDR);
    if (gpioConfig == NULL) {
        LOG_I("GPIO not found at 0x%08X\r\n", RS485_GPIO_BASEADDR);
        return RS485_ERR_HW;
    }

    if (XGpio_CfgInitialize(&h->Gpio, gpioConfig, gpioConfig->BaseAddress) != XST_SUCCESS) {
        LOG_I("GPIO initialisation failed\r\n");
        return RS485_ERR_HW;
    }

    /* 极性先定: 下面的 SetTransmit() 要用它 */
    h->Hal.DeActiveHigh = (RS485_DE_TRANSMIT_LEVEL != 0U) ? 1U : 0U;

    /*
     * 本设计的 AXI GPIO 是 C_ALL_OUTPUTS = 1 生成的, 方向被固定成输出: 下面这句对
     * 普通 GPIO 是对的, 在这里是空操作(GPIO_TRI 读回来是参数默认值 0xFFFFFFFF, 写不进去)。
     */
    XGpio_SetDataDirection(&h->Gpio, RS485_GPIO_CHANNEL, 0x00000000U);

    /* ---- 串口 ------------------------------------------------------------ */
    uartConfig = XUartLite_LookupConfig(RS485_UART_BASEADDR);
    if (uartConfig == NULL) {
        LOG_I("UART not found at 0x%08X\r\n", RS485_UART_BASEADDR);
        return RS485_ERR_HW;
    }

    if (XUartLite_CfgInitialize(&h->Uart, uartConfig, uartConfig->RegBaseAddr) != XST_SUCCESS) {
        LOG_I("UART initialisation failed\r\n");
        return RS485_ERR_HW;
    }

    XUartLite_DisableInterrupt(&h->Uart);   /* 中断源一开始是关的              */
    XUartLite_ResetFifos(&h->Uart);

    /* ---- 计时 ------------------------------------------------------------ */
    cpuFreq = XGet_CpuFreq();

    h->Hal.CyclesPerUs  = cpuFreq / 1000000U;
    h->Hal.CountsPerUs  = (cpuFreq / 2U) / 1000000U;   /* 全局定时器的时钟      */
    h->Hal.TimerAvailable = Rs485_Hal_ProbeTimer();

    if (h->Hal.CountsPerUs == 0U) {
        h->Hal.TimerAvailable = 0U;
    }

    h->Hal.Ready = 1U;

    /*
     * 到这一步才放开总线: SetTransmit() 在 Ready 为 0 时拒绝碰 GPIO, 早调的话方向
     * 状态会停在"发送" —— 于是第一次 Rs485_Send() 会跳过拉 DE, 把整帧发给一个正在
     * 接收的收发器, 帧就这么无声无息地丢了。
     */
    Rs485_Hal_SetTransmit(h, 0U);

    LOG_I("UARTLite 0x%08X @ %u baud, DE = GPIO 0x%08X ch %u bit 0, "
               "active-%s, %s timing\r\n",
               (unsigned)RS485_UART_BASEADDR,
               (unsigned)h->Config.BaudRate,
               (unsigned)RS485_GPIO_BASEADDR,
               (unsigned)RS485_GPIO_CHANNEL,
               (h->Hal.DeActiveHigh != 0U) ? "high" : "low",
               (h->Hal.TimerAvailable != 0U) ? "global timer" : "cycle counter");

    /*
     * 把 GPIO 回读一遍, 让控制台能看出方向那次写有没有生效。注意 "全输出" 的 AXI GPIO
     * 的 GPIO_TRI 不可写、读回来是参数默认值 0xFFFFFFFF: 这块板子只看 data 的 bit0。
     */
    {
        Rs485_DeInfoTypeDef info;

        if (Rs485_Hal_GetDeInfo(h, &info) == RS485_OK) {
            LOG_I("GPIO 0x%08X: data 0x%08X tri 0x%08X | DE level %u, "
                       "%u transitions\r\n",
                       (unsigned)h->Gpio.BaseAddress,
                       (unsigned)info.GpioData,
                       (unsigned)info.GpioTri,
                       (unsigned)info.DeLevel,
                       (unsigned)info.DeWrites);
        }
    }

    return RS485_OK;
}

void Rs485_Hal_DeInit(Rs485_HandleTypeDef *h)
{
    if (h == NULL) {
        return;
    }

    Rs485_Hal_DisconnectRxInterrupt(h);

    if (h->Hal.Ready != 0U) {
        Rs485_Hal_SetTransmit(h, 0U);       /* 绝不把总线留在驱动状态          */
        XUartLite_ResetFifos(&h->Uart);
    }

    h->Hal.Ready = 0U;
}

/* ========================================================================== */
/*  中断                                                                       */
/* ========================================================================== */

Rs485_StatusTypeDef Rs485_Hal_ConnectRxInterrupt(Rs485_HandleTypeDef *h,
                                                 Rs485_Hal_RxHandlerTypeDef handler,
                                                 void *context)
{
    XUartLite_Config *uartConfig;

    if ((h == NULL) || (handler == NULL) || (h->Hal.Ready == 0U)) {
        return RS485_ERR_PARAM;
    }

    if (h->Hal.IrqConnected != 0U) {
        return RS485_OK;                    /* 已经连过了                      */
    }

    uartConfig = XUartLite_LookupConfig(RS485_UART_BASEADDR);
    if (uartConfig == NULL) {
        return RS485_ERR_HW;
    }

    h->Hal.RxHandler = handler;
    h->Hal.RxContext = context;

    /*
     * 连中断有两条路, 由 RS485_USE_RTOS_IRQ 选:
     *
     *   RTOS(Osal_InterruptConnect): OSAL 用 RTOS 端口装中断, 一步做完两件事 ——
     *     初始化 GIC, 并把处理器登记到端口读的那张表里。优先级被设成"最低可用",
     *     这样在中断里调 RTOS 的 FromISR 接口才合法(我们的 ISR 回调要用它叫醒任务)。
     *     好处是 bsp 不用 include FreeRTOS, 只依赖 osal/。
     *
     *   裸机(XSetupInterruptSystem): lwIP 端口提供的包装, 只在没有 RTOS 的工程用。
     */
#if (RS485_USE_RTOS_IRQ != 0U)
    /* 回调引用仍然是 &handle->Uart, 见 RS485_HANDLE_FROM_UART. */
    if (Osal_InterruptConnect((uint32_t)uartConfig->IntrId,
                              (Osal_IsrHandlerTypeDef)Rs485_Hal_Isr,
                              (void *)&h->Uart) != OSAL_OK) {
        LOG_I("failed to connect the UART interrupt\r\n");
        h->Hal.RxHandler = NULL;
        h->Hal.RxContext = NULL;
        return RS485_ERR_NO_IRQ;
    }

    Osal_InterruptEnable((uint32_t)uartConfig->IntrId);
#else
    if (XSetupInterruptSystem(&h->Uart, (void *)Rs485_Hal_Isr,
                              uartConfig->IntrId, uartConfig->IntrParent,
                              RS485_IRQ_PRIORITY) != XST_SUCCESS) {
        LOG_I("failed to connect the UART interrupt\r\n");
        h->Hal.RxHandler = NULL;
        h->Hal.RxContext = NULL;
        return RS485_ERR_NO_IRQ;
    }
#endif

    h->Hal.IrqConnected = 1U;
    XUartLite_EnableInterrupt(&h->Uart);

    return RS485_OK;
}

void Rs485_Hal_DisconnectRxInterrupt(Rs485_HandleTypeDef *h)
{
    XUartLite_Config *uartConfig;

    if ((h == NULL) || (h->Hal.IrqConnected == 0U)) {
        return;
    }

    XUartLite_DisableInterrupt(&h->Uart);

    /* 关中断源 + 从 GIC 上摘掉处理器。先查配置, 再用它里面的 IntrId。 */
    uartConfig = XUartLite_LookupConfig(RS485_UART_BASEADDR);
    if (uartConfig != NULL) {
#if (RS485_USE_RTOS_IRQ != 0U)
        Osal_InterruptDisable((uint32_t)uartConfig->IntrId);
#endif
        (void)XDisconnectInterruptCntrl(uartConfig->IntrId, uartConfig->IntrParent);
    }

    h->Hal.IrqConnected = 0U;
    h->Hal.RxHandler    = NULL;
    h->Hal.RxContext    = NULL;
}

/* ========================================================================== */
/*  收发器方向                                                                 */
/* ========================================================================== */

void Rs485_Hal_SetTransmit(Rs485_HandleTypeDef *h, uint8_t enable)
{
    uint8_t transmit = (enable != 0U) ? 1U : 0U;
    uint32_t level;

    if ((h == NULL) || (h->Hal.Ready == 0U)) {
        return;
    }

    if (transmit == h->Hal.DeTransmit) {
        return;                             /* 没变化, 不用写                  */
    }

    /* 高有效接法: 发送时拉高、接收时拉低; 低有效(RS485_DE_TRANSMIT_LEVEL == 0)反过来 */
    if (h->Hal.DeActiveHigh != 0U) {
        level = (transmit != 0U) ? RS485_GPIO_DE_MASK : 0x00000000U;
    } else {
        level = (transmit != 0U) ? 0x00000000U : RS485_GPIO_DE_MASK;
    }

    XGpio_DiscreteWrite(&h->Gpio, RS485_GPIO_CHANNEL, level);

    h->Hal.DeTransmit = transmit;
    h->Hal.DeWrites++;
}

void Rs485_Hal_SetDeActiveHigh(Rs485_HandleTypeDef *h, uint8_t activeHigh)
{
    uint8_t transmit;

    if ((h == NULL) || (h->Hal.Ready == 0U)) {
        return;
    }

    activeHigh = (activeHigh != 0U) ? 1U : 0U;

    if (activeHigh == h->Hal.DeActiveHigh) {
        return;
    }

    /* 方向不变, 只是按新映射把脚重写一遍 */
    transmit = h->Hal.DeTransmit;
    h->Hal.DeActiveHigh = activeHigh;
    h->Hal.DeTransmit   = (uint8_t)(transmit ^ 1U);     /* 逼它重写            */

    Rs485_Hal_SetTransmit(h, transmit);
}

Rs485_StatusTypeDef Rs485_Hal_GetDeInfo(Rs485_HandleTypeDef *h,
                                        Rs485_DeInfoTypeDef *info)
{
    uint32_t data;
    uint32_t tri;

    if ((h == NULL) || (info == NULL)) {
        return RS485_ERR_PARAM;
    }

    data = XGpio_DiscreteRead(&h->Gpio, RS485_GPIO_CHANNEL);
    tri  = XGpio_GetDataDirection(&h->Gpio, RS485_GPIO_CHANNEL);

    info->DeTransmit   = h->Hal.DeTransmit;
    info->DeActiveHigh = h->Hal.DeActiveHigh;
    info->DeLevel      = (uint8_t)((data & RS485_GPIO_DE_MASK) != 0U);
    info->GpioData     = data;
    info->GpioTri      = tri;
    info->DeWrites     = h->Hal.DeWrites;

    return RS485_OK;
}

/* ========================================================================== */
/*  数据通路                                                                   */
/* ========================================================================== */

uint8_t Rs485_Hal_IsTxFull(const Rs485_HandleTypeDef *h)
{
    if (h == NULL) {
        return 1U;
    }

    return (uint8_t)(XUartLite_IsTransmitFull(h->Uart.RegBaseAddress) != 0);
}

uint8_t Rs485_Hal_IsTxEmpty(const Rs485_HandleTypeDef *h)
{
    if (h == NULL) {
        return 0U;
    }

    return ((XUartLite_GetStatusReg(h->Uart.RegBaseAddress) & XUL_SR_TX_FIFO_EMPTY) != 0U) ? 1U : 0U;
}

void Rs485_Hal_WriteByte(Rs485_HandleTypeDef *h, uint8_t byte)
{
    if (h == NULL) {
        return;
    }

    XUartLite_SendByte(h->Uart.RegBaseAddress, byte);
}

uint8_t Rs485_Hal_IsRxDataAvailable(const Rs485_HandleTypeDef *h)
{
    if (h == NULL) {
        return 0U;
    }

    return (uint8_t)(XUartLite_IsReceiveEmpty(h->Uart.RegBaseAddress) == 0);
}

uint8_t Rs485_Hal_ReadByte(Rs485_HandleTypeDef *h)
{
    if (h == NULL) {
        return 0U;
    }

    return (uint8_t)XUartLite_RecvByte(h->Uart.RegBaseAddress);
}

uint32_t Rs485_Hal_TakeErrorFlags(Rs485_HandleTypeDef *h)
{
    uint32_t flags;

    if (h == NULL) {
        return 0U;
    }

    flags = h->Hal.ErrorFlags;
    h->Hal.ErrorFlags = 0U;

    return flags;
}

/* ========================================================================== */
/*  计时与临界区                                                               */
/* ========================================================================== */

uint32_t Rs485_Hal_EnterCritical(void)
{
    uint32_t cpsr;

    __asm__ volatile ("mrs %0, cpsr" : "=r" (cpsr));
    __asm__ volatile ("cpsid i" ::: "memory");
    /* 确认屏蔽已经生效, 调用者才去碰共享状态 */
    __asm__ volatile ("isb" ::: "memory");

    return cpsr;
}

void Rs485_Hal_ExitCritical(uint32_t state)
{
    if ((state & RS485_CPSR_IRQ_MASK) == 0U) {
        __asm__ volatile ("cpsie i" ::: "memory");
    }
}

void Rs485_Hal_DelayUs(const Rs485_HandleTypeDef *h, uint32_t us)
{
    uint32_t start;
    uint32_t wait;

    if ((h == NULL) || (us == 0U)) {
        return;
    }

    if ((h->Hal.TimerAvailable != 0U) && (h->Hal.CountsPerUs != 0U)) {
        start = Rs485_Hal_TimerCount();
        wait  = us * h->Hal.CountsPerUs;

        while ((Rs485_Hal_TimerCount() - start) < wait) {
            /* 空转: 全局定时器跑在 CPU/2, 分辨率约 3 ns */
        }
    } else {
        Rs485_Hal_DelayCycles(us * h->Hal.CyclesPerUs);
    }
}

uint32_t Rs485_Hal_GetTimestamp(const Rs485_HandleTypeDef *h)
{
    if ((h == NULL) || (h->Hal.TimerAvailable == 0U) || (h->Hal.CountsPerUs == 0U)) {
        return 0U;
    }

    return Rs485_Hal_TimerCount() / h->Hal.CountsPerUs;
}

uint32_t Rs485_Hal_ElapsedUs(const Rs485_HandleTypeDef *h, uint32_t since)
{
    if ((h == NULL) || (since == 0U) || (h->Hal.TimerAvailable == 0U)) {
        return UINT32_MAX;                  /* 没时间基准 / 还没收到过东西     */
    }

    return Rs485_Hal_GetTimestamp(h) - since;
}

uint32_t Rs485_Hal_CharTimeUs(const Rs485_HandleTypeDef *h)
{
    uint32_t baud;

    if ((h == NULL) || (h->Config.BaudRate == 0U)) {
        return 0U;
    }

    baud = h->Config.BaudRate;

    /* 8N1 = 1 起始 + 8 数据 + 1 停止 = 10 个位周期, 向上取整 */
    return ((10U * 1000000U) + baud - 1U) / baud;
}
