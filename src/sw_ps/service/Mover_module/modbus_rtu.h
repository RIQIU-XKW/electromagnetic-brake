/**
 *******************************************************************************
 * @file    modbus_rtu.h
 * @brief   Modbus RTU 主机组帧 + CRC —— 纯逻辑, 不碰任何硬件
 *
 * 刻意不 include 任何 BSP/OSAL 头: 只做"值 -> 帧"的数学, 可以在 PC 上单测。
 * 帧格式: [从站][功能码][数据...][CRC_lo][CRC_hi]
 * 只组发出去的请求帧: 驱动器只收不回, 没有接收方向 / 校验 / 解析的代码。
 *******************************************************************************
 */

#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <stdint.h>

/** 单帧最大长度: 写 32 位对象是 13 字节, 留点余量。 */
#define MB_FRAME_MAX    64U

/* -------------------------------------------------------------------------- */
/*  Modbus 功能码(寄存器映射见 motor_cia402.h)                                 */
/* -------------------------------------------------------------------------- */

#define MODBUS_FC_READ_HOLDING      0x03U   /**< 读保持寄存器(本组件不发)       */
#define MODBUS_FC_WRITE_SINGLE      0x06U   /**< 写单个寄存器                   */
#define MODBUS_FC_WRITE_MULTIPLE    0x10U   /**< 写多个寄存器(32 位对象用这个)  */

/* -------------------------------------------------------------------------- */
/*  CRC                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief  算 Modbus CRC16(多项式 0xA001, 初值 0xFFFF)。
 * @param  Data : 参与计算的字节(从站地址到最后一个数据字节, 不含 CRC 本身)。
 * @return CRC 值; 线上先发低字节再发高字节。
 */
uint16_t Mb_Crc16(const uint8_t *Data, uint16_t Len);

/* -------------------------------------------------------------------------- */
/*  组帧(主机 -> 驱动器)                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief  0x06 写单个寄存器(例如控制字 0x6040、模式 0x6060)。
 * @param  DstSize : 缓冲大小, 至少要 8。
 * @return 帧长度(8), 参数不合法返回 0。例: 01 06 60 40 00 0F D6 1A
 */
uint16_t Mb_BuildWriteSingle(uint8_t *Dst, uint16_t DstSize,
                             uint8_t Slave, uint16_t Reg, uint16_t Value);

/**
 * @brief  0x10 写多个寄存器; Values 高位寄存器在前, Count = 1..123。
 * @return 帧长度(9 + 2*Count), 参数不合法返回 0。
 */
uint16_t Mb_BuildWriteMultiple(uint8_t *Dst, uint16_t DstSize,
                               uint8_t Slave, uint16_t Reg,
                               const uint16_t *Values, uint16_t Count);

/**
 * @brief  0x10 写一个 32 位对象(占 2 个寄存器, 高字在前)。
 * @return 帧长度(13), 参数不合法返回 0。例: 01 10 60 81 00 02 04 00 07 A1 20 5B 88
 */
uint16_t Mb_BuildWriteU32(uint8_t *Dst, uint16_t DstSize,
                          uint8_t Slave, uint16_t Reg, uint32_t Value);

#endif /* MODBUS_RTU_H */
