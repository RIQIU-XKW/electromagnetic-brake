/**
 *******************************************************************************
 * @file    service_pwm.h  (HOST STUB)
 * @brief   主机测试用的 PWM 服务桩: 只留电流闭环真正写的那几个接口
 *
 * 真正的 service_pwm 会去算死区周期、写 AXI 寄存器、改腿 B 模式; 这里只要把
 * "闭环到底让谁输出、输出了多少"记下来, 断言就靠这几个值。
 *******************************************************************************
 */

#ifndef STUB_SERVICE_PWM_H
#define STUB_SERVICE_PWM_H

#include <stdint.h>

#include "service/service.h"

/** 写某桥占空比(千分比)并把它切成"腿 A 静态高 + 腿 B 斩波"。 */
Svc_StatusTypeDef Svc_Pwm_SetBridgeBrakePermille(int Bridge, uint32_t Permille);

/** 五桥输出使能(1 = 开)。 */
Svc_StatusTypeDef Svc_Pwm_SetAllBridgeEnable(int Enable);

/** 全局调制使能(1 = 开)。 */
Svc_StatusTypeDef Svc_Pwm_SetGlobalEnable(int Enable);

/** 五桥死区时间(ns)统一设置。 */
Svc_StatusTypeDef Svc_Pwm_SetAllDeadtimeNs(uint32_t Ns);

#endif /* STUB_SERVICE_PWM_H */
