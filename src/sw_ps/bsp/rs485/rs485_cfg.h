/**
 *******************************************************************************
 * @file    rs485_cfg.h
 * @brief   RS-485 模块的硬件绑定与编译期配置
 *
 * 板级接线(base_system_pins.xdc):
 *
 *   AXI UARTLite @ 0x42C00000   115200 8N1 无校验
 *     tx  -> D19 ----> 收发器 DI   (FPGA 输出)
 *     rx  <- D20 <---- 收发器 RO   (FPGA 输入)
 *     这两根不能接反: 接反了 TX 和 RO 对顶, 收发全废。
 *
 *   AXI GPIO     @ 0x41200000   通道 1, 1 位, 全输出
 *     bit 0 -> G14 ---> 收发器 DE / !RE(并在一起, 1 = 驱动总线, 0 = 接收)
 *
 * 波特率/数据位/无校验是做在 IP 里的, 这里只是照抄过来给日志和算字符时间用。
 *******************************************************************************
 */

#ifndef RS485_CFG_H
#define RS485_CFG_H

#include "xparameters.h"

/* -------------------------------------------------------------------------- */
/* 硬件绑定                                                                   */
/* -------------------------------------------------------------------------- */

/** AXI UARTLite: 数据通路。 */
#define RS485_UART_BASEADDR         XPAR_AXI_UARTLITE_0_BASEADDR
#define RS485_UART_BAUDRATE         XPAR_AXI_UARTLITE_0_BAUDRATE
#define RS485_UART_DATA_BITS        XPAR_AXI_UARTLITE_0_DATA_BITS

/** AXI GPIO: 收发器方向控制。 */
#define RS485_GPIO_BASEADDR         XPAR_AXI_GPIO_0_BASEADDR
#define RS485_GPIO_CHANNEL          1U          /**< 单通道实例              */
#define RS485_GPIO_DE_MASK          0x00000001U /**< de_485_tri_o[0]         */

/** DE 极性: 1 = 高电平让收发器驱动总线。接反了先在线翻(Rs485_SetDeActiveHigh)。 */
#define RS485_DE_TRANSMIT_LEVEL     1U
#define RS485_DE_RECEIVE_LEVEL      0U

/* -------------------------------------------------------------------------- */
/* 发送时序                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * 8N1 一个字符的宽度(1 起始 + 8 数据 + 1 停止)。
 * 用来定 DE 释放时机: UARTLite 在最后一个字符还在移位时就报 FIFO 空, 所以使能必须
 * 多保持至少一个字符时间。两个字符余量是 Xilinx RS-485 参考设计用的值。
 */
#define RS485_TX_TAIL_GUARD_CHARS   2U

/** "TX FIFO 有空位"和"TX FIFO 排空"的超时(微秒)。 */
#define RS485_TX_TIMEOUT_US         20000U

/** 切到发送之后、压第一个字节之前的稳定时间(微秒)。 */
#define RS485_DE_SETTLE_US          2U

/** 中断优先级, 只在 RS485_USE_RTOS_IRQ = 0(裸机)时用。 */
#define RS485_IRQ_PRIORITY          0xA0U

/**
 * 1 = 走 OSAL 连中断(默认), 0 = 用 lwIP 端口的 XSetupInterruptSystem(仅裸机)。
 * 不能用后者: 它把标准 XScuGic_InterruptHandler 装到异常向量上, 而 RTOS 要自己的
 * 中断入口(保存/恢复任务上下文), 混用会让 tick 走错路。
 */
#define RS485_USE_RTOS_IRQ          1U

/* -------------------------------------------------------------------------- */
/* 接收                                                                       */
/* -------------------------------------------------------------------------- */

/** 软件接收 FIFO 深度。必须是 2 的幂(模块用掩码而不是取模)。 */
#define RS485_RX_FIFO_SIZE          256U

/** 一次交给接收钩子的字节数上限。 */
#define RS485_RX_CHUNK_SIZE         64U

/** FIFO 排空循环的上限(防止状态位卡住导致死循环)。 */
#define RS485_RX_DRAIN_MAX          64U

#endif /* RS485_CFG_H */
