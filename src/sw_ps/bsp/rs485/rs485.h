/**
 *******************************************************************************
 * @file    rs485.h
 * @brief   RS-485 模块对外接口(伞头文件)
 *
 * 组成(引脚见 rs485_cfg.h):
 *     rs485.c       半双工服务: DE 方向时序、统计、软件 FIFO 分发
 *     rs485_fifo.c  单生产者/单消费者字节 FIFO
 *     rs485_hal.c   AXI UARTLite + DE 的 AXI GPIO + GIC + 计时
 *
 * 用法:
 *     Rs485_Config_LoadDefaults(&cfg);      // 板级默认值
 *     Rs485_Init(&hrs485, &cfg);            // GPIO + UART + 计时
 *     Rs485_Start(&hrs485);                 // 连接收中断
 *     for (;;) { Rs485_Poll(&hrs485); }     // 不阻塞
 *
 * 中断顺序: 第一个连中断的地方会初始化 GIC 并清掉处理器表, 本工程里是 lwIP 在
 *   xemac_add() 里触发的, 所以 Rs485_Start() 必须排在 Enet_Init() 之后。
 *   顺序满足不了就打开 RS485_RX_POLLED(那个模式不需要中断)。
 *
 * 不仲裁总线(那是协议层的事, 见 service/Mover_module 的 Modbus RTU)。
 * Rs485_Send() 会阻塞到整帧发完并把收发器交回接收态。
 *******************************************************************************
 */

#ifndef RS485_H
#define RS485_H

#include "rs485_cfg.h"
#include "rs485_types.h"
#include "rs485_fifo.h"
#include "rs485_hal.h"

/* -------------------------------------------------------------------------- */
/* 配置                                                                       */
/* -------------------------------------------------------------------------- */

/** @brief  用板级默认值填一个配置结构(cfg 不许为 NULL)。 */
void Rs485_Config_LoadDefaults(Rs485_ConfigTypeDef *cfg);

/* -------------------------------------------------------------------------- */
/* 生命周期                                                                   */
/* -------------------------------------------------------------------------- */

/** @brief  初始化方向 GPIO、UART 和计时(cfg = NULL 用默认值)。 */
Rs485_StatusTypeDef Rs485_Init(Rs485_HandleTypeDef *h, const Rs485_ConfigTypeDef *cfg);

/**
 * @brief  启动: 中断模式下在这里连接收中断。
 * @note   连中断失败会自动退化成 RS485_RX_POLLED, 句柄仍可用。
 */
Rs485_StatusTypeDef Rs485_Start(Rs485_HandleTypeDef *h);

/** @brief  周期服务: 把收到的字节补进软件 FIFO 再分发出去; 非阻塞。 */
void Rs485_Poll(Rs485_HandleTypeDef *h);

/** @brief  停端口: 断开中断并放开外设(Init 的对称操作)。 */
void Rs485_DeInit(Rs485_HandleTypeDef *h);

/* -------------------------------------------------------------------------- */
/* 发送                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief  发一帧(阻塞, 半双工)。
 * @note   时序: 拉 DE -> 等收发器稳定 -> 按 UART FIFO 握手推字节 -> 等 FIFO 空 ->
 *         再保持 DE 一段尾部保护(最后一个字节还在移位输出) -> 释放 DE。
 */
Rs485_StatusTypeDef Rs485_Send(Rs485_HandleTypeDef *h, const uint8_t *data, uint16_t len);

/* -------------------------------------------------------------------------- */
/* 接收                                                                       */
/* -------------------------------------------------------------------------- */

/** @brief  装接收钩子; 之后 Rs485_Poll() 按最多 RS485_RX_CHUNK_SIZE 一块交给应用。 */
void Rs485_SetRxHook(Rs485_HandleTypeDef *h, Rs485_RxHook hook, void *context);

/**
 * @brief  装"收到字节就叫醒我"的回调(在中断上下文里调)。
 * @note   本模块不认识操作系统, 怎么叫醒由调用者决定(本工程走 OSAL 的任务通知)。
 *         中断里只做"叫醒任务", 不要碰总线。
 */
void Rs485_SetRxWakeup(Rs485_HandleTypeDef *h, Rs485_WakeupFn fn, void *context);

/* -------------------------------------------------------------------------- */
/* 查询                                                                       */
/* -------------------------------------------------------------------------- */

/** @brief  收发器此刻在干什么。 */
Rs485_BusStateTypeDef Rs485_GetBusState(const Rs485_HandleTypeDef *h);

/** @brief  距上一个收到字节过了多少微秒(没收到过 / 无时间基准返回 UINT32_MAX)。 */
uint32_t Rs485_GetIdleTimeUs(const Rs485_HandleTypeDef *h);

/** @brief  把统计计数拷进调用者的缓冲区。 */
Rs485_StatusTypeDef Rs485_GetStats(const Rs485_HandleTypeDef *h, Rs485_StatsTypeDef *stats);

/** @brief  打印 DE 方向诊断(GPIO 寄存器回读 + 极性 + DE 翻转次数)。 */
void Rs485_PrintDirection(Rs485_HandleTypeDef *h);

/** @brief  运行时改 DE 极性(1 = 高电平发送, 0 = 反相)。 */
Rs485_StatusTypeDef Rs485_SetDeActiveHigh(Rs485_HandleTypeDef *h, uint8_t activeHigh);

/** @brief  回读方向脚状态和 GPIO 寄存器。 */
Rs485_StatusTypeDef Rs485_GetDirectionInfo(Rs485_HandleTypeDef *h,
                                           Rs485_DeInfoTypeDef *info);

/** @brief  强制收发器方向(非 0 = 一直驱动总线, 0 = 一直听), 给硬件 bring-up 用。 */
Rs485_StatusTypeDef Rs485_ForceDirection(Rs485_HandleTypeDef *h, uint8_t transmit);

/** @brief  生命周期状态的字符串名(日志用)。 */
const char *Rs485_StateName(Rs485_StateTypeDef state);

#endif /* RS485_H */
