/**
 *******************************************************************************
 * @file    modbus_rtu.c
 * @brief   Modbus RTU 主机**组帧** + CRC(纯逻辑, 无 BSP/OSAL 依赖)
 *
 * ⚠ 2026-10-15 裁剪: 只保留"发"这一半 —— CRC + 组帧。驱动器**只接收不回复**, 所以
 *   Mb_CheckFrame() / Mb_ParseResponse() / Mb_ExceptionName() 三个接收解析函数,
 *   以及 Mb_BuildReadHolding()(读保持寄存器, 没有读的入口了)全部删掉了。
 *******************************************************************************
 */

#include "service/Mover_module/modbus_rtu.h"

#include <string.h>

/* ========================================================================== */
/*  CRC                                                                        */
/* ========================================================================== */

uint16_t Mb_Crc16(const uint8_t *Data, uint16_t Len)
{
    uint16_t crc = 0xFFFFU;
    uint16_t i;
    uint8_t  bit;

    if (Data == NULL) {
        return 0U;
    }

    for (i = 0U; i < Len; i++) {
        crc ^= (uint16_t)Data[i];

        for (bit = 0U; bit < 8U; bit++) {
            if ((crc & 0x0001U) != 0U) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }

    return crc;
}


/* ========================================================================== */
/*  组帧                                                                       */
/* ========================================================================== */

/**
 * @brief  收尾: 补 CRC 并返回帧长.
 * @param  Dst : 已经写好 [从站][功能码][数据...] 的缓冲.
 * @param  Len : 上面的长度.
 * @return Len + 2.
 */
static uint16_t Mb_Finish(uint8_t *Dst, uint16_t Len)
{
    uint16_t crc = Mb_Crc16(Dst, Len);

    Dst[Len]      = (uint8_t)(crc & 0xFFU);     /* 低字节在前 */
    Dst[Len + 1U] = (uint8_t)(crc >> 8);

    return (uint16_t)(Len + 2U);
}

uint16_t Mb_BuildWriteSingle(uint8_t *Dst, uint16_t DstSize,
                             uint8_t Slave, uint16_t Reg, uint16_t Value)
{
    if ((Dst == NULL) || (DstSize < 8U)) {
        return 0U;
    }

    Dst[0] = Slave;
    Dst[1] = MODBUS_FC_WRITE_SINGLE;
    Dst[2] = (uint8_t)(Reg >> 8);
    Dst[3] = (uint8_t)(Reg & 0xFFU);
    Dst[4] = (uint8_t)(Value >> 8);
    Dst[5] = (uint8_t)(Value & 0xFFU);

    return Mb_Finish(Dst, 6U);
}

uint16_t Mb_BuildWriteMultiple(uint8_t *Dst, uint16_t DstSize,
                               uint8_t Slave, uint16_t Reg,
                               const uint16_t *Values, uint16_t Count)
{
    uint16_t i;
    uint16_t pos;

    if ((Dst == NULL) || (Values == NULL) || (Count == 0U) || (Count > 123U)) {
        return 0U;
    }

    /* 1 地址 + 1 功能 + 2 起始 + 2 数量 + 1 字节数 + 2*N 数据 + 2 CRC */
    if (DstSize < (uint16_t)(9U + (2U * Count))) {
        return 0U;
    }

    Dst[0] = Slave;
    Dst[1] = MODBUS_FC_WRITE_MULTIPLE;
    Dst[2] = (uint8_t)(Reg >> 8);
    Dst[3] = (uint8_t)(Reg & 0xFFU);
    Dst[4] = (uint8_t)(Count >> 8);
    Dst[5] = (uint8_t)(Count & 0xFFU);
    Dst[6] = (uint8_t)(Count * 2U);

    pos = 7U;
    for (i = 0U; i < Count; i++) {
        Dst[pos] = (uint8_t)(Values[i] >> 8);       /* 寄存器值高字节在前 */
        Dst[pos + 1U] = (uint8_t)(Values[i] & 0xFFU);
        pos = (uint16_t)(pos + 2U);
    }

    return Mb_Finish(Dst, pos);
}

uint16_t Mb_BuildWriteU32(uint8_t *Dst, uint16_t DstSize,
                          uint8_t Slave, uint16_t Reg, uint32_t Value)
{
    uint16_t words[2];

    words[0] = (uint16_t)(Value >> 16);     /* 高字在前 */
    words[1] = (uint16_t)(Value & 0xFFFFU);

    return Mb_BuildWriteMultiple(Dst, DstSize, Slave, Reg, words, 2U);
}

