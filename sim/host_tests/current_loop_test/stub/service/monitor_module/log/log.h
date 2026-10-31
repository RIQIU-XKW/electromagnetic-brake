/**
 *******************************************************************************
 * @file    log.h  (HOST STUB)
 * @brief   主机测试用的日志桩: LOG_* 全部转到 Host_Log(), 由测试决定打不打
 *
 * 真的 log.h 会去抢 osal 的递归锁再走 UART; 主机上没有 UART, 也不需要锁,
 * 只要保证被测代码里的 LOG_I/LOG_E 能编过、想看的时候能看到。
 *******************************************************************************
 */

#ifndef STUB_LOG_H
#define STUB_LOG_H

#include <stdint.h>

/** 桩日志输出(带 "[curloop] " 前缀; 由 Host_SetLogVerbose() 控制开关)。 */
void Host_Log(const char *Fmt, ...);

#define LOG_I(...)   Host_Log(__VA_ARGS__)
#define LOG_W(...)   Host_Log(__VA_ARGS__)
#define LOG_E(...)   Host_Log(__VA_ARGS__)
#define LOG_D(...)   ((void)0)

#endif /* STUB_LOG_H */
