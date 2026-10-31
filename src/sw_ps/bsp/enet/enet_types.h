/**
 *******************************************************************************
 * @file    enet_types.h
 * @brief   Ethernet module — public data types
 *
 * The module follows the handle/instance style used by the rest of the
 * workspace drivers: the caller owns an @ref Enet_HandleTypeDef, passes its
 * address to every API call, and the module keeps all of its state inside
 * that structure (no hidden globals except the platform tick counter).
 *******************************************************************************
 */

#ifndef ENET_TYPES_H
#define ENET_TYPES_H

#include <stdint.h>
#include <stddef.h>

#include "lwip/netif.h"
#include "lwip/ip_addr.h"

#include "enet_cfg.h"

/* -------------------------------------------------------------------------- */
/* Return codes                                                               */
/* -------------------------------------------------------------------------- */

typedef enum {
    ENET_OK             =  0,   /**< Operation succeeded                     */
    ENET_ERR_PARAM      = -1,   /**< NULL / out-of-range argument            */
    ENET_ERR_STATE      = -2,   /**< Call is invalid in the current state    */
    ENET_ERR_HW         = -3,   /**< Peripheral did not initialise           */
    ENET_ERR_NETIF      = -4,   /**< xemac_add() failed (MAC / PHY)          */
    ENET_ERR_NO_MEMORY  = -5,   /**< lwIP pool exhausted                     */
    ENET_ERR_BIND       = -6,   /**< tcp_bind() / tcp_listen() failed        */
    ENET_ERR_DHCP       = -7,   /**< No lease and no static fall-back        */
    ENET_ERR_TIMEOUT    = -8    /**< Hardware handshake timed out            */
} Enet_StatusTypeDef;

/* -------------------------------------------------------------------------- */
/* Runtime state                                                              */
/* -------------------------------------------------------------------------- */

typedef enum {
    ENET_STATE_RESET       = 0, /**< Handle zeroed, nothing initialised      */
    ENET_STATE_INITIALIZED,     /**< MAC added, clock fixed, IRQ enabled     */
    ENET_STATE_DHCP_WAIT,       /**< Interface up, waiting for a lease       */
    ENET_STATE_READY,           /**< Address valid, services are running     */
    ENET_STATE_ERROR            /**< Fatal error, only Enet_Stop() is valid  */
} Enet_StateTypeDef;

/** Where the GEM0 reference clock is taken from. */
typedef enum {
    ENET_CLKSRC_AUTO = 0,       /**< Use the design setting, repair if unusable */
    ENET_CLKSRC_IO_PLL,         /**< Force the internal IO PLL path          */
    ENET_CLKSRC_EXTERNAL_EMIO   /**< Keep the PL (EMIO) clock, never touch it */
} Enet_ClockSourceTypeDef;

typedef enum {
    ENET_SPEED_UNKNOWN = 0,
    ENET_SPEED_10      = 10,
    ENET_SPEED_100     = 100,
    ENET_SPEED_1000    = 1000
} Enet_SpeedTypeDef;

/** Mirrors the lwIP port's ::ethernet_link_status enum. */
typedef enum {
    ENET_LINK_UNDEFINED = 0,
    ENET_LINK_UP,
    ENET_LINK_DOWN,
    ENET_LINK_NEGOTIATING
} Enet_LinkStatusTypeDef;

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t Addr[6];                /**< MAC address (locally administered)  */
} Enet_MacAddressTypeDef;

/**
 * @brief  Static configuration of one Ethernet instance.
 *         Fill it with Enet_Config_LoadDefaults() and override what differs.
 */
typedef struct {
    Enet_MacAddressTypeDef  MacAddress;      /**< MAC programmed into GEM0     */
    uint32_t                EmacBaseAddr;    /**< MAC base address             */
    uint16_t                EchoPort;        /**< TCP service port             */
    uint8_t                 UseDhcp;         /**< Start a DHCP client          */
    uint8_t                 DhcpFallbackToStatic; /**< Static IP on timeout    */
    uint32_t                DhcpTimeoutTicks;/**< Lease timeout, 500 ms ticks  */
    uint8_t                 ClockDiv0;       /**< GEM0 SLCR divisor 0 (IO PLL) */
    Enet_ClockSourceTypeDef ClockSource;     /**< GEM0 clock source policy     */
    ip4_addr_t              StaticIp;        /**< Address used w/o DHCP        */
    ip4_addr_t              StaticNetmask;   /**< Netmask used w/o DHCP        */
    ip4_addr_t              StaticGateway;   /**< Gateway used w/o DHCP        */
} Enet_ConfigTypeDef;

/* -------------------------------------------------------------------------- */
/* Runtime information                                                        */
/* -------------------------------------------------------------------------- */

typedef struct {
    Enet_LinkStatusTypeDef Status;      /**< Last status reported by the PHY  */
    uint8_t                Up;          /**< 1 while the link is up           */
    Enet_SpeedTypeDef      Speed;       /**< Negotiated speed                 */
    uint32_t               UpTimeMs;    /**< Milliseconds spent with link up   */
    uint32_t               UpCount;     /**< Number of up transitions         */
    uint32_t               DownCount;   /**< Number of down transitions       */
    uint32_t               SpeedChanges;/**< Number of speed changes          */
} Enet_LinkInfoTypeDef;

typedef struct {
    uint32_t Polls;         /**< Enet_Poll() invocations                      */
    uint32_t RxPackets;     /**< Frames handed over to lwIP                   */
    uint32_t TcpAccepted;   /**< Accepted TCP connections                     */
    uint32_t TcpRejected;   /**< Connections dropped (channel pool empty)     */
    uint32_t TcpClosed;     /**< Closed TCP connections                       */
    uint32_t TcpRxBytes;    /**< Bytes received by the TCP service            */
    uint32_t TcpTxBytes;    /**< Bytes echoed back                            */
    uint32_t TcpErrors;     /**< tcp_write() / connection errors              */
    uint32_t DhcpLeases;    /**< DHCP leases acquired                         */
    uint32_t DhcpTimeouts;  /**< DHCP timeouts (static fall-back taken)       */
    uint32_t ClockFixups;   /**< GEM0 reference-clock corrections applied     */
    uint32_t PhySpeedFixups;/**< Reference-clock updates after a link change  */
} Enet_StatsTypeDef;

/* -------------------------------------------------------------------------- */
/* TCP channel and user hooks                                                 */
/* -------------------------------------------------------------------------- */

struct Enet_Handle;             /* forward declaration (see below) */

/**
 * @brief  One TCP connection.  Instances live in the handle's static pool and
 *         are handed to lwIP as tcp_arg(), so every callback receives a
 *         pointer to the channel that owns the connection.
 */
typedef struct {
    struct tcp_pcb      *Pcb;       /**< lwIP control block (NULL = free)     */
    struct Enet_Handle  *Owner;     /**< Back-pointer to the owning handle    */
    uint32_t             RxBytes;   /**< Bytes received on this connection    */
    uint32_t             TxBytes;   /**< Bytes sent on this connection        */
    uint32_t             RxSegments;/**< Segments received                    */
    uint8_t              InUse;     /**< Pool slot occupied                   */
} Enet_TcpChannelTypeDef;

/**
 * @brief  Application receive hook.
 *         Return ENET_OK to tell the service that the payload was consumed;
 *         any other value makes the default echo behaviour run instead.
 *         @p data is only valid for the duration of the call.
 */
typedef Enet_StatusTypeDef (*Enet_TcpReceiveHook)(void *context,
                                                  const uint8_t *data,
                                                  uint16_t len,
                                                  Enet_TcpChannelTypeDef *channel);

/** Called whenever the PHY link state or speed changes. */
typedef void (*Enet_LinkEventHook)(void *context, const Enet_LinkInfoTypeDef *link);

/* -------------------------------------------------------------------------- */
/* Handle                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief  各类周期工作下一次该做的时间(毫秒).
 *
 * lwIP 在 NO_SYS = 1 模式下自己不跑定时器(BSP 里 NO_SYS_NO_TIMERS = 1), 需要
 * 应用按时去调它的 tcp_fasttmr() 等函数.  这里用 "下一次该做的时间" 记录每一
 * 类工作的节拍, 晚到了就直接做一次并把下次时间顺延, 不做补课, 逻辑简单且不会
 * 因为一次长时间阻塞而爆发式地补跑一堆定时器.
 */
typedef struct {
    uint32_t FastMs;        /**< tcp_fasttmr()                每 250 ms        */
    uint32_t SlowMs;        /**< tcp_slowtmr() + DHCP fine    每 500 ms        */
    uint32_t CoarseMs;      /**< dhcp_coarse_tmr()            每 60 s          */
    uint32_t LinkMs;        /**< PHY 链路/速率轮询            每 1 s           */
    uint32_t RecoveryMs;    /**< GEM0 收包路径恢复            每 100 s         */
} Enet_TimerTypeDef;

/**
 * @brief  Complete state of one Ethernet instance.
 *
 *          Config      caller configuration (defaults + overrides)
 *          Link        live PHY information
 *          Stats       counters, useful for telemetry
 *          State       lifecycle state machine
 *          Netif       lwIP network interface owned by this handle
 *          Channels    static TCP channel pool (no dynamic allocation)
 *          OnTcpReceive/OnLinkChange   optional application hooks
 */
typedef struct Enet_Handle {
    Enet_ConfigTypeDef     Config;
    Enet_LinkInfoTypeDef   Link;
    Enet_StatsTypeDef      Stats;
    volatile Enet_StateTypeDef State;

    struct netif           Netif;                       /**< lwIP interface   */
    struct tcp_pcb        *ListenPcb;                   /**< Listening socket */

    Enet_TcpChannelTypeDef Channels[ENET_TCP_MAX_CHANNELS];

    Enet_TimerTypeDef      Timers;          /**< 周期工作的时间表             */
    uint32_t               DhcpTimeoutCnt;  /**< Remaining lease timeout      */
    uint8_t                AddrReported;    /**< Addresses already printed    */

    Enet_TcpReceiveHook    OnTcpReceive;    /**< Optional receive hook        */
    Enet_LinkEventHook     OnLinkChange;    /**< Optional link hook           */
    void                  *TcpContext;      /**< Context of OnTcpReceive      */
    void                  *LinkContext;     /**< Context of OnLinkChange      */
} Enet_HandleTypeDef;

#endif /* ENET_TYPES_H */
