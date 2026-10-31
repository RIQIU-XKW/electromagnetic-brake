/**
 *******************************************************************************
 * @file    service_key.h
 * @brief   PS 两个按键的消抖、判"按下"、派发动作
 *
 * 硬件见 bsp/ps_key(KEY1 = MIO10/E9, KEY2 = MIO11/C6, 按下为低)。
 *
 *   每 SVC_KEY_POLL_MS 读一次两个 MIO -> 连续 SVC_KEY_DEBOUNCE 次一样才算数
 *     -> 稳定电平从松开变按下那一次触发动作: KEY1 软复位, KEY2 重启 TCP 服务端
 *     -> 每次消抖后的电平变化都回调一次 sink(应用层用它上报给上位机)
 *
 * 只上报不接收命令: 按键是"板子 -> 上位机"的输入。应用层可发 0xAA 查状态 /
 * 0xAB 软复位 / 0xAC 网口重连; 上行是 2 字节广播 [0xAA][掩码]。
 *
 * 用轮询而不是 GPIO 中断: 20 ms 延迟感觉不到, 消抖逻辑是纯整数状态机, 能在 PC 上测。
 *******************************************************************************
 */

#ifndef SERVICE_KEY_H
#define SERVICE_KEY_H

#include <stdint.h>

#include "service/service.h"
#include "service/inter_module/service_net.h"     /* Svc_Net_CmdTypeDef / Svc_Net_ReplyTypeDef */

/* -------------------------------------------------------------------------- */
/*  组件参数                                                                   */
/* -------------------------------------------------------------------------- */

#define SVC_KEY_STACK_WORDS     384U    /**< 1.5 KB                          */
#define SVC_KEY_PRIO            2U      /**< 按键不急, 被抢占没关系            */

/** 按下 KEY1 之后先等这么久再真复位(让正在录的数据落盘、日志发完)。 */
#define SVC_KEY_RESET_SETTLE_MS 300U

/** KEY2 重连前先等这么久, 让"按键按下了"那条上报先发出去。 */
#define SVC_KEY_NET_NOTICE_MS   50U

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  初始化参数 —— 由 app 从 app_config.h 填。
 *         "多久轮询""按几下算稳定"是应用策略, service 层不 include app/ 的头。
 */
typedef struct {
    uint32_t PollMs;        /**< 轮询周期(ms), 0 = 用默认 20                  */
    uint32_t ArmDelayMs;    /**< 上电后多久之内不响应按键(ms), 0 = 不保护      */
    uint8_t  Debounce;      /**< 连续几次相同才算稳定, 0 = 用默认 3            */
    uint8_t  ActiveLow;     /**< 1 = 按下读到低(按键接 GND + 内部上拉)        */
    uint8_t  Key2RestartEnable; /**< 1 = KEY2 执行"重启 TCP 服务端"; 0 = 保留不使用 */
} Svc_Key_ConfigTypeDef;

/** 一条按键的消抖状态(纯整数, 好测)。 */
typedef struct {
    uint8_t Last;       /**< 上一次采到的原始电平                         */
    uint8_t Stable;     /**< 消抖之后的稳定电平                           */
    uint8_t Count;      /**< 连续采到 Last 的次数                          */
} Svc_KeyStateTypeDef;

/** 上报掩码里的一位: 哪个键现在按着。 */
#define SVC_KEY_EVENT_KEY1      0x01U   /**< KEY1(MIO10/E9) 按下 -> 软复位   */
#define SVC_KEY_EVENT_KEY2      0x02U   /**< KEY2(MIO11/C6) 按下 -> 网口重连 */

/**
 * @brief  按键状态变化回调(param = 消抖后当前按着的键, 见 SVC_KEY_EVENT_*)。
 * @note   消抖后电平变化时(按下和松开都调), 过了上电保护期先回调一次基线。
 *         跑在 key 任务里, 必须"拷走就返回"(典型实现是 Svc_Net_SendBytes 入队)。
 */
typedef void (*Svc_Key_EventSinkTypeDef)(uint8_t PressedMask);

/** 给上层看的只读快照。 */
typedef struct {
    uint8_t  Ready;         /**< 1 = 驱动初始化完成                         */
    uint8_t  Armed;         /**< 1 = 已经过了上电保护期, 按键开始生效        */
    uint8_t  Key1Level;     /**< KEY1 消抖后的电平(0 = 按下)                */
    uint8_t  Key2Level;     /**< KEY2 消抖后的电平                          */
    uint32_t Key1Presses;   /**< KEY1 按下过几次                            */
    uint32_t Key2Presses;   /**< KEY2 按下过几次                            */
    uint32_t Resets;        /**< 触发过几次复位                             */
    uint32_t NetRestarts;   /**< 触发过几次网络重启                         */
    uint8_t  Key2Enabled;   /**< 1 = KEY2 的动作是开的; 0 = 保留不使用       */
} Svc_Key_SnapshotTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                     */
/* -------------------------------------------------------------------------- */

/** @brief  初始化键盘驱动 + 保存运行参数(Cfg 不允许为 NULL)。 */
Svc_StatusTypeDef Svc_Key_Init(const Svc_Key_ConfigTypeDef *Cfg);

/** @brief  初始化好了没有(1 = 好了)。应用层据此决定要不要建任务。 */
uint8_t Svc_Key_IsReady(void);

/** @brief  按键任务入口(轮询 + 消抖 + 动作/上报); 任务由应用层创建。 */
void Svc_Key_Task(void *Arg);

/** @brief  读一份快照。 */
Svc_StatusTypeDef Svc_Key_GetSnapshot(Svc_Key_SnapshotTypeDef *Out);

/** @brief  挂上"按键状态变化"的上报口; NULL = 取消。 */
Svc_StatusTypeDef Svc_Key_SetEventSink(Svc_Key_EventSinkTypeDef Sink);

/** @brief  软复位(和按 KEY1 同一动作); 正常不会返回。 */
void Svc_Key_RequestReset(void);

/** @brief  重启 TCP 服务(和按 KEY2 同一动作)。1 = 已请求; 0 = 这个功能没开。 */
int Svc_Key_RequestNetRestart(void);

#endif /* SERVICE_KEY_H */
