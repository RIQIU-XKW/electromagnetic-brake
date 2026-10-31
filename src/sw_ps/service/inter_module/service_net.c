/**
 *******************************************************************************
 * @file    service_net.c
 * @brief   net 业务组件 —— net 任务(lwIP 独占者)、广播队列、网口快照
 *
 * 为什么必须独占 lwIP: BSP 里的 lwIP 是 NO_SYS = 1 的 RAW API 版本, 它没有任何
 * 锁, 多任务同时调用会直接破坏 PCB 链表.  所以:
 *
 *   - 所有 lwIP 的收发都在本组件的 net 任务里(Enet_Poll() 内部会跑 tcp_recv 等回调);
 *   - 别的组件想发文本, 只调 Svc_Net_SendText() 排队, 由 net 任务广播;
 *   - 别的组件想读网口状态, 读 Svc_Net_GetSnapshot() 刷出来的只读快照。
 *
 * 命令分发在 net_cmd.c 里(注册表), 本文件只管 "任务 + lwIP + 队列 + 快照 + 回话"。
 *******************************************************************************
 */

#define LOG_ENABLE 1   /* 排障期打开: 客户端连上/断开 + 广播失败(过流上报发不出去就靠它) */
#define LOG_TAG "net"
#include "service/monitor_module/log/log.h"

#include "service/inter_module/service_net.h"
#include "service/inter_module/net_cmd.h"
#include "service/inter_module/net_text.h"

#include "osal/osal.h"

#include <string.h>

/* ========================================================================== */
/*  组件状态(全部 private)                                                     */
/* ========================================================================== */

static Enet_HandleTypeDef      gEnet;
static Osal_QueueHandleTypeDef gQNet;           /**< 待广播文本                   */
static uint8_t                 gReady;
static uint8_t                 gStarted;        /**< 1 = net 任务已经在跑          */

/* 只读快照: 由 net 任务刷新, 别的组件随时读(32 位对齐读写, 不需要加锁). */
static char             gIp[16];
static volatile uint8_t  gAddrSource;           /**< 0=等 DHCP 1=DHCP 2=静态      */
static volatile uint8_t  gLinkUp;
static volatile uint16_t gSpeedMbps;
static volatile uint8_t  gClients;
static volatile uint32_t gTcpCmds;
static volatile uint32_t gDrops;
static uint8_t           gTxFailLogged;   /**< "广播失败"这一行打过没有(避免刷屏) */

/* 客户端数量变化回调(例如"网口在线"呼吸灯就是靠它驱动的) */
static Svc_Net_ClientHookTypeDef gClientHook;
static void                     *gClientHookContext;

/**
 * TCP 服务端重启请求(KEY2 / KEYNET)。
 *
 * 由 Svc_Net_RequestRestart() 置位 —— 那个函数可能在**任何任务**里被调(按键任务、
 * 命令处理函数), 而**所有 lwIP 调用都必须在 net 任务里**(本组件的基本约定), 所以
 * 这里只传一个标志, 真正的 Stop/Start 由 net 任务在下一次循环里做。
 *
 * 用 volatile 的 8 位量就够了: 单字节写在 Cortex-A9 上是原子的, 而且这里只需要
 * "最终会被看到", 不需要"立刻"。
 */
static volatile uint8_t  gRestartReq;

/* ========================================================================== */
/*  内部函数(static)                                                           */
/* ========================================================================== */

/**
 * @brief  刷新给其它组件看的网口快照.
 *
 * 快照的目的: 别的组件需要知道 "链路通不通 / 有几个客户端", 但它们不能去读
 * lwIP 的内部结构(那是 net 任务的私有数据).  这里由 net 任务定期同步一次.
 */
static void Svc_Net_RefreshSnapshot(void)
{
    Enet_LinkInfoTypeDef link;
    uint8_t              clients;

    if (Enet_GetLinkInfo(&gEnet, &link) == ENET_OK) {
        gLinkUp    = (uint8_t)((link.Up != 0U) ? 1U : 0U);
        gSpeedMbps = (uint16_t)link.Speed;
    }

    clients = (uint8_t)Enet_Tcp_GetActiveChannels(&gEnet);

    /*
     * 客户端数**真的变了**才通知一次(0 <-> >=1 就是"上位机连上/断开")。
     * 这里不加锁: gClients 是 volatile 的 8 位量, 只有 net 任务会写。
     */
    if (clients != gClients) {
        /*
         * ⚠ 这一行是现场判断"上位机到底连着没有"的关键: 过流上报是**广播**给客户端的,
         *   一个客户端都没有时发送照样"成功"(没人听不算错) —— 于是"板子报了但界面没反应"
         *   最常见的真相就是**那一刻根本没人在听**(没连/连错 IP/中途掉了)。
         *   日志里只要能看到 "clients 1 -> 0" 出现在过流前后, 就不用再怀疑固件。
         */
        LOG_I("clients %u -> %u\r\n", (unsigned)gClients, (unsigned)clients);

        gClients = clients;

        if (gClientHook != NULL) {
            gClientHook(gClientHookContext, clients);
        }
    }

    /* 地址: 0.0.0.0 表示还在等 DHCP; 有地址时区分是租来的还是静态回落的 */
    Enet_Netif_FormatIp(&gEnet.Netif.ip_addr, gIp, sizeof(gIp));

    if (strcmp(gIp, "0.0.0.0") == 0) {
        gAddrSource = (gEnet.Config.UseDhcp != 0U) ? 0U : 2U;
    } else {
        gAddrSource = (gEnet.Config.UseDhcp != 0U) ? 1U : 2U;
    }
}

/** @brief  net 任务: 喂 lwIP + 广播队列 + 刷新快照(创建由应用层负责). */
/* 内部回调: 在 Svc_Net_Init() 里注册给 enet, 定义在文件末尾 */
static Enet_StatusTypeDef Svc_Net_OnTcp(void *Context, const uint8_t *Data, uint16_t Len,
                                        Enet_TcpChannelTypeDef *Channel);
static void Svc_Net_OnLink(void *Context, const Enet_LinkInfoTypeDef *Link);

void Svc_Net_Task(void *Arg)
{
    Svc_Net_MsgTypeDef msg;
    uint32_t           period = Osal_MsToTicks(SVC_NET_PERIOD_MS);

    (void)Arg;

    if (period == 0U) {
        period = 1U;                        /* 1 个 tick 是调度器的最小时间片 */
    }

    LOG_I("task started (lwIP owner, %u tick = %u ms period)\r\n",
          (unsigned)period, (unsigned)Osal_TicksToMs(period));

    gStarted = 1U;

    for (;;) {
        /* 1. 喂 lwIP: 协议定时器 + MAC 收包 + TCP 回调(命令在这里被分发) */
        Enet_Poll(&gEnet);

        /* 2. 有人要求重起 TCP 服务端 -> 在这里做(只有 net 任务能动 lwIP) */
        if (gRestartReq != 0U) {
            gRestartReq = 0U;

            LOG_W("restarting the tcp server (KEY2/KEYNET): dropping clients + re-listen\r\n");

            /*
             * Stop 会: 摘掉 accept 回调 -> 关掉监听 pcb -> 关掉所有客户端通道。
             * Start 会重新 new/bind/listen/accept。
             * **只动 TCP 服务端, 不碰网口** —— 链路和 IP(含 DHCP 租约)都不受影响,
             * 所以断开窗口就是这两句的执行时间。
             */
            Enet_Tcp_Stop(&gEnet);

            if (Enet_Tcp_Start(&gEnet) == ENET_OK) {
            } else {
                LOG_E("tcp server restart FAILED - the port is closed until the next try\r\n");
            }
        }

        /* 3. 把别的组件要广播的文本发给所有已连接的客户端 */
        while (Osal_QueueReceive(gQNet, &msg, OSAL_WAIT_NOWAIT) == OSAL_OK) {
            if (Enet_Tcp_Broadcast(&gEnet, msg.Text, msg.Len) != ENET_OK) {
                gDrops++;

                /*
                 * ⚠ 上报发不出去在原来这里是**完全静默**的(只累加 gDrops) —— 现场就会
                 *   变成"板子说报了、界面什么都没有", 查无可查。这里补一行日志, 但只在
                 *   **从成功变失败**的那一次打: 发送窗口满会连续失败, 每 10 ms 一行同样
                 *   会把串口(和别的日志)刷爆。恢复了再打一行, 两头都有交代。
                 */
                if (gTxFailLogged == 0U) {
                    gTxFailLogged = 1U;
                    LOG_E("broadcast FAILED (client send window full?) drops=%u\r\n",
                          (unsigned)gDrops);
                }
            } else if (gTxFailLogged != 0U) {
                gTxFailLogged = 0U;
                LOG_I("broadcast ok again (drops=%u)\r\n", (unsigned)gDrops);
            }
        }

        /* 4. 刷新给其它组件看的快照 */
        Svc_Net_RefreshSnapshot();

        Osal_DelayMs(SVC_NET_PERIOD_MS);
    }
}

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

Svc_StatusTypeDef Svc_Net_Init(const Svc_Net_ConfigTypeDef *Cfg)
{
    Enet_ConfigTypeDef cfg;
    const char        *addrSrc;

    if ((Cfg == NULL) || (Cfg->TcpPort == 0U)) {
        return SVC_ERR_PARAM;
    }

    /* ---- 1. lwIP + GEM0(必须最先: 第一次 GIC 初始化在这里) --------------- */
    Enet_Config_LoadDefaults(&cfg);
    cfg.UseDhcp  = (Cfg->UseDhcp != 0U) ? 1U : 0U;
    cfg.EchoPort = Cfg->TcpPort;

    /* DhcpTimeoutTicks 的单位是 500 ms 的慢速服务次数(见 bsp/enet/enet_cfg.h). */
    cfg.DhcpTimeoutTicks = Cfg->DhcpTimeoutMs / 500U;
    if (cfg.DhcpTimeoutTicks == 0U) {
        cfg.DhcpTimeoutTicks = 1U;
    }

    if (Enet_Init(&gEnet, &cfg) != ENET_OK) {
        return SVC_ERR_HW;
    }

    if (Enet_Start(&gEnet) != ENET_OK) {
        return SVC_ERR_HW;
    }

    /* ---- 2. 广播队列 ----------------------------------------------------- */
    gQNet = Osal_QueueCreate(SVC_NET_Q_LEN, sizeof(Svc_Net_MsgTypeDef));
    if (gQNet == NULL) {
        LOG_E("broadcast queue creation failed - increase configTOTAL_HEAP_SIZE\r\n");
        return SVC_ERR_HW;
    }

    /* ---- 3. 挂回调(都在 net 任务上下文里跑) ----------------------------- */
    Enet_Tcp_SetReceiveHook(&gEnet, Svc_Net_OnTcp, NULL);
    Enet_SetLinkHook(&gEnet, Svc_Net_OnLink, NULL);

    Svc_Net_RefreshSnapshot();
    gReady = 1U;

    /* 只有下面这行日志用得到: 关掉日志时用 (void) 避免未使用告警(本工程的老写法) */
    addrSrc = (Cfg->UseDhcp != 0U) ? "dhcp" : "static";
    (void)addrSrc;

    LOG_I("net ready: tcp %u, address %s\r\n",
          (unsigned)Cfg->TcpPort, addrSrc);

    return SVC_OK;
}

uint8_t Svc_Net_IsReady(void)
{
    return gReady;
}

/* ========================================================================== */
/*  回话 / 广播 / 快照                                                         */
/* ========================================================================== */

Svc_StatusTypeDef Svc_Net_Reply(Svc_Net_ReplyTypeDef *R, const char *Text, uint16_t Len)
{
    Enet_TcpChannelTypeDef *channel;

    if ((R == NULL) || (R->Handle == NULL) || (Text == NULL)) {
        return SVC_ERR_PARAM;
    }

    if (Len == 0U) {
        Len = (uint16_t)strlen(Text);
    }

    channel = (Enet_TcpChannelTypeDef *)R->Handle;

    if (Enet_Tcp_Send(channel, Text, Len) != ENET_OK) {
        return SVC_ERR_HW;
    }


    return SVC_OK;
}

Svc_StatusTypeDef Svc_Net_SendText(const char *Text, uint16_t Len)
{
    Svc_Net_MsgTypeDef msg;

    if ((Text == NULL) || (gQNet == NULL)) {
        return SVC_ERR_PARAM;
    }

    if (Len == 0U) {
        Len = (uint16_t)strlen(Text);
    }

    if (Len >= (uint16_t)SVC_NET_MSG_MAX) {
        Len = (uint16_t)(SVC_NET_MSG_MAX - 1U);
    }

    msg.Len = Len;
    (void)memcpy(msg.Text, Text, Len);
    msg.Text[Len] = '\0';

    if (Osal_QueueSend(gQNet, &msg, OSAL_WAIT_NOWAIT) != OSAL_OK) {
        gDrops++;
        return SVC_ERR_FULL;
    }

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Net_SendBytes(const uint8_t *Data, uint16_t Len)
{
    Svc_Net_MsgTypeDef msg;

    if ((Data == NULL) || (gQNet == NULL) || (Len == 0U)) {
        return SVC_ERR_PARAM;
    }

    if (Len >= (uint16_t)SVC_NET_MSG_MAX) {
        Len = (uint16_t)(SVC_NET_MSG_MAX - 1U);
    }

    /*
     * 和 SendText 走同一条路: 排队 -> net 任务广播给所有客户端。
     * ⚠ 这里**不加结尾符、不做任何文本处理** —— 二进制报文(0xB0/0xC0..)必须原样出去。
     *   拷贝用 memcpy 而不是字符串操作, 因为载荷里可能有 0x00。
     */
    msg.Len = Len;
    (void)memcpy(msg.Text, Data, Len);
    msg.Text[Len] = '\0';               /* 只为调试打印时能当字符串看 */

    if (Osal_QueueSend(gQNet, &msg, OSAL_WAIT_NOWAIT) != OSAL_OK) {
        gDrops++;
        return SVC_ERR_FULL;
    }

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Net_GetSnapshot(Svc_Net_SnapshotTypeDef *Out)
{
    if (Out == NULL) {
        return SVC_ERR_PARAM;
    }

    (void)memcpy(Out->Ip, gIp, sizeof(Out->Ip));
    Out->AddrSource   = gAddrSource;
    Out->LinkUp       = gLinkUp;
    Out->SpeedMbps    = gSpeedMbps;
    Out->Clients      = gClients;
    Out->TcpCmds      = gTcpCmds;
    Out->Drops        = gDrops;

    return SVC_OK;
}

Svc_StatusTypeDef Svc_Net_RequestRestart(void)
{
    /*
     * 只置标志。判"组件起来了没有": 没起来的话没人会去消费这个标志, 直接回错误比
     * 让调用者以为成功了强。
     */
    if ((gReady == 0U) || (gStarted == 0U)) {
        return SVC_ERR_STATE;
    }

    gRestartReq = 1U;

    return SVC_OK;
}

void Svc_Net_PrintStatus(void)
{
    Enet_PrintStatus(&gEnet);
}

Svc_StatusTypeDef Svc_Net_SetClientHook(Svc_Net_ClientHookTypeDef Fn, void *Context)
{
    /*
     * 只在 net 任务还没跑起来的时候换回调: 任务一跑就会从快照里调它,
     * 半路换指针可能让回调读到一半的上下文。
     */
    if (gStarted != 0U) {
        return SVC_ERR_STATE;
    }

    gClientHook        = Fn;
    gClientHookContext = Context;

    return SVC_OK;
}

/* ========================================================================== */
/*  回调                                                                       */
/* ========================================================================== */

static void Svc_Net_OnLink(void *Context, const Enet_LinkInfoTypeDef *Link)
{
    (void)Context;

    if (Link == NULL) {
        return;
    }

    LOG_I("ethernet link %s %u Mbps\r\n",
          Enet_Phy_LinkStatusName(Link->Status), (unsigned)Link->Speed);
}

static Enet_StatusTypeDef Svc_Net_OnTcp(void *Context, const uint8_t *Data, uint16_t Len,
                                 Enet_TcpChannelTypeDef *Channel)
{
    Svc_Net_ReplyTypeDef reply;
    char                 text[SVC_NET_MSG_MAX];
    uint16_t             textLen;

    (void)Context;

    if ((Data == NULL) || (Channel == NULL)) {
        return ENET_OK;
    }

    /*
     * 去掉行尾的空白/换行再匹配.  命令是整条报文精确比较的, 而网络调试助手、
     * nc、telnet 默认都会给每条报文补 \r\n —— 不处理的话 "PWMID\r\n" 匹配不上,
     * 会被当成不认识的命令回一行提示, 调试时会很迷惑.
     */
    while ((Len != 0U) &&
           ((Data[Len - 1U] == (uint8_t)'\r') || (Data[Len - 1U] == (uint8_t)'\n') ||
            (Data[Len - 1U] == (uint8_t)' '))) {
        Len--;
    }


    if (Len == 0U) {
        return ENET_OK;                     /* 空行: 什么也不做              */
    }

    gTcpCmds++;
    reply.Handle = Channel;

    /* ---- 内置命令: PING(网口接口自检) ----------------------------------- */
    if (Svc_Net_IsCommand(Data, Len, "PING") != 0) {
        (void)Enet_Tcp_Send(Channel, "pong\r\n", 6U);
        return ENET_OK;
    }

    /* ---- 注册表(led / pwm / app 注册进来的命令) ------------------------- */
    if (Svc_Net_Cmd_Dispatch(Data, Len, &reply) != 0) {
        return ENET_OK;
    }

    /* ---- 都不认识: 回一行用法提示 --------------------------------------- */
    textLen = Svc_Text_AppendStr(text, 0U, (uint16_t)sizeof(text), "unknown (");
    textLen = Svc_Text_AppendUint(text, textLen, (uint16_t)sizeof(text), (uint32_t)Len);
    /*
     * ⚠ 这条提示 2026-10-16 更新过: 原来还写着 PWMID / PWMTST —— 那些命令早随 PWM 命令表
     *   一起删了, 提示里留着只会误导现场(照着发过去还是 unknown)。现在列出**当前真有的三类**:
     *   文本命令 + 三套单字节协议(制动/发射小车/采样)。
     */
    textLen = Svc_Text_AppendStr(text, textLen, (uint16_t)sizeof(text),
                                 " bytes); try STATUS / PING (text), or single bytes: "
                                 "0xA0..0xA5 brake / 0xD0..0xD6 mover / 0xE0..0xE2 samp\r\n");
    (void)Enet_Tcp_Send(Channel, text, textLen);

    return ENET_OK;
}
