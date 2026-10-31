/**
 *******************************************************************************
 * @file    service_monitor.c
 * @brief   monitor 业务组件 —— 每秒一行状态, 每 N 行一次详细状态
 *
 * 打印出来的一行长这样(和重构前一致):
 *
 *   [mon] up 12s | ip 192.168.1.10 (static) up 100M cli 1 | pwm oc 0/0 | cmds 3 | drop 0
 *
 * 每 FullDiv 行额外打印一组详细状态(网口的), 用 Log_Lock() 把它整段包起来, 免得
 * 和别的任务的日志搅在一起。日志锁是递归互斥量, 所以被包住的函数内部继续用
 * LOG_* 也不会死锁(见 log/log.c)。
 *******************************************************************************
 */

#define LOG_ENABLE 0   /* 本模块日志默认关闭; 需要时改成 1 */
#define LOG_TAG "mon"
#include "service/monitor_module/log/log.h"

#include "service/monitor_module/service_monitor.h"
#include "service/inter_module/service_net.h"
#include "service/brake_module/service_pwm.h"

#include "osal/osal.h"

#include <string.h>             /* memset */

/* ========================================================================== */
/*  内部变量                                                                   */
/* ========================================================================== */

static Svc_Mon_ConfigTypeDef gCfg;

/* ========================================================================== */
/*  内部函数(static)                                                           */
/* ========================================================================== */

/** @brief  monitor 任务(创建由应用层负责, 见 service_monitor.h). */
void Svc_Mon_Task(void *Arg)
{
    uint32_t lines = 0U;

    (void)Arg;

    LOG_I("task started (period %u ms, %u Hz tick)\r\n",
          (unsigned)gCfg.PeriodMs, (unsigned)Osal_TickRateHz());

    for (;;) {
        Svc_Net_SnapshotTypeDef net;
        Svc_Pwm_SnapshotTypeDef pwm;
        const char             *source;

        if (Svc_Net_GetSnapshot(&net) != SVC_OK) {
            (void)memset(&net, 0, sizeof(net));
        }
        if (Svc_Pwm_GetSnapshot(&pwm) != SVC_OK) {
            (void)memset(&pwm, 0, sizeof(pwm));
        }

        switch (net.AddrSource) {
            case 1U:  source = "dhcp";    break;
            case 2U:  source = "static";  break;
            default:  source = "waiting"; break;
        }
        (void)source;           /* 只有下面的 LOG_I 用得到: 关掉日志时避免未使用告警 */

        LOG_I("up %us | ip %s (%s) %s %uM cli %u | pwm oc %u/%u | cmds %u | drop %u\r\n",
              (unsigned)(Log_UptimeMs() / 1000U),
              net.Ip,
              source,
              (net.LinkUp != 0U) ? "up" : "down",
              (unsigned)net.SpeedMbps,
              (unsigned)net.Clients,
              (unsigned)pwm.OcNow,
              (unsigned)pwm.OcLatched,
              (unsigned)net.TcpCmds,
              (unsigned)net.Drops);

        lines++;

        /*
         * 过流告警: 逻辑在 bsp/pwm 的 Pwm_Alarm_Poll() 里, 用 [pwm] 标签打印 ——
         * 这样把监视任务自己的日志关掉, 也不会把过流告警一起关掉。
         * 每次循环(默认 1 s)查一次; 硬件已经在 ~2 ns 内把故障桥关掉了, 这里只是报告。
         */
        Svc_Pwm_AlarmPoll(gCfg.PwmOcAutoAck);

        if ((gCfg.FullDiv != 0U) && ((lines % gCfg.FullDiv) == 0U)) {
            /*
             * 整段独占, 整行不会被别的任务切开.  Svc_Net_PrintStatus() 内部也会调
             * LOG_I, 也就是同一个任务再取一次日志锁 —— 所以日志锁必须是递归
             * 互斥量(见 log/log.c 的说明: 用普通互斥量在这里会死锁).
             */
            Log_Lock();
            Svc_Net_PrintStatus();
            /* Pwm_PrintStatus 走 PWMSTAT 命令看就行, 周期行默认不打印 ——
             * 只保留过流告警(见 bsp/pwm/pwm.c 的 Pwm_Alarm_Poll) */
            Log_Unlock();
        }

        Osal_DelayMs(gCfg.PeriodMs);
    }
}

/* ========================================================================== */
/*  对外接口                                                                     */
/* ========================================================================== */

Svc_StatusTypeDef Svc_Mon_Init(const Svc_Mon_ConfigTypeDef *Cfg)
{
    gCfg.PeriodMs      = (Cfg != NULL && Cfg->PeriodMs != 0U) ? Cfg->PeriodMs
                                                             : SVC_MON_PERIOD_MS;
    gCfg.FullDiv       = (Cfg != NULL) ? Cfg->FullDiv : SVC_MON_FULL_DIV;
    gCfg.PwmOcAutoAck  = (Cfg != NULL) ? Cfg->PwmOcAutoAck : 1;

    LOG_I("configured: period %u ms, full state every %u, oc auto-ack %s\r\n",
          (unsigned)gCfg.PeriodMs, (unsigned)gCfg.FullDiv,
          (gCfg.PwmOcAutoAck != 0) ? "on" : "off");

    return SVC_OK;
}
