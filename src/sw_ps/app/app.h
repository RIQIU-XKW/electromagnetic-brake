/**
 *******************************************************************************
 * @file    app.h
 * @brief   应用层(第四层): 组件初始化顺序 + 命令地图注册 + 任务创建
 *
 * 分层: osal(系统抽象) -> bsp(驱动) -> service(业务组件) -> app(装配)。
 * app 层不 include bsp 头、不直接调驱动 API, 需要什么经 service 拿。
 *
 *   service/inter_module    net(命令服务器/广播/分发) + key + net_text
 *   service/monitor_module   monitor(状态巡检) + led(呼吸灯) + log(跨层日志)
 *   service/brake_module     service_pwm + service_ls + service_oc
 *   service/Mover_module     service_motor + service_rs485 + modbus_rtu
 *   service/Samp_module      service_ads + ads8688 + service_emmc
 *
 *   app/app.c          装配: 初始化顺序 + 注册命令地图 + 调各模块的 Xxx_App_Start
 *   app/brake_ctl_app   制动控制(五路同步) + BrakeCtl_App_Start(ls/brkctl 任务)
 *   app/brake_app       旧的光幕时序状态机: 保留但不调用(见 .c 末尾)
 *   app/Mover_app       0xD0..0xD6 发射小车 + Mover_App_Start(rs485/mover 任务)
 *   app/samp_app        0xE0..0xE2 采样 + Samp_App_Start(ads/emmc 任务)
 *   app/monitor_app     0xF0..0xFF 呼吸灯 + Monitor_App_Start(led/mon 任务)
 *   app/inter_app       STATUS + 0xAA..0xAC 按键 + 命令地图 + Inter_App_Start(net/key 任务)
 *******************************************************************************
 */

#ifndef APP_H
#define APP_H

#include <stdint.h>

#include "app/app_config.h"

/* -------------------------------------------------------------------------- */
/*  返回码                                                                     */
/* -------------------------------------------------------------------------- */

typedef enum {
    APP_OK          =  0,
    APP_ERR_PARAM   = -1,
    APP_ERR_FORMAT  = -2,
    APP_ERR_CRC     = -3,
    APP_ERR_FULL    = -4,
    APP_ERR_HW      = -5
} App_StatusTypeDef;

/* -------------------------------------------------------------------------- */
/*  初始化与启动(必须在调度器启动之前调用)                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief  初始化各组件 + 注册命令地图(Inter_App_Commands)。
 *
 * 顺序约束: net 必须最先初始化(lwIP 的 xemac_add 会做第一次 GIC 初始化, 会清 GIC
 * 处理器表); 命令注册必须在建 net 任务之前完成(注册表没有锁)。
 *
 * @return APP_OK / APP_ERR_HW。
 */
App_StatusTypeDef App_Init(void);

/**
 * @brief  建全部任务: 依次调 Inter_App_Start -> Monitor_App_Start -> BrakeCtl_App_Start
 *         -> Mover_App_Start -> Samp_App_Start。
 *
 * 一个模块失败不会影响后面的模块(net 失败除外, 那会返回错误让 main 停机)。
 *
 * @return APP_OK / APP_ERR_HW。
 */
App_StatusTypeDef App_StartTasks(void);

#endif /* APP_H */
