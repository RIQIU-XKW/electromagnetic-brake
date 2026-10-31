/**
 *******************************************************************************
 * @file    test_current_loop.c
 * @brief   电流负反馈闭环的主机测试(跑真的 service_pid.c + service_current_loop.c)
 *
 * 验的是"改代码最容易改坏"的那几条:
 *   1. 5 个桥各自钳在自己的上限 37/40/28/45/75 %, 谁也别想超
 *   2. 每拍占空比增量不超过爬坡步长(2 %/拍)
 *   3. 积分是 float 累加(不是整数定点), 且顶到上限后不再继续积(抗饱和)
 *   4. 目标为 0 时占空比爬回 0, 然后自己把五桥和全局调制关掉
 *   5. 外部停(Svc_CurLoop_Stop, 即 0xA5/过流/切回占空比模式)之后, 闭环不许
 *      再把桥打开, 也不许再往占空比里写 0 去抹掉别人的值
 *   6. 外部停之后再收到电流指令, 能重新接管(最后一条命令说了算)
 *******************************************************************************
 */

#include <math.h>
#include <stdio.h>

#include "host_stubs.h"

#include "service/Algorithm_module/service_current_loop.h"
#include "service/Algorithm_module/service_pid.h"

/* ========================================================================== */
/*  断言小工具                                                                 */
/* ========================================================================== */

static int gChecks;
static int gFails;

static void Check(int Cond, const char *What)
{
    gChecks++;
    if (Cond == 0) {
        gFails++;
        printf("  FAIL  %s\n", What);
    }
}

static void CheckNear(float Got, float Want, float Tol, const char *What)
{
    gChecks++;
    if (fabsf(Got - Want) > Tol) {
        gFails++;
        printf("  FAIL  %s (got %.4f, want %.4f +/- %.4f)\n", What, Got, Want, Tol);
    }
}

/** 各桥占空比上限(和 service_current_loop.h 里的宏一一对应)。 */
static const float kLimit[5] = { 37.0f, 40.0f, 28.0f, 45.0f, 75.0f };

/* ========================================================================== */
/*  1. PID 本身: float 积分 + 抗饱和                                            */
/* ========================================================================== */

static void TestPidFloatIntegral(void)
{
    Svc_Pid_StateTypeDef pid;
    float out;

    printf("\n[1] PID: 积分是 float 累加(不是整数定点)\n");

    /*
     * Ki = 1, 误差 0.1 A, dt = 0.1 s -> 每拍积分加 0.01。
     * 整数定点(比如把积分放大 100 倍存 uint16)在这里会被截断成 0, 三拍之后
     * 输出还是 0; float 应该老老实实累到 0.03。
     */
    Svc_Pid_InitSetup(&pid, 0.0f, 1.0f, 0.0f, -1000.0f, 1000.0f, 0.0f);

    out = Svc_Pid_Update(&pid, 0.1f, 0.0f, 0.1f);
    CheckNear(out, 0.01f, 1e-6f, "first cycle output = Ki*err*dt");
    CheckNear(pid.Integ, 0.01f, 1e-6f, "integral accumulates as float");

    (void)Svc_Pid_Update(&pid, 0.1f, 0.0f, 0.1f);
    out = Svc_Pid_Update(&pid, 0.1f, 0.0f, 0.1f);
    CheckNear(pid.Integ, 0.03f, 1e-6f, "integral after 3 cycles = 0.03");
    CheckNear(out, 0.03f, 1e-6f, "output after 3 cycles = 0.03");
    Check(out > 0.0f && out < 1.0f, "no integer truncation (output is fractional)");
}

static void TestPidAntiWindup(void)
{
    Svc_Pid_StateTypeDef pid;
    float integAtLimit;
    float out;
    int   i;

    printf("\n[2] PID: 顶到上限后停止积分(抗饱和)\n");

    /*
     * 故意让积分慢慢爬上来(Kp=1, Ki=10, dt=0.01): 误差 1 A 时每拍积分加 0.1,
     * 大约 90 拍才顶到上限 10 %。之后积分必须停在那儿 —— 没有抗饱和的话, 400 拍
     * 下来积分能到 40, 目标一降就得先花很久退饱和。
     */
    Svc_Pid_InitSetup(&pid, 1.0f, 10.0f, 0.0f, 0.0f, 10.0f, 0.0f);

    for (i = 0; i < 400; i++) {
        (void)Svc_Pid_Update(&pid, 1.0f, 0.0f, 0.01f);
    }

    integAtLimit = pid.Integ;
    Check(integAtLimit < 9.6f, "integral stops at the saturation point (no windup)");

    /* 再多推 400 拍, 积分不许再涨 */
    for (i = 0; i < 400; i++) {
        (void)Svc_Pid_Update(&pid, 1.0f, 0.0f, 0.01f);
    }
    CheckNear(pid.Integ, integAtLimit, 1e-3f, "integral frozen while saturated");

    /*
     * 误差反号(目标 0, 实际 1 A)之后, 输出应当**第一拍就离开上限**:
     * 抗饱和时输出约 -1 + 8.9 = 7.9 %; 没抗饱和时会算成 -1 + 40 = 39 -> 仍然顶在 10 %。
     */
    out = Svc_Pid_Update(&pid, 0.0f, 1.0f, 0.01f);
    Check(out < 9.95f, "output leaves saturation on the first reversed cycle");
    Check(out > 5.0f, "output keeps most of the accumulated integral (no reset jump)");
}

static void TestPidSlew(void)
{
    Svc_Pid_StateTypeDef pid;
    float prev;
    float out;
    int   i;
    int   ok = 1;

    printf("\n[3] PID: 每拍输出增量受爬坡步长限制\n");

    Svc_Pid_InitSetup(&pid, 100.0f, 0.0f, 0.0f, 0.0f, 100.0f, 2.0f);

    prev = 0.0f;
    for (i = 0; i < 10; i++) {
        out = Svc_Pid_Update(&pid, 1.0f, 0.0f, 0.01f);   /* 误差大, 想一步到顶 */
        if ((out - prev) > 2.0f + 1e-4f) {
            ok = 0;
        }
        prev = out;
    }

    Check(ok != 0, "increment never exceeds SlewStep");
    CheckNear(prev, 20.0f, 0.01f, "10 cycles of 2 %/cycle = 20 %");
}

/* ========================================================================== */
/*  2. 闭环: 各桥上限 / 爬坡 / 关闭 / 接管                                      */
/* ========================================================================== */

static void TestLimits(void)
{
    Svc_CurLoop_SnapshotTypeDef snap;
    int i;
    int ok = 1;

    printf("\n[4] 闭环: 5 个桥各自钳在自己的上限\n");

    (void)Svc_CurLoop_GetSnapshot(&snap);
    for (i = 0; i < 5; i++) {
        CheckNear(snap.LimitPercent[i], kLimit[i], 1e-4f, "per-bridge duty limit");
    }
    Check((int)snap.LimitPercent[2] == 28 && (int)snap.LimitPercent[4] == 75,
          "limits are not all the same (37/40/28/45/75)");

    /*
     * 目标拉满 15 A, 但反馈一直是 0 A(相当于电流上不来) -> 每个桥都会顶到
     * 自己的上限, 一拍都不许超。
     */
    Host_SetAllCurrentMa(0);
    (void)Svc_CurLoop_SetTargetA(15.0f);

    for (i = 0; i < 300; i++) {
        int b;

        Host_RunLoop(1);
        for (b = 0; b < 5; b++) {
            if (Host_GetDutyPermille(b) > (uint32_t)(kLimit[b] * 10.0f + 0.5f)) {
                ok = 0;
            }
        }
    }

    Check(ok != 0, "duty never exceeds its own bridge limit");
    (void)Svc_CurLoop_GetSnapshot(&snap);
    for (i = 0; i < 5; i++) {
        printf("    bridge %d: limit %5.1f %%  snap %7.3f %%  permille %u\n",
               i + 1, (double)kLimit[i], (double)snap.DutyPercent[i],
               (unsigned)Host_GetDutyPermille(i));
    }
    for (i = 0; i < 5; i++) {
        const uint32_t maxPermille = (uint32_t)(kLimit[i] * 10.0f + 0.5f);

        /*
         * 顶到上限时实际停在"上限减不到一个爬坡步长(2 %)"的地方, 而不是正好等于上限:
         * 抗饱和把积分冻结在"刚够顶到上限"的那个值上, 输出因此停在上限下方一点点。
         * 这里要的就是这两条: 不许超, 也不许离上限太远(差太多说明爬坡被卡住了)。
         */
        Check(Host_GetDutyPermille(i) <= maxPermille, "duty does not exceed the limit");
        Check(Host_GetDutyPermille(i) + 21U >= maxPermille,
              "duty sits within one slew step of the limit");
    }
    Check(Host_GetBridgeEnable() == 1, "bridges enabled while the loop runs");
    Check(Host_GetGlobalEnable() == 1, "global modulation enabled while the loop runs");

    /*
     * 投入时死区要被设成 100 ns(硬件允许的最小值): 闭环在低占空比下也要出电流,
     * 死区越长低段失真越大。但**只写一次** —— 上位机之后在 PWM 栏发 0xDC 帧改掉,
     * 闭环不许再刷回去。
     */
    Check(Host_GetDeadNs() == 100U, "dead time defaults to 100 ns when the loop engages");
    Check(Host_GetDeadWrites() == 1U, "dead time written only once (at engage)");

    /*
     * 模拟上位机在 PWM 栏发一条死区帧(500 ns), 再跑 50 拍闭环:
     * 死区必须还是 500 ns, 一次都不许被闭环改写。
     */
    Host_ApplyManualDeadNs(500U);       /* 模拟 PWM 栏发 0xDC 帧 */
    Host_RunLoop(50);    Check(Host_GetDeadNs() == 500U, "manual 0xDC dead time survives the running loop");
    Check(Host_GetDeadWrites() == 2U, "loop does not rewrite the dead time every cycle");
}

static void TestRampAndOff(void)
{
    uint32_t prev;
    uint32_t now;
    uint32_t writesAtRest;
    int      i;
    int      ok = 1;

    printf("\n[5] 闭环: 目标 0 时爬回 0, 然后自己关输出\n");

    /*
     * 上一步各桥已经顶在上限。现在把目标改 0: 占空比必须每拍最多降 2 %
     * (千分比 20), 一路退到 0, 退干净之后闭环自己关五桥 + 关全局调制。
     */
    (void)Svc_CurLoop_SetTargetA(0.0f);

    prev = Host_GetDutyPermille(0);
    for (i = 0; i < 400; i++) {
        Host_RunLoop(1);
        now = Host_GetDutyPermille(0);
        if (prev > now && (prev - now) > 21U) {
            ok = 0;
        }
        prev = now;
    }

    Check(ok != 0, "ramp-down step never exceeds the slew limit");
    for (i = 0; i < 5; i++) {
        Check(Host_GetDutyPermille(i) == 0U, "duty ramped back to 0");
    }
    Check(Host_GetBridgeEnable() == 0, "loop switched the bridges off itself");
    Check(Host_GetGlobalEnable() == 0, "loop switched global modulation off itself");

    /* 停稳之后不该再写占空比(否则会抹掉占空比模式刚设好的值) */
    writesAtRest = Host_GetDutyWrites();
    Host_RunLoop(50);
    Check(Host_GetDutyWrites() == writesAtRest,
          "no duty writes once ramped down (leaves PWM to the duty-mode chain)");
}

static void TestExternalStop(void)
{
    uint32_t writesBefore;
    int      i;

    printf("\n[6] 闭环: 外部停(0xA5/过流/切回占空比)之后不许自己把桥打开\n");

    /* 先让它跑起来 */
    Host_SetAllCurrentMa(500);                  /* 已有 0.5 A */
    (void)Svc_CurLoop_SetTargetA(5.0f);
    Host_RunLoop(20);

    Check(Host_GetBridgeEnable() == 1, "loop took over the power stage");
    Check(Host_GetDutyPermille(0) > 0U, "duty is non-zero while regulating");

    /* 模拟 0xA5 / 过流: 外部直接关功率级, 再让闭环放手 */
    Host_ForcePowerStageOff();
    (void)Svc_CurLoop_Stop();

    writesBefore = Host_GetDutyWrites();

    for (i = 0; i < 50; i++) {
        Host_RunLoop(1);

        if (Host_GetBridgeEnable() != 0) {
            Check(0, "loop must not re-enable the bridges after an external stop");
            break;
        }
        if (Host_GetGlobalEnable() != 0) {
            Check(0, "loop must not re-enable modulation after an external stop");
            break;
        }
    }
    Check(Host_GetBridgeEnable() == 0, "bridges stay off after the external stop");
    Check(Host_GetGlobalEnable() == 0, "modulation stays off after the external stop");
    Check(Host_GetDutyWrites() == writesBefore,
          "no duty writes after an external stop");

    {
        Svc_CurLoop_SnapshotTypeDef snap;

        (void)Svc_CurLoop_GetSnapshot(&snap);
        CheckNear(snap.TargetA, 0.0f, 1e-6f, "target cleared by the stop");
        Check(snap.Active == 0, "snapshot says the loop is not active");
        for (i = 0; i < 5; i++) {
            CheckNear(snap.DutyPercent[i], 0.0f, 1e-6f, "snapshot duty is 0 after the stop");
        }
    }

    /* 最后一条命令说了算: 新指令进来还能重新接管 */
    printf("\n[7] 闭环: 外部停之后再收到电流指令, 重新接管\n");
    Host_SetAllCurrentMa(0);
    (void)Svc_CurLoop_SetTargetA(1.0f);
    Host_RunLoop(5);

    Check(Host_GetBridgeEnable() == 1, "loop takes over again on a new command");
    Check(Host_GetGlobalEnable() == 1, "modulation on again on a new command");
    Check(Host_GetDutyPermille(0) > 0U && Host_GetDutyPermille(0) <= 20U,
          "duty ramps up from 0 at the slew rate (5 cycles = 10 %)");

    /* 收尾: 关掉, 别把状态留给下一段测试 */
    (void)Svc_CurLoop_SetTargetA(0.0f);
    Host_RunLoop(200);
}

static void TestNoSharedState(void)
{
    Svc_CurLoop_SnapshotTypeDef snap;
    int i;

    printf("\n[8] 闭环: 5 路反馈各走各的(不是共用一个采样)\n");

    /*
     * 让 5 路反馈都不一样, 目标一样: 实际电流越小的桥, 占空比应该爬得越高
     * (至少不能 5 路完全一样) —— 如果 5 路共用一个 PID/一个采样, 这里会是同一个值。
     */
    (void)Svc_CurLoop_SetTargetA(3.0f);
    Host_SetCurrentMa(0, 0);
    Host_SetCurrentMa(1, 0);
    Host_SetCurrentMa(2, 0);
    Host_SetCurrentMa(3, 0);
    Host_SetCurrentMa(4, 0);
    Host_RunLoop(10);

    (void)Svc_CurLoop_GetSnapshot(&snap);
    printf("    actual A:");
    for (i = 0; i < 5; i++) {
        printf(" %.2f", (double)snap.ActualA[i]);
    }
    printf("\n");

    /* 反馈换成各不相同的一组, 跑一拍看快照是不是真按路分开的 */
    Host_SetCurrentMa(0, 100);
    Host_SetCurrentMa(1, 200);
    Host_SetCurrentMa(2, 300);
    Host_SetCurrentMa(3, 400);
    Host_SetCurrentMa(4, 500);
    Host_RunLoop(1);

    (void)Svc_CurLoop_GetSnapshot(&snap);
    CheckNear(snap.ActualA[0], 0.1f, 1e-4f, "bridge 1 feedback read separately");
    CheckNear(snap.ActualA[2], 0.3f, 1e-4f, "bridge 3 feedback read separately");
    CheckNear(snap.ActualA[4], 0.5f, 1e-4f, "bridge 5 feedback read separately");
    Check((snap.Cycles > 0U) && (snap.Cmds >= 5U), "cycles and commands counted");

    (void)Svc_CurLoop_SetTargetA(0.0f);
    Host_RunLoop(200);
}

/* ========================================================================== */

int main(void)
{
    Svc_CurLoop_SnapshotTypeDef snap;

    printf("== current loop host test (5 independent PIDs) ==\n");

    Host_SetLogVerbose(0);              /* 想看闭环自己打的行就把这里改 1 */
    Host_SetAllCurrentMa(0);

    Check(Svc_CurLoop_Init() == SVC_OK, "Svc_CurLoop_Init returns OK");

    (void)Svc_CurLoop_GetSnapshot(&snap);
    Check(snap.Ready == 1, "snapshot ready");
    Check(snap.Active == 0, "idle after init (no output)");

    printf("\n[0] 上电即空转: 目标 0, 一拍都不许写 PWM\n");
    Host_RunLoop(50);
    Check(Host_GetDutyWrites() == 0U, "no duty writes while idle");
    Check(Host_GetBridgeEnable() == 0, "bridges stay off while idle");
    Check(Host_GetGlobalEnable() == 0, "modulation stays off while idle");

    TestPidFloatIntegral();
    TestPidAntiWindup();
    TestPidSlew();

    TestLimits();
    TestRampAndOff();
    TestExternalStop();
    TestNoSharedState();

    printf("\n%d checks, %d failed\n", gChecks, gFails);

    if (gFails == 0) {
        printf("RESULT: all checks passed\n");
        return 0;
    }

    printf("RESULT: FAILED\n");
    return 1;
}
