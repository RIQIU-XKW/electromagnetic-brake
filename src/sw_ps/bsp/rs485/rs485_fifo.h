/**
 *******************************************************************************
 * @file    rs485_fifo.h
 * @brief   RS-485 模块的字节 FIFO(单生产者 / 单消费者, 无锁)
 *
 * 用在"接收通路(生产者)"和"应用(消费者)"之间。生产者要么是收中断, 要么是
 * Rs485_Poll() 里的临界区(轮询模式), 两者天生互斥, 所以 put/get 不需要加锁。
 * Head/Tail 是自由增长的计数器(用时才取掩码), 没有"满还是空"的歧义。
 *******************************************************************************
 */

#ifndef RS485_FIFO_H
#define RS485_FIFO_H

#include <stdint.h>
#include <stddef.h>

#include "rs485_cfg.h"

/* -------------------------------------------------------------------------- */
/* 类型                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t           Buffer[RS485_RX_FIFO_SIZE];   /**< 存储区               */
    volatile uint32_t Head;                         /**< 生产者下标           */
    volatile uint32_t Tail;                         /**< 消费者下标           */
} Rs485_FifoTypeDef;

/* -------------------------------------------------------------------------- */
/* 接口                                                                       */
/* -------------------------------------------------------------------------- */

/** @brief  丢弃所有已缓冲的字节(不能和并发的 put 一起用)。 */
void     Rs485_Fifo_Reset(Rs485_FifoTypeDef *fifo);

/** @brief  还有多少字节可读。 */
uint32_t Rs485_Fifo_Available(const Rs485_FifoTypeDef *fifo);

/** @brief  压一个字节; 1 = 成功, 0 = 满了。 */
uint8_t  Rs485_Fifo_Put(Rs485_FifoTypeDef *fifo, uint8_t byte);

/** @brief  弹一个字节; 1 = 成功, 0 = 空了。 */
uint8_t  Rs485_Fifo_Get(Rs485_FifoTypeDef *fifo, uint8_t *byte);

/** @brief  弹一批字节; 返回实际拷进 dst 的字节数。 */
uint32_t Rs485_Fifo_Read(Rs485_FifoTypeDef *fifo, uint8_t *dst, uint32_t len);

#endif /* RS485_FIFO_H */
