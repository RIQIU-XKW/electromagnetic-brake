/**
 *******************************************************************************
 * @file    ads8688.h
 * @brief   ADS8688(8 通道 16 位 ADC)的协议层: 帧格式 + 电压换算, 不碰硬件
 *
 * 器件: TI ADS8688, 8 通道 / 16 位 / 内置 4.096 V 基准; 模块 YK099。
 * 接口: SPI Mode 0, 16 位帧, MSB 在前, 片选低有效。纯逻辑, 可在 PC 上单测。
 *
 *   命令寄存器: [cmd_hi, cmd_lo, 0x00, 0x00](尾部补一帧 NO_OP)
 *   程序寄存器: 写 = [(addr<<1)|1, data]
 *               读 = [addr<<1, 0, 0], 收回 3 字节, Rx[2] 才是寄存器值
 *   自动扫描  : 配好 AUTO_SEQ_EN / CH_PD 后发一次 AUTO_RST, 之后每发一个 16 位
 *               NO_OP 就取回序列里的下一个通道(升序); 每帧之间片选必须拉高再拉低。
 *   换算      : 双极性 mV = (raw - 32767) * Span / 65535; 单极性 mV = raw * Span / 65535
 *******************************************************************************
 */

#ifndef ADS8688_H
#define ADS8688_H

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/*  命令寄存器(16 位)                                                          */
/* -------------------------------------------------------------------------- */

#define ADS_CMD_NO_OP       0x0000U     /**< 空操作(读数据就是发它)          */
#define ADS_CMD_STDBY       0x8200U     /**< 待机(基准不断电, 20 µs 唤醒)    */
#define ADS_CMD_PWR_DN      0x8300U     /**< 掉电(唤醒要 15 ms)              */
#define ADS_CMD_RST         0x8500U     /**< 复位所有程序寄存器              */
#define ADS_CMD_AUTO_RST    0xA000U     /**< 自动扫描模式                    */
#define ADS_CMD_MAN_AUX     0xE000U     /**< 辅助通道(0..4.096 V)            */

/** @brief  手动通道 n 选择命令(n = 0..7)。 */
#define ADS_CMD_MAN_CH(n)   ((uint16_t)(0xC000U + ((uint16_t)(n) * 0x0400U)))

/* -------------------------------------------------------------------------- */
/*  程序寄存器                                                                 */
/* -------------------------------------------------------------------------- */

#define ADS_REG_AUTO_SEQ_EN 0x01U       /**< 自动扫描序列使能(位图, 默认 FF) */
#define ADS_REG_CH_PD       0x02U       /**< 通道掉电(位图, 写 1 = 掉电)     */
#define ADS_REG_F_S         0x03U       /**< 功能选择(菊花链 ID / SDO 格式)  */

#define ADS_REG_CMD_READ    0x3FU       /**< 命令回读(前一帧命令字的高 8 位) */

/** @brief  通道 n 的量程寄存器地址(n = 0..7)。 */
#define ADS_REG_CHIR(n)     ((uint8_t)(0x05U + (uint8_t)(n)))

/** @brief  通道 n 的使能位。 */
#define ADS_CH_BIT(n)       ((uint8_t)(1U << (uint8_t)(n)))

/* -------------------------------------------------------------------------- */
/*  输入量程                                                                   */
/* -------------------------------------------------------------------------- */

#define ADS_RANGE_BIPOLAR_2P5REF    0x00U   /**< ±2.5*Vref  = ±10.24 V       */
#define ADS_RANGE_BIPOLAR_1P25REF   0x01U   /**< ±1.25*Vref = ±5.12 V        */
#define ADS_RANGE_BIPOLAR_0P625REF  0x02U   /**< ±0.625*Vref= ±2.56 V        */
#define ADS_RANGE_UNIPOLAR_2P5REF   0x05U   /**< 0..2.5*Vref = 0..10.24 V    */
#define ADS_RANGE_UNIPOLAR_1P25REF  0x06U   /**< 0..1.25*Vref= 0..5.12 V     */

#define ADS_VREF_MV         4096L           /**< 内置基准(mV)                */
#define ADS_COUNTS          65535L          /**< 16 位满量程计数             */
#define ADS_BIPOLAR_ZERO    32767L          /**< 双极性量程的零点码值        */

/* -------------------------------------------------------------------------- */
/*  组帧 / 解析(纯函数)                                                        */
/* -------------------------------------------------------------------------- */

/** @brief  命令帧: Output = [cmd_hi, cmd_lo, 0x00, 0x00]。 */
void Ads_FrameCmd(uint16_t Cmd, uint8_t Output[4]);

/** @brief  写寄存器帧: Output = [(addr<<1)|1, data]。 */
void Ads_FrameWriteReg(uint8_t Addr, uint8_t Data, uint8_t Output[2]);

/** @brief  读寄存器事务: Output = [addr<<1, 0, 0]; 收回 3 字节, Rx[2] 是寄存器值。 */
void Ads_FrameReadReg(uint8_t Addr, uint8_t Output[3]);

/** @brief  取一个通道数据的事务: Output = [0x00, 0x00](每帧之间片选要拉高)。 */
void Ads_FrameNoOp(uint8_t Output[2]);

/** @brief  大端 16 位: Input[0] 是高字节。 */
uint16_t Ads_Be16(const uint8_t Input[2]);

/* -------------------------------------------------------------------------- */
/*  换算                                                                       */
/* -------------------------------------------------------------------------- */

/** @brief  这个量程是不是双极性(决定要不要减去中点 32767)。 */
int Ads_RangeIsBipolar(uint8_t Range);

/** @brief  满量程跨度(mV): ±2.5*Vref -> 20480, 0..2.5*Vref -> 10240。 */
int32_t Ads_RangeSpanMilliVolts(uint8_t Range);

/** @brief  原始码 -> 电压(mV)。 */
int32_t Ads_RawToMilliVolts(uint16_t Raw, uint8_t Range);

#endif /* ADS8688_H */
