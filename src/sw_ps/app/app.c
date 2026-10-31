/**
 *******************************************************************************
 * @file    app.c
 * @brief   应用装配: 组件初始化顺序 + 注册命令地图 + 建各模块的任务
 *
 * 只做三件事: App_Init() 按顺序初始化各组件并注册命令地图;
 *            App_StartTasks() 依次调各模块的 Xxx_App_Start();
 *            "网口在线"指示灯的客户端回调。
 *******************************************************************************
 */

#define LOG_ENABLE 1   /* 上电装配顺序 + 各组件 ready */
#define LOG_TAG "app"
#include "service/monitor_module/log/log.h"

#include "app/app.h"
#include "app/brake_ctl_app.h" /* BrakeCtl_App_Init/Start + 0xA0..0xA5 + 帧命令 */
#include "app/current_app.h"   /* Current_App_Init/Start + 电流指令帧 0xBA..0xAB */
#include "app/inter_app.h"      /* Inter_App_Start + STATUS/按键 + 命令地图 */
#include "app/monitor_app.h"    /* Monitor_App_Start + 0xF0..0xFF */
#include "app/samp_app.h"       /* Samp_App_Start + 0xE0..0xE2 */

#include "service/inter_module/service_net.h"
#include "service/monitor_module/led/service_led.h"
#include "service/brake_module/service_pwm.h"
#include "service/monitor_module/service_monitor.h"

/* 按需插拔的组件(由 app_config.h 的 APP_ENABLE_* 控制) */
#if (APP_ENABLE_RS485 != 0U)
#include "service/Mover_module/service_rs485.h"
#endif
#if (APP_ENABLE_MOTOR != 0U)
#include "service/Mover_module/service_motor.h"
#include "app/Mover_app.h"      /* Mover_App_Start + 0xD0..0xD6 */
#endif
#if (APP_ENABLE_ADS != 0U)
#include "service/Samp_module/service_ads.h"
#endif
#if (APP_ENABLE_EMMC != 0U)
#include "service/Samp_module/service_emmc.h"
#endif
#if (APP_ENABLE_KEY != 0U)
#include "service/inter_module/service_key.h"
#endif

/** @brief  建任务的结果 -> 汇总行里的 "ok" / "FAILED"。 */
static const char *App_StartOk(Svc_StatusTypeDef Status)
{
    return (Status == SVC_OK) ? "ok" : "FAILED";
}

/* ========================================================================== */
/*  网口在线指示(呼吸灯)                                                       */
/* ========================================================================== */

/* 上一次看到的 TCP 客户端数: 用来识别"0 -> >=1"的那一次(上位机刚连上) */
static uint8_t gClientsPrev;

/**
 * @brief  客户端数变化 -> 点灯 + **默认关闭制动**。
 *
 * 点灯: 有客户端按 APP_LED_LINK_FREQ_HZ 呼吸, 断开就熄灭; 只动 APP_LED_LINK_INDEX
 *       这一路, 不影响用户点亮的其它几颗。
 * 制动: 上位机**刚连上**的那一次(0 -> >=1)强制回到"制动关闭"态 —— 默认就该是不制动,
 *       上一次试验留下的输出不许跟着新连接继续跑。
 * 回调在 net 任务里跑: 点灯只往队列塞命令, 关桥只是几次纳秒级寄存器写, 都不阻塞。
 */
static void App_OnClientCount(void *Context, uint8_t Clients)
{
    (void)Context;

#if (APP_LED_LINK_INDICATOR != 0U)
    if (Clients > 0U) {
        (void)Svc_Led_RequestFreq((uint16_t)APP_LED_LINK_FREQ_HZ);
        (void)Svc_Led_RequestEnable((uint8_t)APP_LED_LINK_INDEX, 1U);
    } else {
        (void)Svc_Led_RequestEnable((uint8_t)APP_LED_LINK_INDEX, 0U);
    }
#endif

    if ((gClientsPrev == 0U) && (Clients > 0U)) {
        BrakeCtl_App_ForceOff();
    }

    gClientsPrev = Clients;
}

/* ========================================================================== */
/*  对外接口                                                                   */
/* ========================================================================== */

App_StatusTypeDef App_Init(void)
{
    /* ---- 1. 网络组件(lwIP + GEM0): 必须最先 ----------------------------- */
    Svc_Net_ConfigTypeDef netCfg;

    netCfg.UseDhcp       = APP_NET_USE_DHCP;
    netCfg.TcpPort       = APP_NET_TCP_PORT;
    netCfg.DhcpTimeoutMs = APP_NET_DHCP_TIMEOUT_MS;

    if (Svc_Net_Init(&netCfg) != SVC_OK) {
        LOG_E("network init failed\r\n");
        return APP_ERR_HW;
    }

    /* ---- 2. LED 呼吸灯 --------------------------------------------------- */
    if (Svc_Led_Init() != SVC_OK) {
        LOG_E("led init failed\r\n");
        return APP_ERR_HW;
    }

    /* 注册必须在建 net 任务之前(任务一跑起来就在刷客户端数);
     * 这个钩子同时负责"上位机刚连上 -> 强制关闭制动", 所以去掉了点灯的开关判断 */
    if (Svc_Net_SetClientHook(App_OnClientCount, NULL) != SVC_OK) {
        LOG_W("client hook not installed (no link LED, brake not forced off on connect)\r\n");
    }

    /* ---- 3. 制动控制(五路同步 + 光幕/过流, 顺序不能换: 过流要借 PWM 的句柄) -- */
    if (BrakeCtl_App_Init() != SVC_OK) {
        return APP_ERR_HW;
    }

    /* ---- 3b. 电流闭环(5 路独立 PID): 要 PWM 已起来, 反馈从 ADS 拿 ---------- */
    if (Current_App_Init() != SVC_OK) {
        LOG_E("current loop init failed\r\n");
        return APP_ERR_HW;
    }

    /* ---- 4. 按需插拔的组件: 初始化失败只打一行, 不让整个应用失败 ---------- */
#if (APP_ENABLE_RS485 != 0U)
    if (Svc_Rs485_Init() != SVC_OK) {
        LOG_E("rs485 init failed - the rs485 component is skipped\r\n");
    }
#endif

#if (APP_ENABLE_ADS != 0U)
    /* 只初始化 SPI 控制器; ADS 的配置序列(带延时)由采集任务下发 */
    if (Svc_Ads_Init() != SVC_OK) {
        LOG_E("ads init failed - the ads component is skipped\r\n");
    }
#endif

#if (APP_ENABLE_MOTOR != 0U)
    if (Svc_Motor_Init() != SVC_OK) {
        LOG_E("motor init failed - the mover component is skipped\r\n");
    }

    if (APP_ENABLE_RS485 == 0U) {
        LOG_W("mover component needs the rs485 component - skipped\r\n");
    }
#endif

#if (APP_ENABLE_EMMC != 0U)
    /* 挂采样回调要在采集任务建起来之前做完(建任务在 Samp_App_Start) */
    if (Svc_Emmc_Init() != SVC_OK) {
        LOG_E("emmc init failed - the emmc component is skipped\r\n");
    } else {
#if (APP_ENABLE_ADS != 0U)
        if (Svc_Ads_SetSampleSink(Svc_Emmc_OnSample) != SVC_OK) {
            LOG_W("emmc: could not subscribe to the ads samples\r\n");
        }
#endif
    }
#endif

#if (APP_ENABLE_KEY != 0U)
    {
        Svc_Key_ConfigTypeDef keyCfg;

        keyCfg.PollMs            = APP_KEY_POLL_MS;
        keyCfg.ArmDelayMs        = APP_KEY_ARM_DELAY_MS;
        keyCfg.Debounce          = (uint8_t)APP_KEY_DEBOUNCE;
        keyCfg.ActiveLow         = (uint8_t)APP_KEY_ACTIVE_LOW;
        keyCfg.Key2RestartEnable = (uint8_t)APP_KEY2_RESTART_ENABLE;

        if (Svc_Key_Init(&keyCfg) != SVC_OK) {
            LOG_E("key init failed - the key component is skipped\r\n");
        } else if (Inter_KeyAttachReport() != SVC_OK) {
            LOG_W("key: could not attach the network report\r\n");
        }
    }
#endif

    /* ---- 5. 监视组件的参数(任务在 Monitor_App_Start 里建) ---------------- */
    {
        Svc_Mon_ConfigTypeDef monCfg;

        monCfg.PeriodMs     = APP_MON_PERIOD_MS;
        monCfg.FullDiv      = APP_MON_FULL_DIV;
        monCfg.PwmOcAutoAck = (int)APP_PWM_OC_AUTO_ACK;

        (void)Svc_Mon_Init(&monCfg);
    }

    /* ---- 6. 注册命令地图(必须在建 net 任务之前) -------------------------- */
    if (Svc_Net_RegisterCommands(Inter_App_Commands, Inter_App_CommandCount) != SVC_OK) {
        LOG_E("registering the command map failed\r\n");
        return APP_ERR_HW;
    }

    {
        Svc_Led_SnapshotTypeDef led;
        Svc_Pwm_SnapshotTypeDef pwm;
        uint16_t                cmdCount;

        (void)Svc_Led_GetSnapshot(&led);
        (void)Svc_Pwm_GetSnapshot(&pwm);

        cmdCount = Inter_App_CommandCount;

        LOG_I("ready: tcp %u, led 0x%02X/%s/%u Hz, pwm %u Hz en 0x%08X, "
              "%u commands (plugins: ls %u rs485 %u ads %u emmc %u)\r\n",
              (unsigned)APP_NET_TCP_PORT,
              (unsigned)led.Mask,
              Svc_Led_ModeName(led.Mode), (unsigned)led.Freq,
              (unsigned)pwm.FrequencyHz, (unsigned)pwm.EnableMask,
              (unsigned)cmdCount,
              (unsigned)APP_ENABLE_LS, (unsigned)APP_ENABLE_RS485,
              (unsigned)APP_ENABLE_ADS, (unsigned)APP_ENABLE_EMMC);
    }

    return APP_OK;
}

/**
 * @brief  建全部任务: 依次调各模块的 Xxx_App_Start()。
 *
 * 顺序: Inter(net+key) -> Monitor(led+mon) -> BrakeCtl(ls+brkctl) ->
 *       Mover(rs485+mover) -> Samp(ads+emmc)。net 必须最先起(别的组件靠它发遥测)。
 *
 * 一个模块失败不允许把后面的模块带走: 五个都叫一遍, 各自记一笔, 最后打一行汇总。
 * 只有 net 失败会返回错误(没有网络这块板子就没有可用的东西)。
 *
 * ⚠ 旧的光幕时序状态机(app/brake_app.c, Brake_App_Start)不再调用 —— 新方案由
 *   BrakeCtl_App_Start 建"ls 采样 + 制动控制"两个任务, 光幕只上报不驱动制动。
 */
App_StatusTypeDef App_StartTasks(void)
{
    const Svc_StatusTypeDef rNet   = Inter_App_Start();       /* net + key    */
    const Svc_StatusTypeDef rMon   = Monitor_App_Start();     /* led + mon    */
    const Svc_StatusTypeDef rBrake = BrakeCtl_App_Start();    /* ls  + brkctl */

#if (APP_ENABLE_MOTOR != 0U)
    const Svc_StatusTypeDef rMover = Mover_App_Start();       /* rs485 + mover */
#else
    const Svc_StatusTypeDef rMover = SVC_OK;                  /* 没装: 当成功  */
#endif

    const Svc_StatusTypeDef rSamp  = Samp_App_Start();        /* ads + emmc   */
    const Svc_StatusTypeDef rCur   = Current_App_Start();     /* curloop      */

    if (rNet != SVC_OK) {
        LOG_E("net/key tasks FAILED - the board has no network\r\n");
    }
    if (rMon != SVC_OK) {
        LOG_E("led/monitor tasks FAILED (see the message above)\r\n");
    }
    if (rBrake != SVC_OK) {
        LOG_W("ls/brkctl tasks FAILED (brake control unavailable)\r\n");
    }
    if (rMover != SVC_OK) {
        LOG_W("rs485/mover tasks FAILED (launch will report BUSY)\r\n");
    }
    if (rSamp != SVC_OK) {
        LOG_W("ads/emmc tasks FAILED (sampling unavailable)\r\n");
    }
    if (rCur != SVC_OK) {
        LOG_W("curloop task FAILED (current closed loop unavailable)\r\n");
    }

    LOG_I("tasks: net/key %s | led/mon %s | ls/brkctl %s | rs485/mover %s | ads/emmc %s | curloop %s\r\n",
          App_StartOk(rNet), App_StartOk(rMon), App_StartOk(rBrake),
          App_StartOk(rMover), App_StartOk(rSamp), App_StartOk(rCur));

    return (rNet == SVC_OK) ? APP_OK : APP_ERR_HW;
}
