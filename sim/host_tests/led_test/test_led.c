/**
 * 4 路呼吸灯(breath_led IP) —— 宿主机测试
 *
 * 这个测试编的是**真实的生产代码**, 一条链路到底:
 *
 *     Svc_Net_Cmd_Dispatch()      service/inter_module/net_cmd.c (真实的命令注册表 + 分发)
 *        -> Monitor_App_StateCommand / Monitor_App_Command  app/monitor_app.c
 *           -> Svc_Led_Request*  -> 队列 -> led 任务        service/monitor_module/led/service_led.c
 *              -> Led_SetEnable/SetMask/SetFreq/SetMode    bsp/led/led.c
 *                 -> Xil_Out32                            host_regfile.c(寄存器文件替身)
 *
 * 命令表由**测试自己**拼一份(见 test_b_setup 里的 LedTest_Commands): 板子上那张全板
 * 统一的命令地图在 app/inter_app.c, 而它把制动/发射/采样/按键几个模块都拉进来了 ——
 * 为了测灯去编那 4 个模块 + 它们的 service/bsp 不值得。这里只搬 LED 那 5 行, 处理
 * 函数用的还是生产代码里那两个。
 *
 * 只有三处是替身: AXI 寄存器文件、OSAL(用 pthread 实现真队列 + 真任务)、
 * Svc_Net_Reply(抓回复文本)。别的都是板子上跑的那份源码。
 *
 * 覆盖:
 *   [A] 驱动寄存器级 —— 4 路掩码、共享频率、夹取、老坑(只写 PERIOD 不亮)
 *   [B] 命令级      —— LEDMASK/LEDON/LEDOFF/LEDFREQ/LEDSTAT/LED_STA_0..3 的
 *                      参数校验、回复文本、寄存器结果、以及"不抢别的命令"。
 *
 * 时间同步不用 sleep 猜: 命令是异步执行的, 测试等 Svc_Led_GetSnapshot().Commands
 * 涨到期望值 —— 那个计数在寄存器写完之后才加, 看到它就是写完了。
 */

#include <stdio.h>
#include <string.h>

/*
 * 用 windows.h 的 Sleep / GetTickCount。
 *
 * 特意**不用 GetTickCount64**: 它要求 _WIN32_WINNT >= 0x0600, 而这个宏必须在
 * **任何**头文件之前定义才有效 —— MinGW 的 <stdio.h> 会先经 _mingw.h 把 WINVER /
 * _WIN32_WINNT 定成默认值, 之后在文件里再 define 已经没用了, 结果就是
 * "implicit declaration of GetTickCount64" 加一条符号比较警告。
 * GetTickCount 没有版本门槛, 而它 49 天回绕的问题在测试里用无符号差分天然免疫。
 */
#include <windows.h>

#include "bsp/led/led.h"
#include "service/monitor_module/led/service_led.h"
#include "service/inter_module/net_cmd.h"
#include "app/monitor_app.h"        /* LED 命令处理(生产代码, 在应用层) */
#include "osal/osal.h"              /* Osal_TaskCreate: 测试扮演应用层, 自己建任务 */

/* host_regfile.c */
void     HostReg_Reset(void);
uint32_t HostReg_Read(uint32_t offset);
void     HostReg_Write(uint32_t offset, uint32_t value);
uint32_t HostReg_Writes(void);

/* host_osal.c */
const char *HostNet_ReplyText(void);
uint16_t    HostNet_ReplyLen(void);
uint32_t    HostNet_ReplyCount(void);
void        HostNet_ReplyReset(void);

static int g_pass;
static int g_fail;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            g_pass++;                                                        \
            printf("  PASS  ");                                              \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  FAIL  ");                                              \
        }                                                                    \
        printf(__VA_ARGS__);                                                 \
        printf("\n");                                                        \
    } while (0)

/* ========================================================================== */
/*  小工具                                                                     */
/* ========================================================================== */

/** CTRL 寄存器的当前值(四路使能掩码)。 */
static uint32_t ctrl(void) { return HostReg_Read(LED_REG_CTRL); }

/** PERIOD 寄存器的当前值(共享呼吸频率)。 */
static uint32_t period(void) { return HostReg_Read(LED_REG_PERIOD); }

/**
 * 等 led 任务把队列里的命令全部执行完(最多 2 s)。
 *
 * 用 GetTickCount 判超时而不是数 Sleep(1) 的次数: Windows 上 Sleep(1) 的实际
 * 粒度是计时器 tick(常见 15.6 ms), 数 2000 次会变成等 31 秒 —— 命令一旦没被执行,
 * 整个测试要跑十几分钟, 看起来就像卡死了(第一次调试这个测试时就是这么被坑的)。
 * 循环里用 Sleep(0) 让出 CPU, 不依赖计时器精度。
 *
 * 时间差用无符号减法算: GetTickCount 是 32 位的, 49 天回绕, 但 (now - start)
 * 在无符号语义下回绕时差值仍然正确。
 */
static int wait_commands(uint32_t want)
{
    DWORD start = GetTickCount();

    for (;;) {
        Svc_Led_SnapshotTypeDef snap;

        (void)Svc_Led_GetSnapshot(&snap);
        if (snap.Commands >= want) {
            return 1;
        }
        if ((DWORD)(GetTickCount() - start) >= 2000U) {
            printf("        (等命令计数到 %u 超时, 当前 %u)\n",
                   (unsigned)want, (unsigned)snap.Commands);
            return 0;
        }
        Sleep(0);
    }
}

/** 把一串命令文本丢给真实的命令分发器, 返回 1 = 有人认领了(用于"不该被认领"的检查)。 */
static int dispatch(const char *cmd)
{
    Svc_Net_ReplyTypeDef reply = { NULL };

    return Svc_Net_Cmd_Dispatch((const uint8_t *)cmd, (uint16_t)strlen(cmd), &reply);
}

/**
 * 把一串**字节**丢给真实的分发器 —— 呼吸灯命令 2026-10-16 起就是单字节协议了,
 * 这里和板子上走的是同一条路(Svc_Net_Cmd_Dispatch -> Monitor_App_Command)。
 */
static int dispatch_bytes(const uint8_t *bytes, uint16_t len)
{
    Svc_Net_ReplyTypeDef reply = { NULL };

    return Svc_Net_Cmd_Dispatch(bytes, len, &reply);
}

/** 发一串字节并**等它执行完**的封装 —— 只用于摆前置状态, 不校验结果。 */
static void fire_b(const uint8_t *bytes, uint16_t len, uint8_t cmd)
{
    Svc_Led_SnapshotTypeDef snap;
    char                    label[24];

    (void)snprintf(label, sizeof(label), "0x%02X", (unsigned)cmd);

    (void)Svc_Led_GetSnapshot(&snap);

    CHECK(dispatch_bytes(bytes, len) == 1, "%s 被认领", label);
    CHECK(wait_commands(snap.Commands + 1U), "%s led 任务执行完成", label);
}

static void fire_b1(uint8_t cmd)
{
    fire_b(&cmd, 1U, cmd);
}

static void fire_b2(uint8_t cmd, uint8_t arg)
{
    const uint8_t bytes[2] = { cmd, arg };

    fire_b(bytes, 2U, cmd);
}

/** 回复文本必须和上报长度一致, 而且以 \r\n 结尾。 */
static void check_reply_sane(const char *what)
{
    size_t real = strlen(HostNet_ReplyText());

    CHECK(real == (size_t)HostNet_ReplyLen(),
          "%s: 回复长度和文本一致 (Len=%u, strlen=%u)", what,
          (unsigned)HostNet_ReplyLen(), (unsigned)real);
    CHECK(real >= 2U && HostNet_ReplyText()[real - 2U] == '\r' &&
          HostNet_ReplyText()[real - 1U] == '\n',
          "%s: 回复以 \\r\\n 结尾", what);
}

/**
 * 发一串字节并校验(老的 expect_full 的字节版)。
 * @param applied : 1 = 命令会进队列(等它执行完); 0 = 不产生队列元素。
 */
static void expect_full_b(const uint8_t *bytes, uint16_t len, const char *reply_substr,
                          int applied, int want_ctrl, uint32_t want_period, const char *what)
{
    Svc_Led_SnapshotTypeDef snap;

    /* 计数要在发命令**之前**抓: 命令是异步执行的, 发完再抓有可能已经把这一次的
     * 增量算进去了, 那样 wait 就会白等 2 秒然后失败。 */
    (void)Svc_Led_GetSnapshot(&snap);
    HostNet_ReplyReset();

    CHECK(dispatch_bytes(bytes, len) == 1, "%s: 被认领", what);

    if (applied != 0) {
        CHECK(wait_commands(snap.Commands + 1U), "%s: led 任务执行完成", what);
        CHECK(HostNet_ReplyCount() == 1U, "%s: 只回了一句话", what);
    } else {
        /* 不产生队列元素: 给 led 任务留点时间, 命令计数必须没涨 */
        Svc_Led_SnapshotTypeDef now;

        Sleep(30);
        (void)Svc_Led_GetSnapshot(&now);
        CHECK(now.Commands == snap.Commands,
              "%s: 没往队列里塞东西(计数仍是 %u)", what, (unsigned)now.Commands);
        CHECK(HostNet_ReplyCount() == 1U, "%s: 只回了一句话", what);
    }

    if (reply_substr != NULL) {
        CHECK(strstr(HostNet_ReplyText(), reply_substr) != NULL,
              "%s: 回复含 \"%s\"  (实际: %s)", what, reply_substr,
              HostNet_ReplyText());
    }
    check_reply_sane(what);

    if (want_ctrl >= 0) {
        CHECK((int)ctrl() == want_ctrl, "%s: CTRL = 0x%02X (期望 0x%02X)", what,
              (unsigned)ctrl(), (unsigned)want_ctrl);
    }
    if (want_period != 0U) {
        CHECK(period() == want_period, "%s: PERIOD = %u (期望 %u)", what,
              (unsigned)period(), (unsigned)want_period);
    }
}

/** 一条单字节命令(0xF0..0xFD): 会被执行。 */
static void expect_b1(uint8_t cmd, const char *reply_substr,
                      int want_ctrl, uint32_t want_period, const char *what)
{
    expect_full_b(&cmd, 1U, reply_substr, 1, want_ctrl, want_period, what);
}

/** 一条"命令 + 参数"的两字节命令(0xFE / 0xFF): 会被执行。 */
static void expect_b2(uint8_t cmd, uint8_t arg, const char *reply_substr,
                      int want_ctrl, uint32_t want_period, const char *what)
{
    const uint8_t bytes[2] = { cmd, arg };

    expect_full_b(bytes, 2U, reply_substr, 1, want_ctrl, want_period, what);
}

/**
 * 不往队列里塞东西的命令 —— 两类:
 *   (a) 参数越界的(只回一句用法提示);
 *   (b) 纯查询命令(0xF4: 只读状态回话)。
 * 两类都不该改动硬件, 也不该让 led 任务执行任何东西。
 */
static void expect_b1_noqueue(uint8_t cmd, const char *reply_substr,
                              int want_ctrl, uint32_t want_period, const char *what)
{
    expect_full_b(&cmd, 1U, reply_substr, 0, want_ctrl, want_period, what);
}

static void expect_b2_noqueue(uint8_t cmd, uint8_t arg, const char *reply_substr,
                              int want_ctrl, uint32_t want_period, const char *what)
{
    const uint8_t bytes[2] = { cmd, arg };

    expect_full_b(bytes, 2U, reply_substr, 0, want_ctrl, want_period, what);
}

/* ========================================================================== */
/*  [A] 驱动寄存器级                                                            */
/* ========================================================================== */

static Led_HandleTypeDef gLed;

static void test_a_constants(void)
{
    printf("[A0] 常量与位宽\n");

    CHECK(LED_COUNT == 4U, "LED_COUNT = %u (期望 4)", (unsigned)LED_COUNT);
    CHECK(LED_MASK_ALL == 0x0FU, "LED_MASK_ALL = 0x%02X (期望 0x0F)", (unsigned)LED_MASK_ALL);
    CHECK(LED_MASK_INDICATOR == 0x01U, "LED_MASK_INDICATOR = 0x%02X (期望 0x01: LED0/K16)",
          (unsigned)LED_MASK_INDICATOR);
    CHECK(LED_MASK_OF(0) == 0x01U && LED_MASK_OF(1) == 0x02U &&
          LED_MASK_OF(2) == 0x04U && LED_MASK_OF(3) == 0x08U,
          "LED_MASK_OF(0..3) = 0x01/0x02/0x04/0x08");
    CHECK(LED_BASEADDR == 0x43C00000U, "LED_BASEADDR = 0x%08X (pl.dtsi 里是 0x43c00000)",
          (unsigned)LED_BASEADDR);
}

static void test_a_init(void)
{
    printf("[A1] 初始化\n");

    HostReg_Reset();

    CHECK(Led_Init(&gLed, 0U) == LED_OK, "Led_Init 返回 LED_OK");
    CHECK(gLed.BaseAddr == LED_BASEADDR, "BaseAddr = 0x%08X", (unsigned)gLed.BaseAddr);
    CHECK(ctrl() == 0U, "初始化后 CTRL = 0x%02X (四路全灭)", (unsigned)ctrl());
    CHECK(Led_GetMask(&gLed) == 0U, "初始化后掩码 = 0x%02X", (unsigned)Led_GetMask(&gLed));
    CHECK(gLed.Enabled == 0U, "初始化后 Enabled = 0");
    CHECK(period() == 100U, "初始化后 PERIOD = %u (标准呼吸 100)", (unsigned)period());
}

static void test_a_enable(void)
{
    uint32_t before;

    printf("[A2] 单路开关(Led_SetEnable)—— 只动自己那一位\n");

    (void)Led_SetMask(&gLed, 0U);

    /* 位 0 = LED0 = K16 */
    CHECK(Led_SetEnable(&gLed, 0U, 1U) == LED_OK, "点亮 LED0 返回 LED_OK");
    CHECK(ctrl() == 0x01U, "CTRL = 0x%02X (期望 0x01: 只有 LED0)", (unsigned)ctrl());

    /* 位 2 = LED2 = K14; LED0 必须还亮着 */
    CHECK(Led_SetEnable(&gLed, 2U, 1U) == LED_OK, "点亮 LED2 返回 LED_OK");
    CHECK(ctrl() == 0x05U, "CTRL = 0x%02X (期望 0x05: LED0 + LED2 同时亮)", (unsigned)ctrl());

    /* 位 3 = LED3 = J14 */
    CHECK(Led_SetEnable(&gLed, 3U, 1U) == LED_OK, "点亮 LED3 返回 LED_OK");
    CHECK(ctrl() == 0x0DU, "CTRL = 0x%02X (期望 0x0D: LED0/LED2/LED3)", (unsigned)ctrl());

    /* 位 1 = LED1 = J16 */
    CHECK(Led_SetEnable(&gLed, 1U, 1U) == LED_OK, "点亮 LED1 返回 LED_OK");
    CHECK(ctrl() == 0x0FU, "CTRL = 0x%02X (期望 0x0F: 四路全亮)", (unsigned)ctrl());
    CHECK(Led_GetMask(&gLed) == 0x0FU, "掩码影子 = 0x%02X", (unsigned)Led_GetMask(&gLed));
    CHECK(gLed.Enabled == 1U, "至少一路亮着 -> Enabled = 1");

    /* 只关 LED1, 其它三路不受影响 */
    CHECK(Led_SetEnable(&gLed, 1U, 0U) == LED_OK, "熄灭 LED1 返回 LED_OK");
    CHECK(ctrl() == 0x0DU, "CTRL = 0x%02X (期望 0x0D: LED1 灭, 其它不变)", (unsigned)ctrl());

    printf("[A3] 单路开关 —— 越界参数必须被拒且不动硬件\n");

    before = ctrl();
    CHECK(Led_SetEnable(&gLed, 4U, 1U) == LED_ERR_PARAM, "Index = 4 (越界) 返回 LED_ERR_PARAM");
    CHECK(Led_SetEnable(&gLed, 255U, 1U) == LED_ERR_PARAM, "Index = 255 返回 LED_ERR_PARAM");
    CHECK(Led_SetEnable(&gLed, 0U, 2U) == LED_ERR_PARAM, "On = 2 (非 0/1) 返回 LED_ERR_PARAM");
    CHECK(ctrl() == before, "三次非法调用后 CTRL 仍是 0x%02X (硬件没被碰)",
          (unsigned)ctrl());
}

static void test_a_mask(void)
{
    printf("[A4] 整体掩码(Led_SetMask)\n");

    CHECK(Led_SetMask(&gLed, 0x0FU) == LED_OK, "SetMask(0x0F) 返回 LED_OK");
    CHECK(ctrl() == 0x0FU, "SetMask(0x0F) 之后 CTRL = 0x%02X (四路全亮)", (unsigned)ctrl());

    /* 高位必须被吃掉: IP 只有 CTRL.bit[3:0] */
    CHECK(Led_SetMask(&gLed, 0xFFU) == LED_OK, "SetMask(0xFF) 返回 LED_OK");
    CHECK(ctrl() == 0x0FU, "SetMask(0xFF) 之后 CTRL = 0x%02X (高位被丢掉)", (unsigned)ctrl());
    CHECK(Led_GetMask(&gLed) == 0x0FU, "掩码影子也只剩低 4 位 (0x%02X)",
          (unsigned)Led_GetMask(&gLed));

    CHECK(Led_SetMask(&gLed, 0x00U) == LED_OK, "SetMask(0x00) 返回 LED_OK");
    CHECK(ctrl() == 0x00U, "SetMask(0x00) 之后 CTRL = 0x%02X (四路全灭)", (unsigned)ctrl());
    CHECK(gLed.Enabled == 0U, "全灭后 Enabled = 0");
}

static void test_a_freq(void)
{
    Led_ModeTypeDef mode = LED_MODE_OFF;
    uint16_t        freq = 0U;
    uint8_t         en   = 0U;

    printf("[A5] 共享频率(Led_SetFreq)—— 含老坑回归\n");

    (void)Led_SetMask(&gLed, 0U);

    CHECK(Led_SetFreq(&gLed, 0U) == LED_ERR_PARAM, "频率 0 返回 LED_ERR_PARAM");

    /*
     * ★ 回归: 2026 年踩过的坑 —— 只写 PERIOD 不写 CTRL, 灯根本不亮。
     *   "网口在线"走的就是 "Freq != 0 -> Led_SetFreq()" 这条路。
     *   掩码为 0 时 SetFreq 必须把指示灯点亮(CTRL = 0x01)。
     */
    CHECK(Led_SetFreq(&gLed, 200U) == LED_OK, "SetFreq(200) 返回 LED_OK");
    CHECK(period() == 200U, "PERIOD = %u (期望 200)", (unsigned)period());
    CHECK(ctrl() == 0x01U,
          "★ 掩码为 0 时设频率 -> CTRL = 0x%02X (期望 0x01, 指示灯被点亮; 老 bug 就是这里为 0)",
          (unsigned)ctrl());

    /* 掩码不为 0 时不许乱动: 改频率不该点灯也不该关灯 */
    (void)Led_SetMask(&gLed, 0x0AU);
    CHECK(Led_SetFreq(&gLed, 500U) == LED_OK, "SetFreq(500) 返回 LED_OK");
    CHECK(period() == 500U, "PERIOD = %u (期望 500)", (unsigned)period());
    CHECK(ctrl() == 0x0AU, "掩码 0x0A 保持不动 -> CTRL = 0x%02X (期望 0x0A)",
          (unsigned)ctrl());

    printf("[A6] 频率夹取(IP 的限制: 10..1000, 且 PERIOD 只有 10 位)\n");

    CHECK(Led_SetFreq(&gLed, 5U) == LED_OK, "SetFreq(5) 返回 LED_OK");
    CHECK(period() == LED_FREQ_MIN, "PERIOD = %u (夹到下限 %u)", (unsigned)period(),
          (unsigned)LED_FREQ_MIN);

    CHECK(Led_SetFreq(&gLed, 5000U) == LED_OK, "SetFreq(5000) 返回 LED_OK");
    CHECK(period() == LED_FREQ_MAX, "PERIOD = %u (夹到上限 %u)", (unsigned)period(),
          (unsigned)LED_FREQ_MAX);
    CHECK(period() <= 1023U, "PERIOD = %u 不超出 10 位 (硬件只留 [9:0])", (unsigned)period());

    /*
     * 状态读回必须报**硬件真正在跑的值**。以前驱动把未夹取的原值存进影子, 于是
     * SetFreq(5000) 之后 Led_GetState() 报 5000, 而板子按 1000 跑 —— STATUS /
     * LEDSTAT 就会印出一个不存在的频率。这条断言就是钉住那个行为。
     */
    CHECK(Led_GetState(&gLed, &mode, &freq, &en) == LED_OK, "Led_GetState 返回 LED_OK");
    CHECK(freq == LED_FREQ_MAX, "★ 读回频率 = %u (期望夹取后的 %u, 不是 5000)",
          (unsigned)freq, (unsigned)LED_FREQ_MAX);
    CHECK(mode == LED_MODE_BREATH_FAST, "1000 -> 模式 fast (实际 %s)", Led_ModeName(mode));
    CHECK(en == 1U, "读回 Enabled = 1");
}

static void test_a_mode(void)
{
    printf("[A7] 模式(Led_SetMode)—— 关灯是四路全灭, 别的模式只保证至少一颗亮\n");

    (void)Led_SetMask(&gLed, 0x0FU);
    CHECK(Led_SetMode(&gLed, LED_MODE_OFF) == LED_OK, "SetMode(OFF) 返回 LED_OK");
    CHECK(ctrl() == 0x00U, "OFF -> CTRL = 0x%02X (四路全灭, 不是只关指示灯)",
          (unsigned)ctrl());

    CHECK(Led_SetMode(&gLed, LED_MODE_BREATH_SLOW) == LED_OK, "SetMode(slow) 返回 LED_OK");
    CHECK(period() == 20U, "slow -> PERIOD = %u (期望 20, ~5 s)", (unsigned)period());
    CHECK(ctrl() == 0x01U, "掩码原本是 0 -> 点亮指示灯 -> CTRL = 0x%02X", (unsigned)ctrl());

    /* 已经有灯亮着时, 换模式不许把用户点亮的其它路关掉 */
    (void)Led_SetMask(&gLed, 0x06U);
    CHECK(Led_SetMode(&gLed, LED_MODE_BREATH_NORMAL) == LED_OK, "SetMode(normal) 返回 LED_OK");
    CHECK(period() == 100U, "normal -> PERIOD = %u (期望 100, ~1 s)", (unsigned)period());
    CHECK(ctrl() == 0x06U, "掩码 0x06 保持不变 -> CTRL = 0x%02X", (unsigned)ctrl());

    CHECK(Led_SetMode(&gLed, LED_MODE_BREATH_FAST) == LED_OK, "SetMode(fast) 返回 LED_OK");
    CHECK(period() == 1000U, "fast -> PERIOD = %u (期望 1000, ~100 ms)", (unsigned)period());

    CHECK(Led_SetMode(&gLed, (Led_ModeTypeDef)99) == LED_ERR_PARAM,
          "模式 99 返回 LED_ERR_PARAM");
    CHECK(Led_SetMode(&gLed, LED_MODE_COUNT) == LED_ERR_PARAM,
          "模式 LED_MODE_COUNT(4) 返回 LED_ERR_PARAM");
}

static void test_a_convert(void)
{
    printf("[A8] 模式 <-> 频率换算(含边界)\n");

    CHECK(Led_ModeToFreq(LED_MODE_BREATH_SLOW) == 20U, "slow -> 20");
    CHECK(Led_ModeToFreq(LED_MODE_BREATH_NORMAL) == 100U, "normal -> 100");
    CHECK(Led_ModeToFreq(LED_MODE_BREATH_FAST) == 1000U, "fast -> 1000");

    CHECK(Led_FreqToMode(40U) == LED_MODE_BREATH_SLOW, "40 -> slow (边界)");
    CHECK(Led_FreqToMode(41U) == LED_MODE_BREATH_NORMAL, "41 -> normal (边界)");
    CHECK(Led_FreqToMode(400U) == LED_MODE_BREATH_NORMAL, "400 -> normal (边界)");
    CHECK(Led_FreqToMode(401U) == LED_MODE_BREATH_FAST, "401 -> fast (边界)");

    CHECK(strcmp(Led_ModeName(LED_MODE_OFF), "off") == 0, "ModeName(OFF) = \"off\"");
    CHECK(strcmp(Led_ModeName(LED_MODE_BREATH_NORMAL), "normal") == 0,
          "ModeName(normal) = \"normal\"");
}

/* ========================================================================== */
/*  [B] 命令级(真实分发链路)                                                     */
/* ========================================================================== */

/*
 * 和 app/inter_app.c 的 Inter_App_Commands 里 LED 那一行一模一样(兜底项 + 处理函数),
 * 只是这里不把别的模块(制动/发射/采样/按键)拖进来编。改了生产代码里的那张表, 这里也要
 * 跟着改 —— 两边对不上, 这个测试测的就不是板子上跑的东西了。
 */
static const Svc_Net_CmdTypeDef LedTest_Commands[] = {
    { NULL, Monitor_App_Command, NULL, "0xF0..0xFF leds" },
};

#define LEDTEST_COMMAND_COUNT ((uint16_t)(sizeof(LedTest_Commands) / sizeof(LedTest_Commands[0])))

static void test_b_setup(void)
{
    printf("[B0] 注册命令表 + 起 led 任务\n");

    HostReg_Reset();
    Svc_Net_Cmd_Reset();

    CHECK(Svc_Net_RegisterCommands(LedTest_Commands, LEDTEST_COMMAND_COUNT) == SVC_OK,
          "Svc_Net_RegisterCommands(LED 表, %u 条) 成功", (unsigned)LEDTEST_COMMAND_COUNT);

    /* 2026-10-16 起 LED 命令只有兜底项(单字节 0xF0..0xFF), 没有精确名了 */
    CHECK(LEDTEST_COMMAND_COUNT == 1U, "LED 表共 %u 条 (只有 1 条兜底)",
          (unsigned)LEDTEST_COMMAND_COUNT);
    CHECK(LedTest_Commands[0].Name == NULL, "那条是兜底项(Name == NULL)");

    CHECK(Svc_Led_Init() == SVC_OK, "Svc_Led_Init 返回 SVC_OK");
    CHECK(Svc_Led_IsReady() != 0U, "Svc_Led_IsReady() = 1");

    /*
     * ⚠ 任务由**调用方**建(板子上是 app/monitor_app.c 的 Monitor_App_Start; 这里由测试
     *   扮演应用层的角色): 服务层只提供 Svc_Led_Task + 栈/优先级常量。2026-10-16 起
     *   服务不再自己起任务, 所以这里必须自己建, 否则后面所有命令都只会进队列没人执行。
     */
    CHECK(Osal_TaskCreate("led", Svc_Led_Task, NULL,
                          SVC_LED_STACK_WORDS, SVC_LED_PRIO, NULL) == OSAL_OK,
          "Osal_TaskCreate(led) 成功 (pthread 版的 led 任务)");
    CHECK(ctrl() == 0U, "初始化后 CTRL = 0x%02X (四路全灭)", (unsigned)ctrl());
}

static void test_b_commands(void)
{
    printf("[B1] 0xF0 / 0xF5 / 0xF6..0xFD / 0xFF <掩码>\n");

    /* 0xF0 = 关灯 —— 它是"状态命令"(和 0xF1..0xF3 一类), 回话是 led state -> off,
     * 不是掩码那一条; 效果上一样是四路全灭(CTRL = 0x00)。 */
    expect_b1(0xF0, "led state -> off (0xF0)", 0x00, 0, "0xF0 四路全灭");

    /* 0xFF <掩码> = 设四路掩码 */
    expect_b2(0xFF, 0x05, "led mask -> 0x05 (on=0,2)", 0x05, 0, "0xFF 5");
    expect_b2(0xFF, 0x0F, "led mask -> 0x0F", 0x0F, 0, "0xFF 15");
    expect_b2(0xFF, 0x00, "led mask -> 0x00 (on=none)", 0x00, 0, "0xFF 0");

    /* 0xF5 = 四路全亮 */
    expect_b1(0xF5, "led mask -> 0x0F (on=0,1,2,3)", 0x0F, 0, "0xF5 四路全亮");

    /* 0xF6..0xF9 = 单独点亮 LED0..3; 0xFA..0xFD = 单独熄灭 */
    expect_b2(0xFF, 0x00, "led mask -> 0x00", 0x00, 0, "前置: 全灭");

    expect_b1(0xF6 + 1, "led 1 ON, mask=0x02 (on=1)", 0x02, 0, "0xF7 = 点亮 LED1");
    expect_b1(0xF6 + 3, "led 3 ON, mask=0x0A (on=1,3)", 0x0A, 0, "0xF9 = 点亮 LED3");
    expect_b1(0xFA + 1, "led 1 OFF, mask=0x08 (on=3)", 0x08, 0, "0xFB = 熄灭 LED1");
    expect_b1(0xFA + 3, "led 3 OFF, mask=0x00 (on=none)", 0x00, 0, "0xFD = 熄灭 LED3");

    printf("[B2] 参数越界 / 参数没跟上来 —— 一律拒掉, 不动硬件\n");

    fire_b2(0xFF, 0x0F);                    /* 摆一个已知状态 0x0F */

    /* 0xFF 的参数 > 15: 拒 */
    expect_b2_noqueue(0xFF, 16, "out of range", 0x0F, 0, "0xFF 16 (越界)");
    expect_b2_noqueue(0xFF, 0xFF, "out of range", 0x0F, 0, "0xFF 255 (越界)");

    /* 只有命令、参数没跟上来(半包): 也拒, 并且不会去猜 */
    expect_b1_noqueue(0xFF, "needs its parameter byte", 0x0F, 0, "0xFF 没有参数");
    expect_b1_noqueue(0xFE, "needs its parameter byte", 0x0F, 0, "0xFE 没有参数");

    printf("[B3] 0xFE <频率/10> / 0xF4 查状态\n");

    fire_b2(0xFF, 0x03);                    /* 0x03 */

    expect_b2(0xFE, 25, "led freq -> 250 Hz", 0x03, 250, "0xFE 25 (=250 Hz)");
    expect_b2_noqueue(0xFE, 0, "out of range", 0x03, 250, "0xFE 0 (参数 0)");
    expect_b2_noqueue(0xFE, 101, "out of range", 0x03, 250, "0xFE 101 (=1010 Hz, 太快)");
    expect_b2_noqueue(0xFE, 255, "out of range", 0x03, 250, "0xFE 255 (越界)");

    /* 边界: 1 -> 10 Hz, 100 -> 1000 Hz 必须收下 */
    expect_b2(0xFE, 1, "led freq -> 10 Hz", 0x03, 10, "0xFE 1 (下限, 合法)");
    expect_b2(0xFE, 100, "led freq -> 1000 Hz", 0x03, 1000, "0xFE 100 (上限, 合法)");
    expect_b2(0xFE, 25, "led freq -> 250 Hz", 0x03, 250, "0xFE 25 (复位)");

    /* 0xF4 是纯查询: 只回话, 不改硬件, 也不进队列 */
    expect_b1_noqueue(0xF4, "led mask=0x03 on=0,1 freq=250Hz state=normal",
                      0x03, 250, "0xF4 查状态");

    printf("[B4] 0xF0..0xF3 的语义 = 老 RS-485 的 LED_STA_0..3\n");

    expect_b1(0xF0, "led state -> off (0xF0)", 0x00, 0, "0xF0 -> off");
    expect_b1(0xF1, "led state -> slow (0xF1)", 0x01, 20, "0xF1 -> slow");
    expect_b1(0xF2, "led state -> normal (0xF2)", 0x01, 100, "0xF2 -> normal");
    expect_b1(0xF3, "led state -> fast (0xF3)", 0x01, 1000, "0xF3 -> fast");

    /* "慢/标准/快"不能让用户点亮的其它路全灭: 先点亮 LED2, 再来一条 0xF2 */
    fire_b2(0xFF, 0x04);
    expect_b1(0xF2, "led state -> normal (0xF2)", 0x04, 100,
              "0xF2 保留已点亮的 LED2");

    printf("[B5] 命名空间: 兜底项只认 0xF0..0xFF, 不能抢别的命令\n");

    HostNet_ReplyReset();
    CHECK(dispatch("PWMID") == 0, "PWMID(文本) 没被 LED 兜底项抢走(返回 0 = 没人认领)");
    CHECK(HostNet_ReplyCount() == 0U, "PWMID 没有产生任何 LED 回复");
    CHECK(dispatch("MOTORSTAT") == 0, "MOTORSTAT(文本) 没被 LED 兜底项抢走");
    CHECK(dispatch("STATUS") == 0, "STATUS(文本) 没被 LED 兜底项抢走");
    CHECK(dispatch("LEDSTAT") == 0, "LEDSTAT(老文本命令已删) 谁都不认领");

    /* 别的模块的字节段也不能被 LED 认领 */
    HostNet_ReplyReset();
    {
        static const uint8_t brakeBytes[] = { 0xA0, 0xA1, 0xA5 };
        static const uint8_t moverBytes[] = { 0xD0, 0xD1 };
        static const uint8_t sampBytes[]  = { 0xE0, 0xE2 };
        static const uint8_t keyBytes[]   = { 0xAA, 0xAC };

        CHECK(dispatch_bytes(brakeBytes, (uint16_t)sizeof(brakeBytes)) == 0,
              "0xA0/0xA1/0xA5(制动) 没被 LED 认领");
        CHECK(dispatch_bytes(moverBytes, (uint16_t)sizeof(moverBytes)) == 0,
              "0xD0/0xD1(发射) 没被 LED 认领");
        CHECK(dispatch_bytes(sampBytes, (uint16_t)sizeof(sampBytes)) == 0,
              "0xE0/0xE2(采样) 没被 LED 认领");
        CHECK(dispatch_bytes(keyBytes, (uint16_t)sizeof(keyBytes)) == 0,
              "0xAA/0xAC(按键) 没被 LED 认领");
        CHECK(HostNet_ReplyCount() == 0U, "这些字节没有产生任何 LED 回复");
    }

    /* 空报文 / 只有 0x00: 也不认领 */
    {
        static const uint8_t zeros[] = { 0x00, 0x01, 0x7F };

        CHECK(dispatch_bytes(zeros, (uint16_t)sizeof(zeros)) == 0,
              "0x00/0x01/0x7F 没被 LED 认领");
    }

    printf("[B5b] 一条报文里粘多个 LED 字节: 全部按顺序处理\n");

    /* 0xFF 0x00(全灭) + 0xF6(点 LED0) + 0xFE 0x64(=1000 Hz) —— 三件事一个报文 */
    {
        static const uint8_t combo[] = { 0xFF, 0x00, 0xF6, 0xFE, 0x64 };
        Svc_Led_SnapshotTypeDef snap;

        (void)Svc_Led_GetSnapshot(&snap);
        HostNet_ReplyReset();

        CHECK(dispatch_bytes(combo, (uint16_t)sizeof(combo)) == 1, "粘在一起的 5 个字节被认领");
        CHECK(wait_commands(snap.Commands + 3U), "三条命令都被 led 任务执行完");
        CHECK(ctrl() == 0x01U, "CTRL = 0x%02X (期望 0x01: 只有 LED0)", (unsigned)ctrl());
        CHECK(period() == 1000U, "PERIOD = %u (期望 1000)", (unsigned)period());
        CHECK(HostNet_ReplyCount() == 3U, "回了 %u 句话(一条命令一句)",
              (unsigned)HostNet_ReplyCount());
    }
}

static void test_b_link_indicator(void)
{
    Svc_Led_SnapshotTypeDef snap;
    uint32_t                before;

    printf("[B6] 网口在线指示: 只动 LED0, 不碰用户点亮的其它路\n");

    /* 用户先点亮 LED1 和 LED3 */
    fire_b2(0xFF, 0x00);
    fire_b1(0xF6 + 1U);
    fire_b1(0xF6 + 3U);
    CHECK(ctrl() == 0x0AU, "前置状态: CTRL = 0x%02X (LED1+LED3)", (unsigned)ctrl());

    /* 上位机连上来: app 层就是这么调的(设频率 + 点亮指示灯那一路)。
     * 计数在发命令之前抓, 否则异步执行会让 wait 的目标值算错。 */
    (void)Svc_Led_GetSnapshot(&snap);
    before = snap.Commands;

    (void)Svc_Led_RequestFreq(100U);
    (void)Svc_Led_RequestEnable(0U, 1U);

    CHECK(wait_commands(before + 2U), "网口在线两条命令执行完成");
    CHECK(ctrl() == 0x0BU, "上线后 CTRL = 0x%02X (期望 0x0B: LED0+LED1+LED3)",
          (unsigned)ctrl());
    CHECK(period() == 100U, "上线后 PERIOD = %u (在线指示 100 = ~1 s)", (unsigned)period());

    /* 断线: 只灭指示灯, 用户的 LED1/LED3 必须还在 */
    (void)Svc_Led_GetSnapshot(&snap);
    before = snap.Commands;

    (void)Svc_Led_RequestEnable(0U, 0U);

    CHECK(wait_commands(before + 1U), "网口断开命令执行完成");
    CHECK(ctrl() == 0x0AU,
          "★ 断线后 CTRL = 0x%02X (期望 0x0A: 只有指示灯灭, LED1/LED3 保持点亮)",
          (unsigned)ctrl());
}

/* ========================================================================== */

int main(void)
{
    /* 中文输出: 控制台切到 UTF-8, 免得 Windows 代码页把断言名弄成乱码 */
    (void)SetConsoleOutputCP(65001);

    /* 无缓冲: 测试里起了一条真任务线程, 万一卡住, 输出被管道缓冲住就完全看不到
     * 卡在哪一步(第一次调试这个测试时就是这么被坑的)。 */
    (void)setvbuf(stdout, NULL, _IONBF, 0U);

    printf("== 4 路呼吸灯 breath_led 驱动 + 命令链路 宿主机测试 ==\n\n");

    test_a_constants();
    test_a_init();
    test_a_enable();
    test_a_mask();
    test_a_freq();
    test_a_mode();
    test_a_convert();

    test_b_setup();
    test_b_commands();
    test_b_link_indicator();

    printf("\n----------------------------------------\n");
    printf("PASS %d, FAIL %d\n", g_pass, g_fail);

    return (g_fail == 0) ? 0 : 1;
}
