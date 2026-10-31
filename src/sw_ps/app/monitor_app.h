/**
 *******************************************************************************
 * @file    monitor_app.h
 * @brief   呼吸灯命令(0xF0..0xFF) + monitor_module 的两条任务
 *
 *   0xF0            四路全灭(掩码 0)             0xF5            四路全亮(掩码 0x0F)
 *   0xF1/0xF2/0xF3  慢(~5 s)/标准(~1 s)/快(~100 ms)呼吸
 *   0xF4            查状态(回一行文本: 掩码 / 亮点 / 频率 / 模式)
 *   0xF6..0xF9      单独点亮 LED0..LED3          0xFA..0xFD      单独熄灭 LED0..LED3
 *   0xFE <频率/10>  设共享频率(1..100 = 10..1000 Hz)
 *   0xFF <掩码>     设四路掩码(0..15)
 *
 * 点灯能力在 service/monitor_module/led; 本层只做"命令 -> Svc_Led_*"的转发。
 *******************************************************************************
 */

#ifndef MONITOR_APP_H
#define MONITOR_APP_H

#include <stdint.h>

#include "service/service.h"
#include "service/inter_module/service_net.h"     /* Svc_Net_CmdTypeDef / Svc_Net_ReplyTypeDef */

/* -------------------------------------------------------------------------- */
/*  呼吸灯命令(下行: 上位机 -> 板子)                                            */
/* -------------------------------------------------------------------------- */

#define LED_CMD_ALL_OFF     0xF0U   /**< 四路全灭                          */
#define LED_CMD_SLOW        0xF1U   /**< 慢呼吸                            */
#define LED_CMD_NORMAL      0xF2U   /**< 标准呼吸                          */
#define LED_CMD_FAST        0xF3U   /**< 快呼吸                            */
#define LED_CMD_STATUS      0xF4U   /**< 查状态(回一行文本)                */
#define LED_CMD_ALL_ON      0xF5U   /**< 四路全亮                          */
#define LED_CMD_ON_BASE     0xF6U   /**< 0xF6+i = 点亮第 i 路(i = 0..3)    */
#define LED_CMD_OFF_BASE    0xFAU   /**< 0xFA+i = 熄灭第 i 路(i = 0..3)    */
#define LED_CMD_FREQ        0xFEU   /**< 后跟 1 字节: 频率/10(1..100)      */
#define LED_CMD_MASK        0xFFU   /**< 后跟 1 字节: 四路掩码(0..15)      */

/** 0xFE 的参数编码: 参数字节 x 10 = Hz。 */
#define LED_CMD_FREQ_UNIT   10U

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  建本模块的两条任务: led(队列阻塞的点灯任务) + mon(状态巡检)。
 * @return SVC_OK / SVC_ERR_STATE(led 组件没就绪) / SVC_ERR_HW。
 */
Svc_StatusTypeDef Monitor_App_Start(void);

/**
 * @brief  处理 0xF0..0xFF(由 inter_app 的命令地图调用)。
 * @return 1 = 报文里有本模块的字节; 0 = 没有(放行)。
 */
int Monitor_App_Command(void *Arg, const uint8_t *Data, uint16_t Len,
                        Svc_Net_ReplyTypeDef *R);

#endif /* MONITOR_APP_H */
