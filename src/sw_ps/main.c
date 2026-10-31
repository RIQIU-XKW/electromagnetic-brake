/**
 *******************************************************************************
 * @file    main.c
 * @brief   app923 入口 —— 只做四件事, 其它都在 app/ service/ bsp/ osal/
 *
 *   1. Log_Init()           建立日志互斥量(必须最先);
 *   2. App_Init()           装配各业务组件 + 注册命令表;
 *   3. App_StartTasks()     建起全部 10 个任务;
 *   4. Osal_StartScheduler() 启动调度器(正常情况下永远不返回)。
 *
 * 分层: bsp -> service -> app, log 是跨层公共设施; 模块划分见 src/README.txt
 *******************************************************************************
 */

#define LOG_ENABLE 0   /* 本模块日志已关闭; 需要时改成 1 */
#define LOG_TAG "main"
#include "service/monitor_module/log/log.h"

#include "app/app.h"

#include "osal/osal.h"

int main(void)
{
    Log_Init();

    /* 开机横幅总是打(带编译时间, 用来确认板上跑的是哪一版) */
    Log_Banner("app923 : FreeRTOS tasks + Ethernet + PWM power stage   [built "
               __DATE__ " " __TIME__ "]");

    /* 装配各组件, 必须在调度器启动之前全部完成 */
    if (App_Init() != APP_OK) {
        LOG_E("App_Init() failed, halted\r\n");
        for (;;) {
        }
    }

    if (App_StartTasks() != APP_OK) {
        LOG_E("task creation failed: OSAL heap too small\r\n");
        for (;;) {
        }
    }

    LOG_I("starting the scheduler\r\n");
    Osal_StartScheduler();

    LOG_E("scheduler returned: not enough heap for the idle task\r\n");
    for (;;) {
    }
}
