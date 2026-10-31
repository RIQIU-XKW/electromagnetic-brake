/**
 *******************************************************************************
 * @file    enet.c
 * @brief   Ethernet module — lifecycle state machine and public API
 *
 * 把 lwIP 需要的周期工作挂在毫秒时间表上(时间基准见 enet_platform.c):
 *
 *   周期         工作
 *   -----------  --------------------------------------------------------
 *   250 ms       tcp_fasttmr()                     (lwIP 硬性要求的节拍)
 *   500 ms       tcp_slowtmr(), dhcp_fine_tmr(), DHCP 租约倒计时, 网口维护
 *   60 s         dhcp_coarse_tmr()
 *   1 s          PHY 链路 / 速率轮询
 *   100 s        SI-692601 问题的补救: 复位 GEM0 收包路径
 *
 * 所有 lwIP 调用都发生在调用者的上下文里(也就是 app 的 net 任务), 没有任何
 * lwIP 函数会被中断重入.
 *******************************************************************************
 */

#include "enet.h"

#include <string.h>

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/dhcp.h"
/* log tag: LOG_I/W/E from this file print with the [enet] prefix */
#define LOG_ENABLE 0   /* 本模块日志默认关闭; 需要时改成 1 */
#define LOG_TAG "enet"
#include "service/monitor_module/log/log.h"

#include "xil_printf.h"

/* --------------------------------------------------------------------------
 * lwIP keeps the fast/slow TCP timers out of its public headers.  With
 * NO_SYS = 1 the application owns the timing, so they have to be declared
 * here (the Xilinx lwIP examples do the same).
 * ------------------------------------------------------------------------ */
extern void tcp_fasttmr(void);
extern void tcp_slowtmr(void);

/* ========================================================================== */
/*  周期工作                                                                    */
/* ========================================================================== */

/** 各类周期工作的第一次执行时间(毫秒), 从 "现在 + 一个周期" 开始. */
static void Enet_ScheduleTimers(Enet_HandleTypeDef *h)
{
    uint32_t now = Enet_Platform_GetTicks();

    h->Timers.FastMs     = now + ENET_FAST_TIMER_MS;
    h->Timers.SlowMs     = now + ENET_SLOW_TIMER_MS;
    h->Timers.CoarseMs   = now + ENET_DHCP_COARSE_MS;
    h->Timers.LinkMs     = now + ENET_LINK_POLL_MS;
    h->Timers.RecoveryMs = now + ENET_RX_RECOVERY_MS;
}

/**
 * @brief  到点的周期工作各做一次.
 * @param  h : Started handle.
 *
 * 用 "下一次该做的时间" 判断, 而不是用 "过了几个节拍": 晚到了就做一次并把下次
 * 时间顺延, 不做补课.  好处是一次长时间阻塞(比如慢速串口打印)之后不会爆发式
 * 地补跑一堆定时器, 逻辑也更好读.
 */
static void Enet_ServiceTimers(Enet_HandleTypeDef *h)
{
    uint32_t now = Enet_Platform_GetTicks();

    /* ---- 250 ms: lwIP 快速定时器 ---------------------------------------- */
    if ((int32_t)(now - h->Timers.FastMs) >= 0) {
        h->Timers.FastMs = now + ENET_FAST_TIMER_MS;
        tcp_fasttmr();

        if (h->Link.Up != 0U) {
            h->Link.UpTimeMs += ENET_FAST_TIMER_MS;
        }
    }

    /* ---- 500 ms: 慢速定时器 + DHCP 精细定时器 + 网口维护 ---------------- */
    if ((int32_t)(now - h->Timers.SlowMs) >= 0) {
        h->Timers.SlowMs = now + ENET_SLOW_TIMER_MS;
        tcp_slowtmr();

#if LWIP_DHCP
        dhcp_fine_tmr();
#endif
        (void)Enet_Netif_Service(h);
    }

    /* ---- 60 s: DHCP 粗定时器 -------------------------------------------- */
    if ((int32_t)(now - h->Timers.CoarseMs) >= 0) {
        h->Timers.CoarseMs = now + ENET_DHCP_COARSE_MS;

#if LWIP_DHCP
        dhcp_coarse_tmr();
#endif
    }

    /* ---- 1 s: PHY 链路与速率 -------------------------------------------- */
    if ((int32_t)(now - h->Timers.LinkMs) >= 0) {
        h->Timers.LinkMs = now + ENET_LINK_POLL_MS;
        (void)Enet_Phy_UpdateLink(h);
    }

    /* ---- 100 s: GEM0 收包路径恢复 --------------------------------------- */
    if ((int32_t)(now - h->Timers.RecoveryMs) >= 0) {
        h->Timers.RecoveryMs = now + ENET_RX_RECOVERY_MS;
        Enet_Phy_RecoverRxPath(h);
    }
}

/* ========================================================================== */
/*  配置                                                                        */
/* ========================================================================== */

void Enet_Config_LoadDefaults(Enet_ConfigTypeDef *cfg)
{
    static const uint8_t mac[6]  = ENET_MAC_ADDRESS;
    static const uint8_t ip[4]   = ENET_IPV4_ADDRESS;
    static const uint8_t mask[4] = ENET_IPV4_NETMASK;
    static const uint8_t gw[4]   = ENET_IPV4_GATEWAY;

    if (cfg == NULL) {
        return;
    }

    (void)memset(cfg, 0, sizeof(*cfg));
    (void)memcpy(cfg->MacAddress.Addr, mac, sizeof(mac));

    IP4_ADDR(&cfg->StaticIp,      ip[0],   ip[1],   ip[2],   ip[3]);
    IP4_ADDR(&cfg->StaticNetmask, mask[0], mask[1], mask[2], mask[3]);
    IP4_ADDR(&cfg->StaticGateway, gw[0],   gw[1],   gw[2],   gw[3]);

    cfg->EmacBaseAddr         = ENET_GEM0_BASEADDR;
    cfg->EchoPort             = ENET_ECHO_PORT;
    cfg->UseDhcp              = ENET_USE_DHCP;
    cfg->DhcpFallbackToStatic = ENET_DHCP_FALLBACK_STATIC;
    cfg->DhcpTimeoutTicks     = ENET_DHCP_TIMEOUT_TICKS;
    cfg->ClockSource          = ENET_CLKSRC_AUTO;
    cfg->ClockDiv0            = ENET_GEM_CLK_DIV0_IO_PLL;
}

/* ========================================================================== */
/*  对外接口                                                                      */
/* ========================================================================== */

Enet_StatusTypeDef Enet_Init(Enet_HandleTypeDef *h, const Enet_ConfigTypeDef *cfg)
{
    Enet_StatusTypeDef status;

    if (h == NULL) {
        return ENET_ERR_PARAM;
    }

    (void)memset(h, 0, sizeof(*h));
    Enet_Config_LoadDefaults(&h->Config);

    if (cfg != NULL) {
        h->Config = *cfg;                   /* caller takes full ownership    */
    }

    h->State      = ENET_STATE_RESET;
    h->Link.Status = ENET_LINK_UNDEFINED;

    LOG_I("--- Zynq-7000 GEM0, lwIP 2.2.0 (%s) ---\r\n",
               (h->Config.UseDhcp != 0U) ? "DHCP" : "static IP");

    /* ---- 1. 250 ms tick source ------------------------------------------ */
    status = Enet_Platform_Init();
    if (status != ENET_OK) {
        h->State = ENET_STATE_ERROR;
        return status;
    }

    /* ---- 2. lwIP core and MAC: negotiates the PHY speed ----------------- */
    status = Enet_Netif_Attach(h);
    if (status != ENET_OK) {
        h->State = ENET_STATE_ERROR;
        return status;
    }

    /* ---- 3. 时间表: 每类周期工作的下一次执行时间 ------------------------ */
    status = Enet_Platform_EnableInterrupts();
    if (status != ENET_OK) {
        h->State = ENET_STATE_ERROR;
        return status;
    }

    /* ---- 4. GEM0 参考时钟: 按刚刚协商出来的速率配置 ---------------------- */
    (void)Enet_Phy_UpdateLink(h);
    (void)Enet_Phy_ApplyClock(h);

    Enet_ScheduleTimers(h);
    h->State = ENET_STATE_INITIALIZED;

    return ENET_OK;
}

Enet_StatusTypeDef Enet_Start(Enet_HandleTypeDef *h)
{
    Enet_StatusTypeDef status;

    if (h == NULL) {
        return ENET_ERR_PARAM;
    }

    if (h->State != ENET_STATE_INITIALIZED) {
        LOG_I("Enet_Start() called in state %s\r\n", Enet_StateName(h->State));
        return ENET_ERR_STATE;
    }

    /* ---- interface up + address ----------------------------------------- */
    status = Enet_Netif_Start(h);
    if (status != ENET_OK) {
        h->State = ENET_STATE_ERROR;
        return status;
    }

    h->State = (h->Config.UseDhcp != 0U) ? ENET_STATE_DHCP_WAIT : ENET_STATE_READY;

    /* ---- services -------------------------------------------------------- */
    status = Enet_Tcp_Start(h);
    if (status != ENET_OK) {
        h->State = ENET_STATE_ERROR;
        return status;
    }

    return ENET_OK;
}

void Enet_Poll(Enet_HandleTypeDef *h)
{
    if (h == NULL) {
        return;
    }

    if ((h->State == ENET_STATE_RESET) || (h->State == ENET_STATE_ERROR)) {
        return;
    }

    h->Stats.Polls++;

    /* Enet_ServiceTimers() drives Enet_Netif_Service() on the 500 ms tick,
     * which is also the period the DHCP timeout counter is expressed in.
     */
    Enet_ServiceTimers(h);                  /* protocol timers + periodic jobs */
    (void)Enet_Netif_Input(h);              /* MAC queue -> lwIP               */
}

/* ========================================================================== */
/*  查询                                                                        */
/* ========================================================================== */

Enet_StatusTypeDef Enet_GetLinkInfo(const Enet_HandleTypeDef *h,
                                    Enet_LinkInfoTypeDef *info)
{
    if ((h == NULL) || (info == NULL)) {
        return ENET_ERR_PARAM;
    }

    *info = h->Link;

    return ENET_OK;
}

Enet_StatusTypeDef Enet_GetStats(const Enet_HandleTypeDef *h,
                                 Enet_StatsTypeDef *stats)
{
    if ((h == NULL) || (stats == NULL)) {
        return ENET_ERR_PARAM;
    }

    *stats = h->Stats;

    return ENET_OK;
}

void Enet_SetLinkHook(Enet_HandleTypeDef *h, Enet_LinkEventHook hook, void *context)
{
    if (h == NULL) {
        return;
    }

    h->OnLinkChange = hook;
    h->LinkContext  = context;
}

const char *Enet_StateName(Enet_StateTypeDef state)
{
    switch (state) {
        case ENET_STATE_RESET:       return "reset";
        case ENET_STATE_INITIALIZED: return "initialized";
        case ENET_STATE_DHCP_WAIT:   return "dhcp-wait";
        case ENET_STATE_READY:       return "ready";
        case ENET_STATE_ERROR:       return "error";
        default:                     return "?";
    }
}

void Enet_PrintStatus(const Enet_HandleTypeDef *h)
{
    char ip[16];

    if (h == NULL) {
        return;
    }

    Enet_Netif_FormatIp(&h->Netif.ip_addr, ip, sizeof(ip));

    LOG_I("%s | link %s %uM | ip %s | up %us | rx %u | tcp a%u c%u x%u | err %u | clk %u/%u\r\n",
               Enet_StateName(h->State),
               Enet_Phy_LinkStatusName(h->Link.Status),
               (unsigned)h->Link.Speed,
               ip,
               (unsigned)Enet_Platform_TicksToSeconds(h->Link.UpTimeMs),
               (unsigned)h->Stats.RxPackets,
               (unsigned)h->Stats.TcpAccepted,
               (unsigned)h->Stats.TcpClosed,
               (unsigned)Enet_Tcp_GetActiveChannels(h),
               (unsigned)h->Stats.TcpErrors,
               (unsigned)h->Stats.ClockFixups,
               (unsigned)h->Stats.PhySpeedFixups);
}
