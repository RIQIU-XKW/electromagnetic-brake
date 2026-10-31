/**
 * PL 功率级 PWM IP(pwm_axi_ip v1.0)驱动逻辑的宿主机测试
 *
 * 把移植到 bsp/pwm 的**真实驱动**编到 PC 上, 用一个 64 KB 的寄存器文件替身
 * 承接 Xil_In32/Xil_Out32, 逐条验证寄存器编码:
 *   - ID 寄存器 / 寄存器读写自检
 *   - 频率 ↔ 周期 换算与取整
 *   - 占空比 permille/percent → DUTY 寄存器
 *   - 相位与交错
 *   - 死区位域打包(A 低 16 位 / B 高 16 位)
 *   - 总使能 / 反相 / 全部关断
 *   - 5 个 H 桥的使能位与线圈驱动/直流/释放/短路四种模式
 *   - 越界参数的错误码
 *
 * 这里验证的是**驱动逻辑与寄存器编码**; IP 的时序(死区、交叠、过流保护)
 * 由 IP 自己的仿真 + 上板示波器验证。
 */

#include <stdio.h>
#include <string.h>

#include "bsp/pwm/pwm.h"

/* host_regfile.c */
void     HostReg_Reset(void);
uint32_t HostReg_Read(uint32_t offset);
void     HostReg_Write(uint32_t offset, uint32_t value);
uint32_t HostReg_Writes(void);

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

#define ACLK_HZ   50000000U

/* ========================================================================== */
/*  1. 初始化 / ID / 自检                                                      */
/* ========================================================================== */

static void test_init(Pwm_HandleTypeDef *p)
{
    printf("[1] 初始化 / ID / 寄存器自检\n");

    HostReg_Reset();

    CHECK(Pwm_Init(p, PWM_BASEADDR, ACLK_HZ) == PWM_AXI_OK, "Pwm_Init 返回 OK");
    CHECK(p->base == PWM_BASEADDR, "base = 0x%08X (pl.dtsi 里是 0x43C10000)",
          (unsigned)p->base);
    CHECK(Pwm_ReadId(p) == PWM_AXI_ID_VALUE, "ID = 0x%08X (期望 0x%08X)",
          (unsigned)Pwm_ReadId(p), (unsigned)PWM_AXI_ID_VALUE);
    CHECK(Pwm_SelfTest(p) == PWM_AXI_OK, "Pwm_SelfTest 通过(寄存器能写能读回)");

    /* 自检会把 PERIOD/使能写乱, 后面先复位成干净状态 */
    Pwm_Stop(p);
    CHECK(Pwm_GetEnableMask(p) == 0U, "Pwm_Stop 后使能掩码 = 0x%03X",
          (unsigned)Pwm_GetEnableMask(p));
}

/* ========================================================================== */
/*  2. 频率 ↔ 周期                                                             */
/* ========================================================================== */

static void test_frequency(Pwm_HandleTypeDef *p)
{
    printf("[2] 频率 ↔ 周期 (aclk = 50 MHz)\n");

    CHECK((Pwm_SetFrequencyHz(p, 20000U) == PWM_AXI_OK) &&
          (HostReg_Read(PWM_AXI_REG_PERIOD) == 2500U),
          "20 kHz -> PERIOD = %u (期望 2500)", (unsigned)HostReg_Read(PWM_AXI_REG_PERIOD));

    CHECK(Pwm_GetFrequencyHz(p) == 20000U, "GetFrequencyHz 回读 = %u",
          (unsigned)Pwm_GetFrequencyHz(p));

    CHECK((Pwm_SetFrequencyHz(p, 100000U) == PWM_AXI_OK) &&
          (HostReg_Read(PWM_AXI_REG_PERIOD) == 500U),
          "100 kHz -> PERIOD = %u (期望 500)", (unsigned)HostReg_Read(PWM_AXI_REG_PERIOD));

    /* 取整: 3 kHz -> (50e6 + 1500)/3000 = 16667.16 -> 16667 */
    (void)Pwm_SetFrequencyHz(p, 3000U);
    CHECK(HostReg_Read(PWM_AXI_REG_PERIOD) == 16667U, "3 kHz -> PERIOD = %u (四舍五入)",
          (unsigned)HostReg_Read(PWM_AXI_REG_PERIOD));

    CHECK(Pwm_SetFrequencyHz(p, 0U) == PWM_AXI_ERR_ARG, "hz = 0 被拒绝");

    /* hz 在 aclk 和 2*aclk 之间时周期取整成 1(也就是实际跑在 aclk 上), 不算错;
     * 只有周期会算成 0 的 hz 才被拒绝。 */
    (void)Pwm_SetFrequencyHz(p, ACLK_HZ + 1U);
    CHECK(HostReg_Read(PWM_AXI_REG_PERIOD) == 1U,
          "hz 略大于 aclk -> PERIOD = %u (取整成 1, 不报错)",
          (unsigned)HostReg_Read(PWM_AXI_REG_PERIOD));
    CHECK(Pwm_SetFrequencyHz(p, 4U * ACLK_HZ) == PWM_AXI_ERR_RANGE,
          "hz 远超 aclk 被拒绝(周期 < 1)");

    (void)Pwm_SetFrequencyHz(p, 20000U);        /* 回到 2500 方便后面的期望值 */
}

/* ========================================================================== */
/*  3. 占空比                                                                  */
/* ========================================================================== */

static void test_duty(Pwm_HandleTypeDef *p)
{
    printf("[3] 占空比 (PERIOD = 2500)\n");

    CHECK((Pwm_SetDutyPermille(p, 0, 300U) == PWM_AXI_OK) &&
          (HostReg_Read(PWM_AXI_REG_DUTY0) == 750U),
          "桥 0 30.0%% -> DUTY0 = %u (期望 750)", (unsigned)HostReg_Read(PWM_AXI_REG_DUTY0));

    CHECK((Pwm_SetDutyPercent(p, 1, 25U) == PWM_AXI_OK) &&
          (HostReg_Read(PWM_AXI_REG_DUTY1) == 625U),
          "桥 1 25%% -> DUTY1 = %u (期望 625)", (unsigned)HostReg_Read(PWM_AXI_REG_DUTY1));

    CHECK((Pwm_SetDutyPermille(p, 4, 1000U) == PWM_AXI_OK) &&
          (HostReg_Read(PWM_AXI_REG_DUTY4) == 2500U),
          "桥 4 100.0%% -> DUTY4 = %u (期望 2500, 全通)",
          (unsigned)HostReg_Read(PWM_AXI_REG_DUTY4));

    CHECK(Pwm_SetDutyPermille(p, 0, 1001U) == PWM_AXI_ERR_RANGE, "1001‰ 被拒绝");
    CHECK(Pwm_SetDutyPermille(p, PWM_AXI_BRIDGE_COUNT, 100U) == PWM_AXI_ERR_ARG,
          "桥号越界被拒绝");
    CHECK(Pwm_GetDutyPermille(p, 0) == 300U, "GetDutyPermille(桥0) = %u",
          (unsigned)Pwm_GetDutyPermille(p, 0));
}

/* ========================================================================== */
/*  4. 相位与交错                                                              */
/* ========================================================================== */

static void test_phase(Pwm_HandleTypeDef *p)
{
    uint32_t i;
    int      ok = 1;
    uint32_t want[PWM_AXI_BRIDGE_COUNT] = { 0U, 500U, 1000U, 1500U, 2000U };

    printf("[4] 相位与交错 (PERIOD = 2500, 5 相交错 = 0/72/144/216/288 度)\n");

    CHECK(Pwm_Interleave(p, PWM_AXI_BRIDGE_COUNT) == PWM_AXI_OK, "Pwm_Interleave(5) OK");

    for (i = 0U; i < PWM_AXI_BRIDGE_COUNT; i++) {
        uint32_t ph = HostReg_Read(PWM_AXI_REG_PHASE0 + (i * PWM_AXI_PHASE_STRIDE));

        if (ph != want[i]) {
            ok = 0;
        }
    }

    CHECK(ok == 1, "PHASE0..4 = %u %u %u %u %u",
          (unsigned)HostReg_Read(PWM_AXI_REG_PHASE0),
          (unsigned)HostReg_Read(PWM_AXI_REG_PHASE1),
          (unsigned)HostReg_Read(PWM_AXI_REG_PHASE2),
          (unsigned)HostReg_Read(PWM_AXI_REG_PHASE3),
          (unsigned)HostReg_Read(PWM_AXI_REG_PHASE4));

    CHECK((Pwm_SetPhasePermille(p, 1, 500U) == PWM_AXI_OK) &&
          (Pwm_GetPhaseCycles(p, 1) == 1250U),
          "桥 1 相位 50%% -> %u (期望 1250)", (unsigned)Pwm_GetPhaseCycles(p, 1));
}

/* ========================================================================== */
/*  5. 死区                                                                    */
/* ========================================================================== */

static void test_deadtime(Pwm_HandleTypeDef *p)
{
    uint32_t reg;

    printf("[5] 死区(两条腿打包在同一个寄存器)\n");

    CHECK(Pwm_SetBridgeDeadtime(p, 0, 25U) == PWM_AXI_OK, "桥 0 死区 25 个周期 OK");
    reg = HostReg_Read(PWM_AXI_REG_DEAD0);
    CHECK(reg == (25U | (25U << 16)), "DEAD0 = 0x%08X (A 低 16 位 / B 高 16 位)",
          (unsigned)reg);

    CHECK((Pwm_GetDeadtimeCycles(p, 0, PWM_AXI_LEG_A) == 25U) &&
          (Pwm_GetDeadtimeCycles(p, 0, PWM_AXI_LEG_B) == 25U),
          "回读 A = %u, B = %u",
          (unsigned)Pwm_GetDeadtimeCycles(p, 0, PWM_AXI_LEG_A),
          (unsigned)Pwm_GetDeadtimeCycles(p, 0, PWM_AXI_LEG_B));

    CHECK((Pwm_SetDeadtimeCycles(p, 1, PWM_AXI_LEG_A, 50U) == PWM_AXI_OK) &&
          (Pwm_GetDeadtimeCycles(p, 1, PWM_AXI_LEG_A) == 50U),
          "单独设桥 1 腿 A = 50");

    CHECK(Pwm_SetBridgeDeadtime(p, PWM_AXI_BRIDGE_COUNT, 10U) == PWM_AXI_ERR_ARG,
          "桥号越界被拒绝");
}

/* ========================================================================== */
/*  6. 总控制                                                                  */
/* ========================================================================== */

static void test_global(Pwm_HandleTypeDef *p)
{
    printf("[6] 总使能 / 反相 / 全部关断\n");

    Pwm_GlobalEnable(p, 1);
    CHECK(((HostReg_Read(PWM_AXI_REG_CTRL) & PWM_AXI_CTRL_GLB_EN) != 0U) &&
          (Pwm_IsGlobalEnabled(p) != 0), "GLB_EN 置位, CTRL = 0x%08X",
          (unsigned)HostReg_Read(PWM_AXI_REG_CTRL));

    Pwm_SetInvert(p, 1);
    CHECK((HostReg_Read(PWM_AXI_REG_CTRL) & PWM_AXI_CTRL_INV_PWM) != 0U,
          "INV_PWM 置位, CTRL = 0x%08X", (unsigned)HostReg_Read(PWM_AXI_REG_CTRL));

    Pwm_SetInvert(p, 0);
    Pwm_SetHalfBridgeEnable(p, 3, 1);
    CHECK((Pwm_GetEnableMask(p) & PWM_AXI_EN_BIT(3)) != 0U, "使能半桥 3 -> mask 0x%03X",
          (unsigned)Pwm_GetEnableMask(p));

    Pwm_Stop(p);
    CHECK((HostReg_Read(PWM_AXI_REG_CTRL) == 0U) &&
          (HostReg_Read(PWM_AXI_REG_PWM_EN) == 0U),
          "Pwm_Stop 后 CTRL = 0x%08X, PWM_EN = 0x%08X",
          (unsigned)HostReg_Read(PWM_AXI_REG_CTRL),
          (unsigned)HostReg_Read(PWM_AXI_REG_PWM_EN));
}

/* ========================================================================== */
/*  7. 线圈(刹车)四种模式                                                      */
/* ========================================================================== */

static void test_coil(Pwm_HandleTypeDef *p)
{
    uint32_t mode_dc;
    uint32_t mode_rel;
    uint32_t mode_sh;

    printf("[7] 线圈驱动 / 直流 / 快释放 / 短路制动\n");

    /* ---- 斩波驱动: 先配置, 再使能 ---------------------------------------
     * Pwm_CoilXxx() 只配置(模式 + 占空比), **不会**打开输出: 这是驱动刻意的
     * 设计 —— 配置不应意外给功率级上电.  使能要靠下面两步显式完成。
     * -------------------------------------------------------------------- */
    CHECK((Pwm_CoilDrive(p, 2, 300U) == PWM_AXI_OK) &&
          (Pwm_GetDutyPermille(p, 2) == 300U),
          "CoilDrive(桥2, 30%%) -> 占空比 %u", (unsigned)Pwm_GetDutyPermille(p, 2));

    CHECK((Pwm_GetEnableMask(p) == 0U) && (Pwm_IsGlobalEnabled(p) == 0),
          "CoilDrive 本身不使能输出(mask = 0x%03X, glb = %d) —— 安全设计",
          (unsigned)Pwm_GetEnableMask(p), Pwm_IsGlobalEnabled(p));

    Pwm_SetHalfBridgeEnable(p, (int)PWM_AXI_HB_INDEX(2, PWM_AXI_LEG_A), 1);
    Pwm_SetHalfBridgeEnable(p, (int)PWM_AXI_HB_INDEX(2, PWM_AXI_LEG_B), 1);
    Pwm_GlobalEnable(p, 1);

    CHECK(((Pwm_GetEnableMask(p) & PWM_AXI_EN_BIT(PWM_AXI_HB_INDEX(2, PWM_AXI_LEG_A))) != 0U) &&
          ((Pwm_GetEnableMask(p) & PWM_AXI_EN_BIT(PWM_AXI_HB_INDEX(2, PWM_AXI_LEG_B))) != 0U) &&
          (Pwm_IsGlobalEnabled(p) != 0),
          "显式使能桥 2 两条半桥 + 总调制 -> mask = 0x%03X",
          (unsigned)Pwm_GetEnableMask(p));

    /* ---- 纯直流 ---- */
    CHECK(Pwm_CoilDc(p, 2) == PWM_AXI_OK, "CoilDc(桥2) OK");
    mode_dc = Pwm_GetLegBMode(p, 2);
    CHECK(mode_dc != PWM_AXI_MODE_NORMAL, "直流模式: 桥 2 腿 B 进入静态模式(%u)", 
          (unsigned)mode_dc);

    /* ---- 快释放 / 短路: 两种静态模式必须相反 ---- */
    CHECK(Pwm_CoilRelease(p, 3) == PWM_AXI_OK, "CoilRelease(桥3) OK");
    mode_rel = Pwm_GetLegBMode(p, 3);

    CHECK(Pwm_CoilShort(p, 3) == PWM_AXI_OK, "CoilShort(桥3) OK");
    mode_sh = Pwm_GetLegBMode(p, 3);

    CHECK((mode_rel != PWM_AXI_MODE_NORMAL) && (mode_sh != PWM_AXI_MODE_NORMAL) &&
          (mode_rel != mode_sh),
          "桥 3: 快释放腿 B 模式 = %u, 短路腿 B 模式 = %u (应不同)",
          (unsigned)mode_rel, (unsigned)mode_sh);

    CHECK(Pwm_CoilDrive(p, PWM_AXI_BRIDGE_COUNT, 100U) == PWM_AXI_ERR_ARG,
          "桥号越界被拒绝");
    CHECK(Pwm_CoilDrive(p, 0, 1001U) == PWM_AXI_ERR_RANGE, "占空比越界被拒绝");
}

/* ========================================================================== */
/*  8. 诊断输出                                                                */
/* ========================================================================== */

static void test_status_print(Pwm_HandleTypeDef *p)
{
    printf("[8] Pwm_PrintStatus(监视任务用的那一行, 这里只验证不崩)\n");

    Pwm_PrintStatus(p);
    printf("  ----\n");
    g_pass++;                               /* 走到这里就算过 */
    printf("  PASS  打印完成\n");
}

int main(void)
{
    Pwm_HandleTypeDef p;

    printf("=====================================================\n");
    printf(" pwm_axi_ip driver host test (register level)\n");
    printf("=====================================================\n");

    test_init(&p);
    test_frequency(&p);
    test_duty(&p);
    test_phase(&p);
    test_deadtime(&p);
    test_global(&p);
    test_coil(&p);
    test_status_print(&p);

    printf("-----------------------------------------------------\n");
    printf(" PASS %d, FAIL %d   (register writes: %u)\n", g_pass, g_fail,
           (unsigned)HostReg_Writes());
    printf("=====================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
