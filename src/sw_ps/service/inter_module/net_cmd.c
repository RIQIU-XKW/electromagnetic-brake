/**
 *******************************************************************************
 * @file    net_cmd.c
 * @brief   net 组件的命令注册表 + 文本解析工具(不碰 lwIP, 不建任务)
 *
 * 注册表用固定容量数组, 不做堆分配: 命令表是编译期常量, 表项数量本来就不多
 * (精确匹配 16 条 + 兜底 2 条, 容量见 service_net.h 的 SVC_NET_MAX_COMMANDS /
 * SVC_NET_MAX_FALLBACK)。加组件时如果注册返回 SVC_ERR_FULL, 就把容量调大。
 *******************************************************************************
 */

#define LOG_ENABLE 0   /* 本模块日志默认关闭; 需要时改成 1 */
#define LOG_TAG "net"
#include "service/monitor_module/log/log.h"

#include "service/inter_module/net_cmd.h"

#include <string.h>

/* ========================================================================== */
/*  注册表                                                                     */
/* ========================================================================== */

/** 精确匹配的命令(Name != NULL), 按注册顺序排列. */
static Svc_Net_CmdTypeDef gCmds[SVC_NET_MAX_COMMANDS];
static uint16_t           gCmdCount;

/** 兜底命令(Name == NULL), 按注册顺序排列. */
static Svc_Net_CmdTypeDef gFallback[SVC_NET_MAX_FALLBACK];
static uint16_t           gFallbackCount;

void Svc_Net_Cmd_Reset(void)
{
    (void)memset(gCmds, 0, sizeof(gCmds));
    (void)memset(gFallback, 0, sizeof(gFallback));
    gCmdCount      = 0U;
    gFallbackCount = 0U;
}

Svc_StatusTypeDef Svc_Net_RegisterCommands(const Svc_Net_CmdTypeDef *Table, uint16_t Count)
{
    uint16_t i;

    if ((Table == NULL) || (Count == 0U)) {
        return SVC_ERR_PARAM;
    }

    for (i = 0U; i < Count; i++) {
        if (Table[i].Handler == NULL) {
            return SVC_ERR_PARAM;
        }

        if (Table[i].Name == NULL) {
            if (gFallbackCount >= SVC_NET_MAX_FALLBACK) {
                return SVC_ERR_FULL;
            }
            gFallback[gFallbackCount++] = Table[i];
        } else {
            if (gCmdCount >= SVC_NET_MAX_COMMANDS) {
                return SVC_ERR_FULL;
            }
            gCmds[gCmdCount++] = Table[i];
        }
    }

    return SVC_OK;
}

/* ========================================================================== */
/*  解析工具                                                                   */
/* ========================================================================== */

/** @brief  整条报文是否正好等于 Cmd(文本命令的整行比较)。 */
int Svc_Net_IsCommand(const uint8_t *Data, uint16_t Len, const char *Cmd)
{
    size_t cmdLen;

    if ((Data == NULL) || (Cmd == NULL)) {
        return 0;
    }

    cmdLen = strlen(Cmd);

    return ((Len == (uint16_t)cmdLen) && (memcmp(Data, Cmd, cmdLen) == 0)) ? 1 : 0;
}

/* ========================================================================== */
/*  分发                                                                       */
/* ========================================================================== */

int Svc_Net_Cmd_Dispatch(const uint8_t *Data, uint16_t Len, Svc_Net_ReplyTypeDef *R)
{
    uint16_t i;

    if ((Data == NULL) || (R == NULL)) {
        return 0;
    }

    /* ---- 1. 精确匹配(按注册顺序, 第一个命中的说话) ---------------------- */
    for (i = 0U; i < gCmdCount; i++) {
        if (Svc_Net_IsCommand(Data, Len, gCmds[i].Name) != 0) {
            if (gCmds[i].Handler(gCmds[i].Arg, Data, Len, R) != 0) {
                return 1;
            }
        }
    }

    /* ---- 2. 兜底: 精确匹配都不认, 按注册顺序依次问 ---------------------- */
    for (i = 0U; i < gFallbackCount; i++) {
        if (gFallback[i].Handler(gFallback[i].Arg, Data, Len, R) != 0) {
            return 1;
        }
    }

    return 0;
}
