/**
 *******************************************************************************
 * @file    log.h
 * @brief   统一格式的串口日志宏(xil_printf + 整行加锁)
 *
 * 定位: 跨层公共设施, 允许被所有层使用(否则 BSP 打日志就会反向依赖 Service),
 *       自己只依赖 osal/。
 *
 *   1. 只用 xil_printf, 不引入 printf / va_list: 标签和格式串在编译期拼成常量。
 *   2. 换标签: 在 include 之前写一行 #define LOG_TAG "net"。
 *   3. 每个宏打印一整行并整行加锁, 多任务同时打印不会出现半行交错。
 *
 * 用法: LOG_I("client %u connected\r\n", (unsigned)n);
 *
 * @note 锁是递归的(Log_Lock(); Enet_PrintStatus(&h); Log_Unlock(); 是安全的),
 *       但别长时间持有: 一行 100 字符在 115200 下要 8.7 ms。
 * @note 不要在中断里用; 调度器没起来时取锁退化成空操作。
 *******************************************************************************
 */

#ifndef LOG_H
#define LOG_H

#include <stdint.h>

#include "xil_printf.h"

/* -------------------------------------------------------------------------- */
/* 标签                                                                        */
/* -------------------------------------------------------------------------- */

/** 默认标签, 可以被各文件覆盖。 */
#ifndef LOG_TAG
#define LOG_TAG "app"
#endif

/* -------------------------------------------------------------------------- */
/* 宏                                                                          */
/* -------------------------------------------------------------------------- */

/** 普通信息。 */
#define LOG_I(...)                                                       \
    do {                                                                 \
        Log_Lock();                                                      \
        xil_printf("[" LOG_TAG "] " __VA_ARGS__);                        \
        Log_Unlock();                                                    \
    } while (0)

/** 警告: 能继续运行, 但需要知道。 */
#define LOG_W(...)                                                       \
    do {                                                                 \
        Log_Lock();                                                      \
        xil_printf("[" LOG_TAG "][W] " __VA_ARGS__);                     \
        Log_Unlock();                                                    \
    } while (0)

/** 错误: 该功能不可用。 */
#define LOG_E(...)                                                       \
    do {                                                                 \
        Log_Lock();                                                      \
        xil_printf("[" LOG_TAG "][E] " __VA_ARGS__);                     \
        Log_Unlock();                                                    \
    } while (0)

/* -------------------------------------------------------------------------- */
/* 函数                                                                        */
/* -------------------------------------------------------------------------- */

/** @brief  建立日志互斥量, 必须在 main() 最前面调用一次(创建任务之前)。 */
void Log_Init(void);

/** @brief  上电到现在的毫秒数(调度器启动前恒为 0)。 */
uint32_t Log_UptimeMs(void);

/** @brief  取得 / 释放日志锁; 直接用 xil_printf 打一行的代码成对调用它们就不会交错。 */
void Log_Lock(void);
void Log_Unlock(void);

/** @brief  打印一个醒目的分节标题, 用于上电信息。 */
void Log_Banner(const char *title);

/* -------------------------------------------------------------------------- */
/*  Per-module switch                                                          */
/* -------------------------------------------------------------------------- */

/*
 * 某个模块想关掉自己的日志, 在 include 本头文件之前(紧挨 LOG_TAG)写:
 *
 *     #define LOG_TAG     "enet"
 *     #define LOG_ENABLE  0
 *     #include "service/monitor_module/log/log.h"
 *
 * 默认 1(开)。LOG_RAW(...) 是可开关的 xil_printf, 给"一行由几次调用拼出来"的地方用。
 */
#ifndef LOG_ENABLE
#define LOG_ENABLE 1
#endif

#if (LOG_ENABLE != 0)
#define LOG_RAW(...)   xil_printf(__VA_ARGS__)
#else
#undef  LOG_I
#undef  LOG_W
#undef  LOG_E
#define LOG_I(...)     ((void)0)
#define LOG_W(...)     ((void)0)
#define LOG_E(...)     ((void)0)
#define LOG_RAW(...)   ((void)0)
#endif

#endif /* LOG_H */
