/**
 *******************************************************************************
 * @file    samp_app.h
 * @brief   采集业务: 单字节命令 0xE0..0xE2
 *
 *   0xE0  开始采样(ADS 每 10 ms 扫一轮 + 每拍广播一行 DATA, 同时开始录卡)
 *   0xE1  停止采样(不发 SPI、不广播、不写卡)
 *   0xE2  查状态(回一行文本: 在采/停止 + 录制情况)
 *
 * 采集和录制本身在 service/Samp_module(service_ads + ads8688 + service_emmc),
 * 本层只把命令转成对 Svc_Ads_* / Svc_Emmc_* 的调用。
 *******************************************************************************
 */

#ifndef SAMP_APP_H
#define SAMP_APP_H

#include <stdint.h>

#include "service/service.h"
#include "service/inter_module/service_net.h"     /* Svc_Net_CmdTypeDef / Svc_Net_ReplyTypeDef */

#define SAMP_CMD_START      0xE0U   /**< 开始采样 */
#define SAMP_CMD_STOP       0xE1U   /**< 停止采样 */
#define SAMP_CMD_STATUS     0xE2U   /**< 查状态   */
#define SAMP_CMD_ZERO       0xE3U   /**< 电流零点标定(采当前电压当零电流电压; 要求制动关闭) */

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/** @brief  开始采样(等价于收到 0xE0)。 */
Svc_StatusTypeDef Samp_App_StartSampling(void);

/** @brief  停止采样(等价于收到 0xE1)。 */
Svc_StatusTypeDef Samp_App_StopSampling(void);

/** @brief  现在在不在采样(1 = 每 10 ms 一轮 + 广播 DATA)。 */
int Samp_App_IsSampling(void);

/**
 * @brief  建本模块的两条任务: ads(10 ms 采集) + emmc(写盘/同步)。
 * @return SVC_OK / SVC_ERR_HW。
 */
Svc_StatusTypeDef Samp_App_Start(void);

/**
 * @brief  处理 0xE0..0xE2(由 inter_app 的命令地图调用)。
 * @return 1 = 报文里有本模块的字节; 0 = 没有(放行)。
 */
int Samp_App_Command(void *Arg, const uint8_t *Data, uint16_t Len, Svc_Net_ReplyTypeDef *R);

#endif /* SAMP_APP_H */
