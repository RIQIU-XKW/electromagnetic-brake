/**
 *******************************************************************************
 * @file    service_net.h
 * @brief   TCP 文本命令服务器(命令注册表 + 广播队列 + 快照)
 *
 * 本组件不认识任何具体命令: 谁想加命令就把一张 Svc_Net_CmdTypeDef 表交给
 * Svc_Net_RegisterCommands()。自己只内置 PING 和"都不认识时回一行提示"。
 *
 * 命令匹配: 先去掉尾部 \r \n 空格; Name 与整行精确相等时命中(按注册顺序取第一个);
 *           Name == NULL 是兜底项, 精确匹配全失败后按注册顺序依次问, 返回非 0 表示认领。
 *
 * 线程模型: lwIP 是 NO_SYS = 1 的 RAW API, 所有调用必须来自 net 任务。别的组件想发
 *           文本只调 Svc_Net_SendText() 排队, 状态由 Svc_Net_GetSnapshot() 只读快照读。
 *******************************************************************************
 */

#ifndef SERVICE_NET_H
#define SERVICE_NET_H

#include <stdint.h>

#include "service/service.h"

#include "bsp/enet/enet.h"      /* Enet_StatusTypeDef / Enet_TcpChannelTypeDef / LinkInfo */

/* -------------------------------------------------------------------------- */
/*  组件参数                                                                   */
/* -------------------------------------------------------------------------- */

#define SVC_NET_PERIOD_MS       2U      /**< 轮询周期(ms): 越短 TCP 越及时  */
#define SVC_NET_STACK_WORDS     1536U   /**< 6 KB: lwIP + 文本格式化        */
#define SVC_NET_PRIO            4U      /**< 最高: EMAC 收包环很小, 要按时喂 */

/** 广播文本队列深度。 */
#define SVC_NET_Q_LEN           8U

/** 一条广播/回复文本的最大长度(含 \r\n); 超长会被截断。 */
#define SVC_NET_MSG_MAX         192U

/** 精确匹配命令表容量(表是编译期常量数组, 多留几个只占几十字节)。 */
#define SVC_NET_MAX_COMMANDS    24U

/** 兜底命令表容量(Name == NULL); 注册失败会让 App_Init 直接返回 APP_ERR_HW。 */
#define SVC_NET_MAX_FALLBACK    8U

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                     */
/* -------------------------------------------------------------------------- */

/** 网络组件初始化参数(app 从 app_config.h 填)。 */
typedef struct {
    uint8_t  UseDhcp;                       /**< 1 = DHCP, 0 = 静态地址       */
    uint16_t TcpPort;                       /**< TCP 服务端口                 */
    uint32_t DhcpTimeoutMs;                 /**< DHCP 等待多久后回落静态      */
} Svc_Net_ConfigTypeDef;

/** 回话句柄 —— 不透明, 处理函数只把它原样传给 Svc_Net_Reply()。 */
typedef struct {
    void *Handle;
} Svc_Net_ReplyTypeDef;

/**
 * @brief  命令处理函数。
 * @param  Arg  : 注册时给的上下文。
 * @param  Data : 整条报文的原始字节(尾部已去掉 \r\n 空格)。
 * @return 非 0 = 已处理(分发到此结束), 0 = 交给下一个。
 */
typedef int (*Svc_Net_CmdHandlerTypeDef)(void *Arg, const uint8_t *Data, uint16_t Len,
                                         Svc_Net_ReplyTypeDef *R);

/** 一条命令。Name == NULL 表示兜底命令。 */
typedef struct {
    const char                *Name;        /**< 整行精确匹配的名字, NULL=兜底 */
    Svc_Net_CmdHandlerTypeDef  Handler;     /**< 处理函数                     */
    void                      *Arg;         /**< 传给处理函数的上下文         */
    const char                *Help;        /**< 一行说明(给人看)             */
} Svc_Net_CmdTypeDef;

/** 给上层看的只读快照(32 位对齐读写, 不需要加锁)。 */
typedef struct {
    char     Ip[16];                        /**< 当前 IP(网络任务刷新)        */
    uint8_t  AddrSource;                    /**< 0=等 DHCP 1=DHCP 2=静态      */
    uint8_t  LinkUp;                        /**< 链路是否 up                   */
    uint16_t SpeedMbps;                     /**< 协商速率                      */
    uint8_t  Clients;                       /**< TCP 客户端数量                */
    uint32_t TcpCmds;                       /**< 处理过的命令数                */
    uint32_t Drops;                         /**< 队列满丢掉的广播数            */
} Svc_Net_SnapshotTypeDef;

/** 一条要广播给所有 TCP 客户端的文本(队列元素)。 */
typedef struct {
    uint16_t Len;                           /**< 有效字节数(含 \r\n)          */
    char     Text[SVC_NET_MSG_MAX];          /**< 文本内容                     */
} Svc_Net_MsgTypeDef;

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief  初始化 lwIP + GEM0 + 广播队列 + 挂回调。
 * @note   必须最先初始化: xemac_add() 会做第一次 GIC 初始化并清空处理器表,
 *         任何"要连中断"的模块都得排在它后面。
 */
Svc_StatusTypeDef Svc_Net_Init(const Svc_Net_ConfigTypeDef *Cfg);

/** @brief  初始化好了没有(1 = 好了)。应用层据此决定要不要建任务。 */
uint8_t Svc_Net_IsReady(void);

/** @brief  net 任务入口(lwIP 的唯一拥有者: 喂栈 + 广播队列 + 收发命令); 任务由应用层创建。 */
void Svc_Net_Task(void *Arg);

/* -------------------------------------------------------------------------- */
/*  注册与回话                                                                 */
/* -------------------------------------------------------------------------- */

/** @brief  注册一批命令(可多次调用累加, 后注册的排在后面)。 */
Svc_StatusTypeDef Svc_Net_RegisterCommands(const Svc_Net_CmdTypeDef *Table, uint16_t Count);

/** @brief  给发起这条命令的客户端回一行(或多行)文本。 */
Svc_StatusTypeDef Svc_Net_Reply(Svc_Net_ReplyTypeDef *R, const char *Text, uint16_t Len);

/* -------------------------------------------------------------------------- */
/*  给别的组件用的小接口                                                       */
/* -------------------------------------------------------------------------- */

/** @brief  把一行文本排进广播队列(由 net 任务发给所有客户端); Len = 0 表示用 strlen。 */
Svc_StatusTypeDef Svc_Net_SendText(const char *Text, uint16_t Len);

/** @brief  广播一段原始字节(不加结尾符、可含 0x00), 给自定义二进制报文用。 */
Svc_StatusTypeDef Svc_Net_SendBytes(const uint8_t *Data, uint16_t Len);

/** @brief  读一份网口快照。 */
Svc_StatusTypeDef Svc_Net_GetSnapshot(Svc_Net_SnapshotTypeDef *Out);

/**
 * @brief  请求重启 TCP 服务端(断开所有客户端 + 关掉监听再重新监听)。
 * @note   只置一个标志, 真正的 Stop/Start 由 net 任务执行, 所以线程安全、谁都能调。
 *         只动服务端不动网口: 链路和 IP(含 DHCP 租约)不受影响。
 */
Svc_StatusTypeDef Svc_Net_RequestRestart(void);

/** @brief  打印网口详细状态(包一层 bsp 的 Enet_PrintStatus)。 */
void Svc_Net_PrintStatus(void);

/* -------------------------------------------------------------------------- */
/*  客户端数量变化通知                                                         */
/* -------------------------------------------------------------------------- */

/** TCP 客户端数量变化回调(运行在 net 任务里, 只许做非阻塞的事)。 */
typedef void (*Svc_Net_ClientHookTypeDef)(void *Context, uint8_t Clients);

/** @brief  装上客户端数量变化回调(NULL = 取消)。要在建 net 任务之前注册。 */
Svc_StatusTypeDef Svc_Net_SetClientHook(Svc_Net_ClientHookTypeDef Fn, void *Context);

/*
 * 接收回调 / 链路回调 / 整行比较都是组件内部用的: 前两个在 service_net.c 里 static,
 * 比较函数在 net_cmd.h 里声明。
 */

#endif /* SERVICE_NET_H */
