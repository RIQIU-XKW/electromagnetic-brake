/**
 *******************************************************************************
 * @file    service_ads.h  (HOST STUB)
 * @brief   主机测试用的 ADS 桩: 只留电流闭环真正用到的那一个接口
 *
 * 这里不 include 真的 service_ads.h —— 它拖着 ps_spi/ads8688 一大堆寄存器定义,
 * 在 PC 上编不过也没必要。桩只保留 SVC_ADS_COILS 和取电流那个函数。
 *******************************************************************************
 */

#ifndef STUB_SERVICE_ADS_H
#define STUB_SERVICE_ADS_H

#include <stdint.h>

#include "service/service.h"

/** 线圈路数(和真头文件一致)。 */
#define SVC_ADS_COILS   5U

/** 读 5 路线圈电流(mA)。桩里由 Host_SetCurrentMa() 喂。 */
Svc_StatusTypeDef Svc_Ads_GetCoilCurrentMa(int32_t *Out, uint32_t Count);

#endif /* STUB_SERVICE_ADS_H */
