/**
 *******************************************************************************
 * @file    rs485_types.h
 * @brief   RS-485 模块的数据类型
 *
 * 和本工程其它模块一样是"句柄制": 调用者自己持有 Rs485_HandleTypeDef, 每次调用
 * 把地址传进来, 模块的全部状态都在这个结构体里。
 *******************************************************************************
 */

#ifndef RS485_TYPES_H
#define RS485_TYPES_H

#include <stdint.h>
#include <stddef.h>

#include "xuartlite.h"
#include "xgpio.h"

#include "rs485_cfg.h"
#include "rs485_fifo.h"

/* -------------------------------------------------------------------------- */
/* 返回码                                                                     */
/* -------------------------------------------------------------------------- */

typedef enum {
    RS485_OK            =  0,   /**< 成功                                   */
    RS485_ERR_PARAM     = -1,   /**< 参数为 NULL / 越界                     */
    RS485_ERR_STATE     = -2,   /**< 当前状态下不许这么调                   */
    RS485_ERR_HW        = -3,   /**< 外设没初始化成功                       */
    RS485_ERR_TIMEOUT   = -4,   /**< UART 迟迟不收或不发                    */
    RS485_ERR_OVERRUN   = -5,   /**< 软件 FIFO 满, 字节丢了                 */
    RS485_ERR_NO_IRQ    = -6    /**< 连中断失败                             */
} Rs485_StatusTypeDef;

/* -------------------------------------------------------------------------- */
/* 运行时状态                                                                 */
/* -------------------------------------------------------------------------- */

typedef enum {
    RS485_STATE_RESET = 0,      /**< 句柄清零, 还没初始化                   */
    RS485_STATE_READY,          /**< 外设就绪, 总线已交给接收               */
    RS485_STATE_ERROR           /**< 致命错误, 只剩 DeInit 可用             */
} Rs485_StateTypeDef;

/** 收发器此刻在干什么。 */
typedef enum {
    RS485_BUS_LISTEN = 0,       /**< DE 释放, 接收器工作                    */
    RS485_BUS_TRANSMIT          /**< DE 拉高, 正在驱动总线                  */
} Rs485_BusStateTypeDef;

/** 接收通路怎么服务。 */
typedef enum {
    RS485_RX_INTERRUPT = 0,     /**< 收中断负责填软件 FIFO                  */
    RS485_RX_POLLED             /**< Rs485_Poll() 负责填软件 FIFO           */
} Rs485_RxModeTypeDef;

/* -------------------------------------------------------------------------- */
/* 配置                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t            BaudRate;       /**< 必须和 IP 的配置一致            */
    Rs485_RxModeTypeDef RxMode;         /**< 中断收还是轮询收                */
    uint8_t             TxTailGuardChars;/**< 最后一个字节之后 DE 保持几个字符*/
    uint32_t            TxTimeoutUs;    /**< FIFO 握手超时                   */
} Rs485_ConfigTypeDef;

/* -------------------------------------------------------------------------- */
/* 运行时信息                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t TxBytes;           /**< 压进 UART 发送 FIFO 的字节数           */
    uint32_t TxTransfers;       /**< 完成的 Rs485_Send() 次数               */
    uint32_t TxTimeouts;        /**< 因超时中断的发送次数                   */
    uint32_t RxBytes;           /**< 从线上收下来的字节数                   */
    uint32_t RxBursts;          /**< 发现新数据的 Poll 次数                 */
    uint32_t RxFifoOverruns;    /**< 软件 FIFO 满导致的丢字节               */
    uint32_t RxHwOverruns;      /**< IP 报的溢出                            */
    uint32_t RxFrameErrors;     /**< IP 报的帧错误                          */
    uint32_t RxParityErrors;    /**< IP 报的校验错误                        */
    uint32_t Polls;             /**< Rs485_Poll() 被调用的次数              */
} Rs485_StatsTypeDef;

/**
 * @brief  应用层的接收钩子。
 * @param  context : 传给 Rs485_SetRxHook() 的指针。
 * @param  data    : 收到的字节, **只在本次调用期间有效**。
 *
 * 装了钩子之后由 Rs485_Poll() 消费软件 FIFO 并交给应用; 不装就应用自己从
 * FIFO 里读。
 */
typedef void (*Rs485_RxHook)(void *context, const uint8_t *data, uint16_t len);

/**
 * @brief  收中断里的"叫醒服务"。
 *
 * 模块不认识操作系统: 它只在中断里收到字节之后回调一次, "叫醒哪个任务"由调用者
 * 决定。本工程的实现里就两行 —— Osal_TaskNotifyGiveFromIsr() + Osal_IsrYield()
 * (见 osal/osal.h)。
 *
 * @note  在中断上下文里调用: 只允许用带 FromIsr 后缀的 OSAL 接口。
 */
typedef void (*Rs485_WakeupFn)(void *context);

/* -------------------------------------------------------------------------- */
/* 句柄                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief  服务层的接收中断入口。
 * @note   在中断上下文里调用: 只许把硬件 FIFO 里的字节搬进软件 FIFO。
 */
typedef void (*Rs485_Hal_RxHandlerTypeDef)(void *context);

/** @brief  HAL 自己那部分状态(rs485_hal.c)。 */
typedef struct {
    uint8_t  Ready;             /**< 外设已初始化                           */
    uint8_t  TimerAvailable;    /**< 全局定时器可用                         */
    uint8_t  IrqConnected;      /**< 收中断已接到 GIC                       */
    uint8_t  DeTransmit;        /**< 1 = 正在驱动总线                       */
    uint8_t  DeActiveHigh;      /**< 1 = DE 高电平为发送                    */
    uint32_t CountsPerUs;       /**< 全局定时器每微秒计数                   */
    uint32_t CyclesPerUs;       /**< CPU 每微秒周期数(兜底用)               */
    uint32_t ErrorFlags;        /**< 粘滞的 UART 错误标志                   */
    uint32_t DeWrites;          /**< DE 翻转次数(诊断用)                    */
    Rs485_Hal_RxHandlerTypeDef RxHandler;   /**< 服务 ISR 入口              */
    void    *RxContext;                     /**< RxHandler 的上下文         */
} Rs485_HalTypeDef;

/**
 * @brief  方向脚的诊断快照(从 AXI GPIO **回读**, 所以是硬件真实状态而不是软件
 *         以为的状态)。链路一声不响时用它证明"方向脚到底被配过没有"。
 */
typedef struct {
    uint8_t  DeTransmit;    /**< 1 = 驱动总线, 0 = 接收                     */
    uint8_t  DeActiveHigh;  /**< 当前极性                                   */
    uint8_t  DeLevel;       /**< 从 DE 脚回读到的电平                       */
    uint32_t GpioData;      /**< AXI GPIO 数据寄存器回读                    */
    uint32_t GpioTri;       /**< AXI GPIO 方向寄存器回读                    */
    uint32_t DeWrites;      /**< 初始化以来 DE 翻转次数                     */
} Rs485_DeInfoTypeDef;

/** @brief  一个 RS-485 口的全部状态。 */
typedef struct Rs485_Handle {
    Rs485_ConfigTypeDef  Config;                    /**< 调用者的配置         */
    Rs485_StatsTypeDef   Stats;                     /**< 统计计数             */
    volatile Rs485_StateTypeDef State;              /**< 生命周期状态         */
    volatile Rs485_BusStateTypeDef Bus;             /**< 收发器此刻的状态     */

    Rs485_HalTypeDef     Hal;                       /**< HAL 记账             */
    XUartLite            Uart;                      /**< AXI UARTLite 驱动    */
    XGpio                Gpio;                      /**< AXI GPIO 驱动(DE 脚) */
    Rs485_FifoTypeDef    RxFifo;                    /**< 软件接收 FIFO        */

    uint32_t             LastRxTimestamp;           /**< 0 = 还没收到过       */

    Rs485_RxHook         OnRxData;                  /**< 可选的接收钩子       */
    void                *UserContext;               /**< 钩子的上下文         */

    Rs485_WakeupFn       RxWakeup;                  /**< 中断里的叫醒回调     */
    void                *RxWakeupContext;           /**< 叫醒回调的上下文     */
} Rs485_HandleTypeDef;

#endif /* RS485_TYPES_H */
