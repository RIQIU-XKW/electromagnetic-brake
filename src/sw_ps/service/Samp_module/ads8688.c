/**
 *******************************************************************************
 * @file    ads8688.c
 * @brief   ADS8688 协议层实现: 组帧 / 取数 / 原始码换算电压(纯逻辑)
 *******************************************************************************
 */

#include "service/Samp_module/ads8688.h"

#include <stddef.h>     /* NULL */

void Ads_FrameCmd(uint16_t Cmd, uint8_t Output[4])
{
    if (Output == NULL) {
        return;
    }

    Output[0] = (uint8_t)(Cmd >> 8);        /* MSB 在前 */
    Output[1] = (uint8_t)(Cmd & 0xFFU);
    Output[2] = 0x00U;                      /* 再补一帧 NO_OP */
    Output[3] = 0x00U;
}

void Ads_FrameWriteReg(uint8_t Addr, uint8_t Data, uint8_t Output[2])
{
    if (Output == NULL) {
        return;
    }

    /* 16 位帧 = [A6..A0][R/W=1][D7..D0] */
    Output[0] = (uint8_t)((Addr << 1) | 0x01U);
    Output[1] = Data;
}

void Ads_FrameReadReg(uint8_t Addr, uint8_t Output[3])
{
    if (Output == NULL) {
        return;
    }

    /* [A6..A0][R/W=0], 再收 2 字节; 与厂商驱动的 24 个时钟一致 */
    Output[0] = (uint8_t)(Addr << 1);
    Output[1] = 0x00U;
    Output[2] = 0x00U;
}

void Ads_FrameNoOp(uint8_t Output[2])
{
    if (Output == NULL) {
        return;
    }

    Output[0] = 0x00U;
    Output[1] = 0x00U;
}

uint16_t Ads_Be16(const uint8_t Input[2])
{
    if (Input == NULL) {
        return 0U;
    }

    return (uint16_t)(((uint16_t)Input[0] << 8) | (uint16_t)Input[1]);
}

int Ads_RangeIsBipolar(uint8_t Range)
{
    switch (Range) {
        case ADS_RANGE_BIPOLAR_2P5REF:
        case ADS_RANGE_BIPOLAR_1P25REF:
        case ADS_RANGE_BIPOLAR_0P625REF:
            return 1;
        default:
            return 0;               /* 单极性 */
    }
}

int32_t Ads_RangeSpanMilliVolts(uint8_t Range)
{
    switch (Range) {
        /* 双极性: 满量程跨度 = 2 * N * Vref */
        case ADS_RANGE_BIPOLAR_2P5REF:  return  2L * 2500L * ADS_VREF_MV / 1000L;  /* 20480 */
        case ADS_RANGE_BIPOLAR_1P25REF: return  2L * 1250L * ADS_VREF_MV / 1000L;  /* 10240 */
        case ADS_RANGE_BIPOLAR_0P625REF:return  2L *  625L * ADS_VREF_MV / 1000L;  /*  5120 */
        /* 单极性: 跨度 = N * Vref */
        case ADS_RANGE_UNIPOLAR_2P5REF: return      2500L * ADS_VREF_MV / 1000L;   /* 10240 */
        case ADS_RANGE_UNIPOLAR_1P25REF:return      1250L * ADS_VREF_MV / 1000L;   /*  5120 */
        default:                        return      2500L * ADS_VREF_MV / 1000L;
    }
}

int32_t Ads_RawToMilliVolts(uint16_t Raw, uint8_t Range)
{
    const int32_t span = Ads_RangeSpanMilliVolts(Range);

    if (Ads_RangeIsBipolar(Range) != 0) {
        /* 偏移二进制, 中点 32767(厂商 demo 的算法); 整数运算, 不引入浮点 */
        const int32_t centred = (int32_t)Raw - (int32_t)ADS_BIPOLAR_ZERO;

        return (int32_t)(((int64_t)centred * (int64_t)span) / ADS_COUNTS);
    }

    return (int32_t)(((int64_t)(int32_t)Raw * (int64_t)span) / ADS_COUNTS);
}
