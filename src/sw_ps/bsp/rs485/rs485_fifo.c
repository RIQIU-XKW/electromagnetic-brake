/**
 *******************************************************************************
 * @file    rs485_fifo.c
 * @brief   RS-485 模块的字节 FIFO(单生产者 / 单消费者)
 *
 * 并发约定见 rs485_fifo.h。所有下标都用 RS485_RX_FIFO_SIZE - 1 取掩码, 所以深度
 * 必须是 2 的幂。
 *******************************************************************************
 */

#include "rs485_fifo.h"

/* ========================================================================== */
/*  内部函数(static)                                                             */
/* ========================================================================== */

#define RS485_FIFO_MASK     (RS485_RX_FIFO_SIZE - 1U)

/** 编译期检查"深度是 2 的幂"(不满足时数组长度为 -1, 直接编不过)。 */
typedef char Rs485_Fifo_SizeMustBePowerOfTwo[(RS485_FIFO_MASK & RS485_RX_FIFO_SIZE) == 0U ? 1 : -1];

/* ========================================================================== */
/*  对外接口                                                                     */
/* ========================================================================== */

void Rs485_Fifo_Reset(Rs485_FifoTypeDef *fifo)
{
    if (fifo == NULL) {
        return;
    }

    fifo->Head = 0U;
    fifo->Tail = 0U;
}

uint32_t Rs485_Fifo_Available(const Rs485_FifoTypeDef *fifo)
{
    if (fifo == NULL) {
        return 0U;
    }

    return (fifo->Head - fifo->Tail) & RS485_FIFO_MASK;
}

uint8_t Rs485_Fifo_Put(Rs485_FifoTypeDef *fifo, uint8_t byte)
{
    uint32_t head;

    if (fifo == NULL) {
        return 0U;
    }

    head = fifo->Head;

    /* 留一格不用, 这样 Head == Tail 永远表示"空" */
    if (((head - fifo->Tail) & RS485_FIFO_MASK) == (RS485_RX_FIFO_SIZE - 1U)) {
        return 0U;                          /* 满                              */
    }

    fifo->Buffer[head & RS485_FIFO_MASK] = byte;

    /* 数据写好了才发布 Head */
    fifo->Head = (head + 1U) & RS485_FIFO_MASK;

    return 1U;
}

uint8_t Rs485_Fifo_Get(Rs485_FifoTypeDef *fifo, uint8_t *byte)
{
    uint32_t tail;
    uint8_t  value;

    if ((fifo == NULL) || (byte == NULL)) {
        return 0U;
    }

    tail = fifo->Tail;

    if (tail == fifo->Head) {
        return 0U;                          /* 空                              */
    }

    value = fifo->Buffer[tail & RS485_FIFO_MASK];

    /* 数据读走了才释放这一格 */
    fifo->Tail = (tail + 1U) & RS485_FIFO_MASK;
    *byte      = value;

    return 1U;
}

uint32_t Rs485_Fifo_Read(Rs485_FifoTypeDef *fifo, uint8_t *dst, uint32_t len)
{
    uint32_t read = 0U;

    if ((fifo == NULL) || (dst == NULL)) {
        return 0U;
    }

    while (read < len) {
        if (Rs485_Fifo_Get(fifo, &dst[read]) == 0U) {
            break;
        }
        read++;
    }

    return read;
}
