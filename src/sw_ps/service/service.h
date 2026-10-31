/**
 *******************************************************************************
 * @file    service.h
 * @brief   业务组件层公共定义(第三层): 返回码
 *
 *   app/      应用逻辑(第四层): 装配 —— 初始化顺序、命令表、建任务
 *   service/  业务组件(第三层): 按功能分目录(inter / brake / Samp / Mover / monitor)
 *   bsp/      驱动(第一层): enet / pwm / ls / rs485 / ps_spi / emmc / led ...
 *   osal/     系统抽象(第二层): 任务/队列/互斥量/时间/中断
 *
 * 依赖方向: app -> service -> bsp -> Xilinx, 且 app/service/log/bsp -> osal -> RTOS。
 *           service 不许 include "app/...", bsp 不许 include "app/..." 或 "service/..."。
 *******************************************************************************
 */

#ifndef SERVICE_H
#define SERVICE_H

/** 业务组件的统一返回码: 上层只想知道"这件事成没成", 不想知道是哪个驱动报的错。 */
typedef enum {
    SVC_OK         =  0,
    SVC_ERR_PARAM  = -1,
    SVC_ERR_FULL   = -2,
    SVC_ERR_HW     = -3,
    SVC_ERR_STATE  = -4
} Svc_StatusTypeDef;

#endif /* SERVICE_H */
