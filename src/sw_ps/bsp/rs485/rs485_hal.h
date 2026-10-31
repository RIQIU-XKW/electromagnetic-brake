/**
 *******************************************************************************
 * @file    rs485_hal.h
 * @brief   RS-485 模块的硬件抽象层接口
 *
 * 本模块只有这一层碰 Xilinx 驱动(XUartLite / XGpio)和中断控制器; 上面的 rs485.c
 * 只说"字节 / 方向 / 时间戳"。
 *
 * 两个平台事实:
 *   1. SCU 私有定时器归 lwIP tick 所有, xiltimer 也挂在同一个上 —— 所以这里不能
 *      用 usleep()/XTime_GetTime(); HAL 用的是 Cortex-A9 全局定时器(没人占)。
 *   2. AXI UARTLite 在最后一个字节还在移位时就报"TX FIFO 空", 所以发送方要多保持
 *      一个字符时间的收发器使能(见 RS485_TX_TAIL_GUARD_CHARS)。
 *******************************************************************************
 */

#ifndef RS485_HAL_H
#define RS485_HAL_H

#include "rs485_types.h"

/* -------------------------------------------------------------------------- */
/* Rs485_Hal_TakeErrorFlags() 报的错误位                                       */
/* -------------------------------------------------------------------------- */

#define RS485_HAL_ERR_OVERRUN   0x01U   /**< IP 里的 RX FIFO 溢出            */
#define RS485_HAL_ERR_FRAME     0x02U   /**< 帧错误(停止位不对)              */
#define RS485_HAL_ERR_PARITY    0x04U   /**< 校验错误                        */

/* -------------------------------------------------------------------------- */
/* 生命周期                                                                   */
/* -------------------------------------------------------------------------- */

/** @brief  初始化 UART、方向 GPIO 和计时(Config 必须已经填好)。 */
Rs485_StatusTypeDef Rs485_Hal_Init(Rs485_HandleTypeDef *h);

/** @brief  释放外设和中断连接。 */
void Rs485_Hal_DeInit(Rs485_HandleTypeDef *h);

/* -------------------------------------------------------------------------- */
/* 中断                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief  注册服务 ISR 并接到中断控制器上。
 * @note   本应用第一个连中断的地方会初始化 GIC 并清空处理器表(lwIP 在 xemac_add()
 *         里做), 所以必须在 Enet_Init() 之后调用。
 */
Rs485_StatusTypeDef Rs485_Hal_ConnectRxInterrupt(Rs485_HandleTypeDef *h,
                                                 Rs485_Hal_RxHandlerTypeDef handler,
                                                 void *context);

/** @brief  关掉中断源, 并断开与中断控制器的连接。 */
void Rs485_Hal_DisconnectRxInterrupt(Rs485_HandleTypeDef *h);

/* -------------------------------------------------------------------------- */
/* 收发器方向                                                                 */
/* -------------------------------------------------------------------------- */

/** @brief  驱动收发器的 DE / !RE 脚(非 0 = 驱动总线, 0 = 接收)。 */
void Rs485_Hal_SetTransmit(Rs485_HandleTypeDef *h, uint8_t enable);

/** @brief  运行时选择 DE 极性(1 = 高电平为发送, 默认; 0 = 反相)。 */
void Rs485_Hal_SetDeActiveHigh(Rs485_HandleTypeDef *h, uint8_t activeHigh);

/** @brief  回读方向脚状态和 GPIO 寄存器。 */
Rs485_StatusTypeDef Rs485_Hal_GetDeInfo(Rs485_HandleTypeDef *h,
                                        Rs485_DeInfoTypeDef *info);

/* -------------------------------------------------------------------------- */
/* 数据通路                                                                   */
/* -------------------------------------------------------------------------- */

/** @brief  发送 FIFO 塞不下了(非 0 = 满)。 */
uint8_t Rs485_Hal_IsTxFull(const Rs485_HandleTypeDef *h);

/** @brief  发送 FIFO 空了(非 0 = 空)。 */
uint8_t Rs485_Hal_IsTxEmpty(const Rs485_HandleTypeDef *h);

/** @brief  往发送 FIFO 压一个字节(调用者先查 IsTxFull)。 */
void Rs485_Hal_WriteByte(Rs485_HandleTypeDef *h, uint8_t byte);

/** @brief  接收 FIFO 里有数据(非 0 = 有)。 */
uint8_t Rs485_Hal_IsRxDataAvailable(const Rs485_HandleTypeDef *h);

/** @brief  从接收 FIFO 取一个字节(调用者先查有没有)。 */
uint8_t Rs485_Hal_ReadByte(Rs485_HandleTypeDef *h);

/** @brief  读走并清掉上次调用之后锁存的错误标志; 返回 RS485_HAL_ERR_* 位掩码。 */
uint32_t Rs485_Hal_TakeErrorFlags(Rs485_HandleTypeDef *h);

/* -------------------------------------------------------------------------- */
/* 计时与临界区                                                               */
/* -------------------------------------------------------------------------- */

/** @brief  进临界区(关中断); 返回之前的中断屏蔽状态, 要原样传给 ExitCritical()。 */
uint32_t Rs485_Hal_EnterCritical(void);

/** @brief  出临界区(state = 对应的 EnterCritical() 返回值)。 */
void Rs485_Hal_ExitCritical(uint32_t state);

/** @brief  忙等若干微秒(时间常数取自句柄的配置)。 */
void Rs485_Hal_DelayUs(const Rs485_HandleTypeDef *h, uint32_t us);

/** @brief  自由运行的微秒时间戳(约 4295 s 回绕一次; 无时间基准返回 0)。 */
uint32_t Rs485_Hal_GetTimestamp(const Rs485_HandleTypeDef *h);

/** @brief  距某个时间戳过了多少微秒(回绕安全); since = 0 或无时间基准返回 UINT32_MAX。 */
uint32_t Rs485_Hal_ElapsedUs(const Rs485_HandleTypeDef *h, uint32_t since);

/** @brief  当前波特率下一个 8N1 字符的时间(微秒, 向上取整)。 */
uint32_t Rs485_Hal_CharTimeUs(const Rs485_HandleTypeDef *h);

#endif /* RS485_HAL_H */
