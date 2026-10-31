/*=============================================================================
 * pwm_axi_ip_bsp.c
 *-----------------------------------------------------------------------------
 * BSP layer for the pwm_axi_ip AXI4-Lite 5 x H-bridge / 20 output PWM
 * generator.
 *
 * Uses Xil_In32 / Xil_Out32 from the Vitis standalone BSP (xil_io.h), which
 * works on both the Cortex-A9 (Zynq-7000) and MicroBlaze.  No floating point
 * is used, so everything is safe to call from any context.
 *===========================================================================*/

#include "bsp/pwm/pwm.h"
#include "xil_io.h"

/* log tag: LOG_I/W/E from this file print with the [pwm] prefix */
/* 功率级的状态打印(Pwm_PrintStatus)就靠它 —— 桥"有没有输出"全靠这一行, 默认打开。 */
#define LOG_ENABLE 1   /* 排障期打开: 功率级全状态行(id/Hz/en/oc now+latched+src/duty/phase) */
#define LOG_TAG "pwm"
#include "service/monitor_module/log/log.h"

#define PWM_AXI_REG_DUTY_BASE   0x08u   /* DUTY0, the DUTYx registers are +4 apart */
#define PWM_AXI_REG_DEAD_BASE   0x1Cu   /* DEAD0, the DEADx registers are +4 apart */
#define PWM_AXI_REG_STRIDE      0x04u

/*-----------------------------------------------------------------------------
 * Low level register access
 *---------------------------------------------------------------------------*/
uint32_t Pwm_ReadReg(const Pwm_HandleTypeDef *p, uint32_t offset)
{
    return Xil_In32(p->base + offset);
}

void Pwm_WriteReg(const Pwm_HandleTypeDef *p, uint32_t offset, uint32_t value)
{
    Xil_Out32(p->base + offset, value);
}

/*-----------------------------------------------------------------------------
 * Small argument helpers (static, so they never leak into the caller)
 *---------------------------------------------------------------------------*/
static int pwm_check_bridge(int bridge)
{
    if (bridge < 0 || bridge >= (int)PWM_AXI_BRIDGE_COUNT) {
        return PWM_AXI_ERR_ARG;
    }
    return PWM_AXI_OK;
}

static int pwm_check_leg(int leg)
{
    if (leg < 0 || leg >= (int)PWM_AXI_LEG_COUNT) {
        return PWM_AXI_ERR_ARG;
    }
    return PWM_AXI_OK;
}

/* Flat half-bridge index, 0..9, equal to the bit position inside PWM_EN. */
static int pwm_check_hb_index(int index)
{
    if (index < 0 || index >= (int)PWM_AXI_HALF_BRIDGE_COUNT) {
        return PWM_AXI_ERR_ARG;
    }
    return PWM_AXI_OK;
}

static uint32_t pwm_duty_reg(int bridge)
{
    return PWM_AXI_REG_DUTY_BASE + (uint32_t)bridge * PWM_AXI_REG_STRIDE;
}

static uint32_t pwm_dead_reg(int bridge)
{
    return PWM_AXI_REG_DEAD_BASE + (uint32_t)bridge * PWM_AXI_REG_STRIDE;
}

/*-----------------------------------------------------------------------------
 * Life cycle
 *---------------------------------------------------------------------------*/
int Pwm_Init(Pwm_HandleTypeDef *p, uint32_t base, uint32_t aclk_hz)
{
    int i, leg;

    if (p == 0 || base == 0u || aclk_hz == 0u) {
        return PWM_AXI_ERR_ARG;
    }

    p->base        = base;
    p->aclk_hz     = aclk_hz;
    p->period      = 0u;
    p->enable_mask = 0u;
    for (i = 0; i < (int)PWM_AXI_BRIDGE_COUNT; i++) {
        p->duty[i]  = 0u;
        p->phase[i] = 0u;
        for (leg = 0; leg < (int)PWM_AXI_LEG_COUNT; leg++) {
            p->dead[i][leg] = 0u;
        }
    }

    if (Pwm_ReadId(p) != PWM_AXI_ID_VALUE) {
        return PWM_AXI_ERR_ID;
    }

    /* Leave the IP in its safe power-up state: modulation off, all gate
     * drivers released, invalid period (which also forces all 20 outputs
     * low).  Order matters: stop modulating before clearing the period. */
    Pwm_WriteReg(p, PWM_AXI_REG_CTRL,   0u);
    Pwm_WriteReg(p, PWM_AXI_REG_PWM_EN, 0u);
    Pwm_WriteReg(p, PWM_AXI_REG_PERIOD, 0u);

    p->period = Pwm_ReadReg(p, PWM_AXI_REG_PERIOD);
    return PWM_AXI_OK;
}

uint32_t Pwm_ReadId(const Pwm_HandleTypeDef *p)
{
    return Pwm_ReadReg(p, PWM_AXI_REG_ID);
}

/* Write/read every writable register and check the read-back.  Peripherals
 * that are not mapped in the address space (Zynq returns 0xFFFFFFFF or hangs
 * on an unmapped access) are caught here.
 *
 * The caller keeps modulation off (CTRL = 0) while this runs, so the gate
 * drivers cannot see the temporary register values. */
static int pwm_selftest_registers(Pwm_HandleTypeDef *p)
{
    uint32_t i, v;

    Pwm_WriteReg(p, PWM_AXI_REG_SCRATCH, 0xA5A55A5Au);
    if (Pwm_ReadReg(p, PWM_AXI_REG_SCRATCH) != 0xA5A55A5Au) {
        return PWM_AXI_ERR_ID;
    }
    Pwm_WriteReg(p, PWM_AXI_REG_SCRATCH, 0x12345678u);
    if (Pwm_ReadReg(p, PWM_AXI_REG_SCRATCH) != 0x12345678u) {
        return PWM_AXI_ERR_ID;
    }

    Pwm_WriteReg(p, PWM_AXI_REG_PERIOD, 0x00000400u);
    if (Pwm_ReadReg(p, PWM_AXI_REG_PERIOD) != 0x00000400u) {
        return PWM_AXI_ERR_ID;
    }

    Pwm_WriteReg(p, PWM_AXI_REG_PWM_EN, PWM_AXI_EN_MASK_ALL);
    if ((Pwm_ReadReg(p, PWM_AXI_REG_PWM_EN) & PWM_AXI_EN_MASK_ALL) !=
        PWM_AXI_EN_MASK_ALL) {
        return PWM_AXI_ERR_ID;
    }

    for (i = 0u; i < PWM_AXI_BRIDGE_COUNT; i++) {
        v = 0x100u + i;
        Pwm_WriteReg(p, pwm_duty_reg((int)i), v);
        if (Pwm_ReadReg(p, pwm_duty_reg((int)i)) != v) {
            return PWM_AXI_ERR_ID;
        }

        /* DEADx is packed: leg A in [15:0], leg B in [31:16] */
        v  = 0x0010u + i;
        v |= ((0x0020u + i) << PWM_AXI_DEAD_LEG_B_SHIFT);
        Pwm_WriteReg(p, pwm_dead_reg((int)i), v);
        if (Pwm_ReadReg(p, pwm_dead_reg((int)i)) != v) {
            return PWM_AXI_ERR_ID;
        }
    }

    /* STATUS is read only and mirrors the live hardware: PERIOD is non zero
     * at this point, so PERIOD_OK must be set. */
    if ((Pwm_ReadReg(p, PWM_AXI_REG_STATUS) & PWM_AXI_STATUS_PERIOD_OK) == 0u) {
        return PWM_AXI_ERR_ID;
    }

    return PWM_AXI_OK;
}

int Pwm_SelfTest(Pwm_HandleTypeDef *p)
{
    uint32_t save_ctrl, save_period, save_en;
    uint32_t save_duty[PWM_AXI_BRIDGE_COUNT];
    uint32_t save_dead[PWM_AXI_BRIDGE_COUNT];
    int i, result;

    if (p == 0) {
        return PWM_AXI_ERR_ARG;
    }
    if (Pwm_ReadId(p) != PWM_AXI_ID_VALUE) {
        return PWM_AXI_ERR_ID;
    }

    save_ctrl   = Pwm_ReadReg(p, PWM_AXI_REG_CTRL);
    save_period = Pwm_ReadReg(p, PWM_AXI_REG_PERIOD);
    save_en     = Pwm_ReadReg(p, PWM_AXI_REG_PWM_EN);
    for (i = 0; i < (int)PWM_AXI_BRIDGE_COUNT; i++) {
        save_duty[i] = Pwm_ReadReg(p, pwm_duty_reg(i));
        save_dead[i] = Pwm_ReadReg(p, pwm_dead_reg(i));
    }

    /* modulation off: the 20 outputs stay low for the whole test */
    Pwm_WriteReg(p, PWM_AXI_REG_CTRL, 0u);

    result = pwm_selftest_registers(p);

    /* put the application's configuration back, CTRL last so modulation only
     * resumes once every other register is consistent again */
    Pwm_WriteReg(p, PWM_AXI_REG_PERIOD, save_period);
    for (i = 0; i < (int)PWM_AXI_BRIDGE_COUNT; i++) {
        Pwm_WriteReg(p, pwm_duty_reg(i), save_duty[i]);
        Pwm_WriteReg(p, pwm_dead_reg(i), save_dead[i]);
    }
    Pwm_WriteReg(p, PWM_AXI_REG_PWM_EN, save_en);
    Pwm_WriteReg(p, PWM_AXI_REG_CTRL,   save_ctrl);

    p->period      = save_period;
    p->enable_mask = save_en & PWM_AXI_EN_MASK_ALL;

    return result;
}

/*-----------------------------------------------------------------------------
 * Global control
 *---------------------------------------------------------------------------*/
void Pwm_GlobalEnable(Pwm_HandleTypeDef *p, int enable)
{
    uint32_t ctrl;

    if (p == 0) {
        return;
    }
    ctrl = Pwm_ReadReg(p, PWM_AXI_REG_CTRL);
    if (enable) {
        ctrl |= PWM_AXI_CTRL_GLB_EN;
    } else {
        ctrl &= ~PWM_AXI_CTRL_GLB_EN;
    }
    Pwm_WriteReg(p, PWM_AXI_REG_CTRL, ctrl);
}

int Pwm_IsGlobalEnabled(const Pwm_HandleTypeDef *p)
{
    if (p == 0) {
        return 0;
    }
    return (Pwm_ReadReg(p, PWM_AXI_REG_CTRL) & PWM_AXI_CTRL_GLB_EN) ? 1 : 0;
}

/* CTRL.INV_PWM inverts all 20 output levels, which is how you drive active
 * low gate drivers without touching the pin constraints. */
void Pwm_SetInvert(Pwm_HandleTypeDef *p, int invert)
{
    uint32_t ctrl;

    if (p == 0) {
        return;
    }
    ctrl = Pwm_ReadReg(p, PWM_AXI_REG_CTRL);
    if (invert) {
        ctrl |= PWM_AXI_CTRL_INV_PWM;
    } else {
        ctrl &= ~PWM_AXI_CTRL_INV_PWM;
    }
    Pwm_WriteReg(p, PWM_AXI_REG_CTRL, ctrl);
}

/* Safe stop: modulation off first, then release the gate drivers. */
void Pwm_Stop(Pwm_HandleTypeDef *p)
{
    uint32_t en;

    if (p == 0) {
        return;
    }

    /* 关调制 + 关全部门使能, 但**保留 PWM_EN[19:10] 的腿 B 模式**(下次使能还是同一模式) */
    en  = Pwm_ReadReg(p, PWM_AXI_REG_PWM_EN);
    en &= ~PWM_AXI_EN_MASK_ALL;

    Pwm_WriteReg(p, PWM_AXI_REG_CTRL,   0u);
    Pwm_WriteReg(p, PWM_AXI_REG_PWM_EN, en);
    p->enable_mask = 0u;
}

/*-----------------------------------------------------------------------------
 * Period / frequency
 *---------------------------------------------------------------------------*/
/* PERIOD is valid from 1 to 0xFFFFFFFF.  Writing 0 makes the period invalid
 * and forces all 20 PWM outputs low, which is a safe way to park the bridge. */
void Pwm_SetPeriodCycles(Pwm_HandleTypeDef *p, uint32_t cycles)
{
    if (p == 0) {
        return;
    }
    Pwm_WriteReg(p, PWM_AXI_REG_PERIOD, cycles);
    p->period = cycles;
}

/* Returns PWM_AXI_OK, or PWM_AXI_ERR_RANGE if the requested frequency cannot
 * be produced (hz higher than the AXI clock). */
int Pwm_SetFrequencyHz(Pwm_HandleTypeDef *p, uint32_t hz)
{
    uint64_t period;

    if (p == 0 || hz == 0u || p->aclk_hz == 0u) {
        return PWM_AXI_ERR_ARG;
    }

    /* round to the nearest count so the real frequency is as close as
     * possible to the request; the result always fits in 32 bits because
     * aclk_hz itself is a 32 bit value */
    period = ((uint64_t)p->aclk_hz + (uint64_t)(hz / 2u)) / (uint64_t)hz;
    if (period < 1u) {
        return PWM_AXI_ERR_RANGE;          /* hz > aclk_hz */
    }

    Pwm_SetPeriodCycles(p, (uint32_t)period);
    return PWM_AXI_OK;
}

uint32_t Pwm_GetPeriodCycles(const Pwm_HandleTypeDef *p)
{
    if (p == 0) {
        return 0u;
    }
    return Pwm_ReadReg(p, PWM_AXI_REG_PERIOD);
}

uint32_t Pwm_GetFrequencyHz(const Pwm_HandleTypeDef *p)
{
    uint32_t period;

    if (p == 0 || p->aclk_hz == 0u) {
        return 0u;
    }
    period = Pwm_ReadReg(p, PWM_AXI_REG_PERIOD);
    if (period == 0u) {
        return 0u;
    }
    return p->aclk_hz / period;
}

/*-----------------------------------------------------------------------------
 * Duty cycle
 *
 * One DUTY value drives both legs of a bridge, which is exactly what lock
 * anti-phase (bipolar) operation needs: AH is in phase with BL and AL is in
 * phase with BH, so the diagonals conduct together.
 *
 * NOTE ON THE DEAD TIME
 *   The generated waveform puts the high-side on-time at
 *       DEAD .. DUTY-1
 *   so the high side is actually on for (DUTY - DEAD) cycles.  Dead time
 *   always eats into the effective duty cycle; that is inherent to dead-time
 *   insertion, not a bug.  Use Pwm_GetEffectiveHighPermille() to see the real
 *   value, and compensate in your control loop if you need exact duty.
 *---------------------------------------------------------------------------*/
int Pwm_SetDutyCycles(Pwm_HandleTypeDef *p, int bridge, uint32_t cycles)
{
    if (p == 0)                       return PWM_AXI_ERR_ARG;
    if (pwm_check_bridge(bridge) < 0) return PWM_AXI_ERR_ARG;

    /* The hardware clamps implicitly: DUTY > PERIOD simply keeps the high side
     * on for the whole period.  Store the requested value untouched so the
     * read-back matches what the caller asked for. */
    Pwm_WriteReg(p, pwm_duty_reg(bridge), cycles);
    p->duty[bridge] = cycles;
    return PWM_AXI_OK;
}

int Pwm_SetDutyPermille(Pwm_HandleTypeDef *p, int bridge, uint32_t permille)
{
    uint32_t period, duty;

    if (p == 0)                       return PWM_AXI_ERR_ARG;
    if (pwm_check_bridge(bridge) < 0) return PWM_AXI_ERR_ARG;
    if (permille > 1000u)             return PWM_AXI_ERR_RANGE;

    period = Pwm_ReadReg(p, PWM_AXI_REG_PERIOD);
    if (period == 0u) {
        return PWM_AXI_ERR_RANGE;      /* set a valid PERIOD first */
    }
    duty = (uint32_t)(((uint64_t)period * (uint64_t)permille) / 1000u);

    Pwm_WriteReg(p, pwm_duty_reg(bridge), duty);
    p->duty[bridge] = duty;
    return PWM_AXI_OK;
}

int Pwm_SetDutyPercent(Pwm_HandleTypeDef *p, int bridge, uint32_t percent)
{
    if (percent > 100u) {
        return PWM_AXI_ERR_RANGE;
    }
    return Pwm_SetDutyPermille(p, bridge, percent * 10u);
}

uint32_t Pwm_GetDutyCycles(const Pwm_HandleTypeDef *p, int bridge)
{
    if (p == 0 || pwm_check_bridge(bridge) < 0) {
        return 0u;
    }
    return Pwm_ReadReg(p, pwm_duty_reg(bridge));
}

uint32_t Pwm_GetDutyPermille(const Pwm_HandleTypeDef *p, int bridge)
{
    uint32_t period, duty;

    if (p == 0 || pwm_check_bridge(bridge) < 0) {
        return 0u;
    }
    period = Pwm_ReadReg(p, PWM_AXI_REG_PERIOD);
    if (period == 0u) {
        return 0u;
    }
    duty = Pwm_GetDutyCycles(p, bridge);
    if (duty > period) {
        duty = period;
    }
    return (uint32_t)(((uint64_t)duty * 1000u) / (uint64_t)period);
}

/* The duty cycle that actually appears on the pin of that leg:
 * (DUTY - DEAD_leg) / PERIOD, or 0 if DUTY <= DEAD_leg (the high side of that
 * leg never turns on).  leg is PWM_AXI_LEG_A (0) or PWM_AXI_LEG_B (1). */
uint32_t Pwm_GetEffectiveHighPermille(const Pwm_HandleTypeDef *p, int bridge, int leg)
{
    uint32_t period, duty, dead;

    if (p == 0 || pwm_check_bridge(bridge) < 0) {
        return 0u;
    }
    if (pwm_check_leg(leg) < 0) {
        return 0u;
    }
    period = Pwm_ReadReg(p, PWM_AXI_REG_PERIOD);
    if (period == 0u) {
        return 0u;
    }
    duty = Pwm_GetDutyCycles(p, bridge);
    if (duty > period) {
        duty = period;                 /* the high side is on all period long */
    }
    dead = Pwm_GetDeadtimeCycles(p, bridge, leg);
    if (duty <= dead) {
        return 0u;                     /* the high side never turns on */
    }
    return (uint32_t)(((uint64_t)(duty - dead) * 1000u) / (uint64_t)period);
}

/*-----------------------------------------------------------------------------
 * Phase offset
 *
 * The five bridges share one period counter; PHASEn only moves the point of
 * that counter bridge n compares against.  Both legs of a bridge use the same
 * shifted counter, so the complementary relationship inside a leg - and with
 * it the dead time and the no-shoot-through guarantee - is completely
 * unaffected by the phase setting.
 *---------------------------------------------------------------------------*/
int Pwm_SetPhaseCycles(Pwm_HandleTypeDef *p, int bridge, uint32_t cycles)
{
    if (p == 0)                       return PWM_AXI_ERR_ARG;
    if (pwm_check_bridge(bridge) < 0) return PWM_AXI_ERR_ARG;

    Pwm_WriteReg(p, PWM_AXI_REG_PHASE0 + (uint32_t)bridge * PWM_AXI_PHASE_STRIDE,
                 cycles);
    p->phase[bridge] = cycles;
    return PWM_AXI_OK;
}

/* Phase given as a fraction of the period, 0..1000 permille. */
int Pwm_SetPhasePermille(Pwm_HandleTypeDef *p, int bridge, uint32_t permille)
{
    uint32_t period, ph;

    if (p == 0)                       return PWM_AXI_ERR_ARG;
    if (pwm_check_bridge(bridge) < 0) return PWM_AXI_ERR_ARG;
    if (permille > 1000u)             return PWM_AXI_ERR_RANGE;

    period = Pwm_ReadReg(p, PWM_AXI_REG_PERIOD);
    ph     = (uint32_t)(((uint64_t)period * (uint64_t)permille) / 1000u);

    Pwm_WriteReg(p, PWM_AXI_REG_PHASE0 + (uint32_t)bridge * PWM_AXI_PHASE_STRIDE,
                 ph);
    p->phase[bridge] = ph;
    return PWM_AXI_OK;
}

uint32_t Pwm_GetPhaseCycles(const Pwm_HandleTypeDef *p, int bridge)
{
    if (p == 0 || pwm_check_bridge(bridge) < 0) {
        return 0u;
    }
    return Pwm_ReadReg(p, PWM_AXI_REG_PHASE0 +
                          (uint32_t)bridge * PWM_AXI_PHASE_STRIDE);
}

/* Spread `count` bridges evenly over one period.  This is the interleaving
 * case: with 5 bridges and a 4 kHz period the offsets are 0 / 2500 / 5000 /
 * 7500 / 10000 counts, i.e. 0 / 72 / 144 / 216 / 288 degrees, which staggers
 * the supply current pulses instead of adding them up. */
int Pwm_Interleave(Pwm_HandleTypeDef *p, uint32_t count)
{
    uint32_t period, i;

    if (p == 0 || count == 0u || count > PWM_AXI_BRIDGE_COUNT) {
        return PWM_AXI_ERR_ARG;
    }

    period = Pwm_ReadReg(p, PWM_AXI_REG_PERIOD);
    if (period == 0u) {
        return PWM_AXI_ERR_RANGE;      /* no valid period yet */
    }

    for (i = 0u; i < PWM_AXI_BRIDGE_COUNT; i++) {
        /* bridges at or beyond `count` are left at phase 0 */
        uint32_t ph = (i < count) ? (uint32_t)(((uint64_t)period * i) / count) : 0u;
        int rc = Pwm_SetPhaseCycles(p, (int)i, ph);
        if (rc != PWM_AXI_OK) {
            return rc;
        }
    }
    return PWM_AXI_OK;
}

/*-----------------------------------------------------------------------------
 * Leg B mode  (PWM_EN[19:10], 2 bits per bridge)
 *
 * Read-modify-write so that changing one bridge's mode never disturbs the
 * half-bridge enables in PWM_EN[9:0] or another bridge's mode.
 *---------------------------------------------------------------------------*/
int Pwm_SetLegBMode(Pwm_HandleTypeDef *p, int bridge, uint32_t mode)
{
    uint32_t en, shift;

    if (p == 0)                       return PWM_AXI_ERR_ARG;
    if (pwm_check_bridge(bridge) < 0) return PWM_AXI_ERR_ARG;
    if (mode > PWM_AXI_MODE_LEG_A_STATIC_HIGH) return PWM_AXI_ERR_RANGE;

    shift = PWM_AXI_MODE_SHIFT + 2u * (uint32_t)bridge;
    en    = Pwm_ReadReg(p, PWM_AXI_REG_PWM_EN);
    en   &= ~(PWM_AXI_MODE_FIELD_MASK << shift);
    en   |= (mode << shift);

    Pwm_WriteReg(p, PWM_AXI_REG_PWM_EN, en);
    p->enable_mask = en & 0x3FFu;
    return PWM_AXI_OK;
}

uint32_t Pwm_GetLegBMode(const Pwm_HandleTypeDef *p, int bridge)
{
    if (p == 0 || pwm_check_bridge(bridge) < 0) {
        return 0u;
    }
    return (Pwm_ReadReg(p, PWM_AXI_REG_PWM_EN) >>
            (PWM_AXI_MODE_SHIFT + 2u * (uint32_t)bridge)) & PWM_AXI_MODE_FIELD_MASK;
}

/*-----------------------------------------------------------------------------
 * Coil / eddy current brake helpers
 *
 * The load is an inductive coil, so what matters is the CURRENT waveform, not
 * the voltage: eddy current braking force follows the coil MMF, which is
 * proportional to the current, so any current ripple shows up directly as
 * force ripple (vibration, noise).  In the diagonal (motor) mode the coil sits
 * between +Vbus and -Vbus, giving a ripple of about 2*Vbus*d*(1-d)*T/L; the
 * static modes put it between +Vbus and 0 V, which is roughly half of that at
 * the same average current and switches only one leg.
 *
 * Leg A is the PWM leg and leg B is the static "direction" leg, which is
 * exactly the classic coil / solenoid drive scheme.
 *---------------------------------------------------------------------------*/

/* Low-ripple hold: chop leg A, leg B static low (slow decay). */
int Pwm_CoilDrive(Pwm_HandleTypeDef *p, int bridge, uint32_t duty_permille)
{
    int rc;

    rc = Pwm_SetLegBMode(p, bridge, PWM_AXI_MODE_STATIC_LOW);
    if (rc != PWM_AXI_OK) {
        return rc;
    }
    return Pwm_SetDutyPermille(p, bridge, duty_permille);
}

/* Clean DC: no switching at all, so zero ripple and zero switching loss.
 * This is the best state for a brake that only needs a fixed field. */
int Pwm_CoilDc(Pwm_HandleTypeDef *p, int bridge)
{
    uint32_t period;
    int      rc;

    if (p == 0)                       return PWM_AXI_ERR_ARG;
    if (pwm_check_bridge(bridge) < 0) return PWM_AXI_ERR_ARG;

    period = Pwm_ReadReg(p, PWM_AXI_REG_PERIOD);
    if (period == 0u) {
        return PWM_AXI_ERR_RANGE;      /* no valid period yet */
    }

    rc = Pwm_SetLegBMode(p, bridge, PWM_AXI_MODE_STATIC_LOW);
    if (rc != PWM_AXI_OK) return rc;
    rc = Pwm_SetDeadtimeCycles(p, bridge, PWM_AXI_LEG_A, 0u);
    if (rc != PWM_AXI_OK) return rc;
    return Pwm_SetDutyCycles(p, bridge, period);
}

/* Fast decay: leg A static low, leg B static high -> the coil sees -Vbus.
 *
 * !! DO NOT LEAVE THE COIL IN THIS STATE !!
 *   -Vbus is a FORCING voltage, not a clamp.  Left applied it will drive the
 *   current all the way to -Vbus/R (a full reverse current), which heats the
 *   coil for nothing and puts a reverse field on the workpiece.  Measured on a
 *   1 mH / 2 ohm coil at 24 V: releasing from 5.1 A, the current reaches zero
 *   in about 178 us, but if you keep the state applied it just keeps going
 *   negative.
 *
 *   Correct sequence: call this, watch your current sense, and call
 *   Pwm_CoilShort() (or Pwm_GlobalEnable(0) to coast) the moment the current
 *   reaches zero.  Both of those hold the current at zero without heating.
 *   The IP cannot do this for you because it has no current feedback. */
int Pwm_CoilRelease(Pwm_HandleTypeDef *p, int bridge)
{
    int rc;

    rc = Pwm_SetLegBMode(p, bridge, PWM_AXI_MODE_STATIC_HIGH);
    if (rc != PWM_AXI_OK) return rc;
    rc = Pwm_SetDeadtimeCycles(p, bridge, PWM_AXI_LEG_A, 0u);
    if (rc != PWM_AXI_OK) return rc;
    return Pwm_SetDutyCycles(p, bridge, 0u);
}

/* Active short: both terminals at GND, current circulates through the two low
 * side switches.  Gentle, controlled decay - the coil only sees its own I*R. */
int Pwm_CoilShort(Pwm_HandleTypeDef *p, int bridge)
{
    int rc;

    rc = Pwm_SetLegBMode(p, bridge, PWM_AXI_MODE_STATIC_LOW);
    if (rc != PWM_AXI_OK) return rc;
    rc = Pwm_SetDeadtimeCycles(p, bridge, PWM_AXI_LEG_A, 0u);
    if (rc != PWM_AXI_OK) return rc;
    return Pwm_SetDutyCycles(p, bridge, 0u);
}

/*-----------------------------------------------------------------------------
 * Dead time
 *
 * DEADx is packed: bits[15:0] = leg A dead time, bits[31:16] = leg B dead
 * time.  Every setter here is a read-modify-write so that changing one leg
 * never clobbers the other.  One leg dead time is 16 bits wide (0..0xFFFF),
 * so larger values are rejected with PWM_AXI_ERR_RANGE.
 *---------------------------------------------------------------------------*/
int Pwm_SetDeadtimeCycles(Pwm_HandleTypeDef *p, int bridge, int leg, uint32_t cycles)
{
    uint32_t reg, offset;

    if (p == 0)                       return PWM_AXI_ERR_ARG;
    if (pwm_check_bridge(bridge) < 0) return PWM_AXI_ERR_ARG;
    if (pwm_check_leg(leg) < 0)       return PWM_AXI_ERR_ARG;
    if (cycles > PWM_AXI_DEAD_MAX)    return PWM_AXI_ERR_RANGE;

    offset = pwm_dead_reg(bridge);
    reg    = Pwm_ReadReg(p, offset);

    if (leg == PWM_AXI_LEG_A) {
        reg = (reg & PWM_AXI_DEAD_LEG_B_MASK) |
              (cycles & PWM_AXI_DEAD_LEG_A_MASK);
    } else {
        reg = (reg & PWM_AXI_DEAD_LEG_A_MASK) |
              ((cycles << PWM_AXI_DEAD_LEG_B_SHIFT) & PWM_AXI_DEAD_LEG_B_MASK);
    }

    Pwm_WriteReg(p, offset, reg);
    p->dead[bridge][leg] = cycles;
    return PWM_AXI_OK;
}

/* ns -> aclk cycles, rounded to the nearest cycle.  One count is 20 ns with
 * the 50 MHz PS clock, so 500 ns becomes 25 counts. */
int Pwm_SetDeadtimeNs(Pwm_HandleTypeDef *p, int bridge, int leg, uint32_t ns)
{
    uint64_t cycles;

    if (p == 0)                       return PWM_AXI_ERR_ARG;
    if (pwm_check_bridge(bridge) < 0) return PWM_AXI_ERR_ARG;
    if (pwm_check_leg(leg) < 0)       return PWM_AXI_ERR_ARG;
    if (p->aclk_hz == 0u)             return PWM_AXI_ERR_ARG;

    cycles = ((uint64_t)p->aclk_hz * (uint64_t)ns + 500000000ull) / 1000000000ull;
    if (cycles > (uint64_t)PWM_AXI_DEAD_MAX) {
        return PWM_AXI_ERR_RANGE;      /* does not fit in the 16 bit field */
    }
    return Pwm_SetDeadtimeCycles(p, bridge, leg, (uint32_t)cycles);
}

/* Convenience: the same dead time on both legs of a bridge.  With DTA = DTB
 * the AH and BL edges are exactly aligned. */
int Pwm_SetBridgeDeadtime(Pwm_HandleTypeDef *p, int bridge, uint32_t cycles)
{
    int result;

    if (p == 0)                       return PWM_AXI_ERR_ARG;
    if (pwm_check_bridge(bridge) < 0) return PWM_AXI_ERR_ARG;

    result = Pwm_SetDeadtimeCycles(p, bridge, PWM_AXI_LEG_A, cycles);
    if (result != PWM_AXI_OK) {
        return result;
    }
    return Pwm_SetDeadtimeCycles(p, bridge, PWM_AXI_LEG_B, cycles);
}

uint32_t Pwm_GetDeadtimeCycles(const Pwm_HandleTypeDef *p, int bridge, int leg)
{
    uint32_t reg;

    if (p == 0 || pwm_check_bridge(bridge) < 0 || pwm_check_leg(leg) < 0) {
        return 0u;
    }
    reg = Pwm_ReadReg(p, pwm_dead_reg(bridge));
    if (leg == PWM_AXI_LEG_A) {
        return reg & PWM_AXI_DEAD_LEG_A_MASK;
    }
    return (reg & PWM_AXI_DEAD_LEG_B_MASK) >> PWM_AXI_DEAD_LEG_B_SHIFT;
}

uint32_t Pwm_GetDeadtimeNs(const Pwm_HandleTypeDef *p, int bridge, int leg)
{
    uint32_t cycles;

    if (p == 0 || p->aclk_hz == 0u) {
        return 0u;
    }
    cycles = Pwm_GetDeadtimeCycles(p, bridge, leg);
    return (uint32_t)(((uint64_t)cycles * 1000000000ull) / (uint64_t)p->aclk_hz);
}

/*-----------------------------------------------------------------------------
 * Half-bridge gate-driver enables
 *
 * The flat index is 0..9 and equals 2*bridge + leg, i.e. the bit position in
 * PWM_EN: bit(2*i) = bridge i leg A, bit(2*i+1) = bridge i leg B.  gate_en
 * only enables the gate drivers, it does not change the PWM waveforms.
 *---------------------------------------------------------------------------*/
void Pwm_SetHalfBridgeEnable(Pwm_HandleTypeDef *p, int index, int enable)
{
    uint32_t mask;

    if (p == 0 || pwm_check_hb_index(index) < 0) {
        return;
    }

    /*
     * ⚠ 只改 [9:0], **其余位必须原样写回去**: PWM_EN[19:10] 存的是每桥的腿 B 模式。
     *   早先这里有一句 `mask &= PWM_AXI_EN_MASK_ALL`, 等于把模式位一起清零 ——
     *   于是"启动制动(使能输出)之后又变回对角驱动, 再动一次占空比才恢复 mode 3"
     *   (2026-10-27 实机踩到)。
     */
    mask = Pwm_ReadReg(p, PWM_AXI_REG_PWM_EN);
    if (enable) {
        mask |= PWM_AXI_EN_BIT(index);
    } else {
        mask &= ~PWM_AXI_EN_BIT(index);
    }

    Pwm_WriteReg(p, PWM_AXI_REG_PWM_EN, mask);
    p->enable_mask = mask & PWM_AXI_EN_MASK_ALL;
}

void Pwm_SetLegEnable(Pwm_HandleTypeDef *p, int bridge, int leg, int enable)
{
    if (p == 0 || pwm_check_bridge(bridge) < 0 || pwm_check_leg(leg) < 0) {
        return;
    }
    Pwm_SetHalfBridgeEnable(p, (int)PWM_AXI_HB_INDEX(bridge, leg), enable);
}

/* Both legs of one bridge, which is what you normally want: a bridge is only
 * operational when both of its gate drivers are enabled. */
void Pwm_SetBridgeEnable(Pwm_HandleTypeDef *p, int bridge, int enable)
{
    uint32_t mask, bits;

    if (p == 0 || pwm_check_bridge(bridge) < 0) {
        return;
    }

    bits = PWM_AXI_EN_BIT(PWM_AXI_HB_INDEX(bridge, PWM_AXI_LEG_A)) |
           PWM_AXI_EN_BIT(PWM_AXI_HB_INDEX(bridge, PWM_AXI_LEG_B));

    mask = Pwm_ReadReg(p, PWM_AXI_REG_PWM_EN);
    if (enable) {
        mask |= bits;
    } else {
        mask &= ~bits;
    }
    /* 同样只改 [9:0], 保留 [19:10] 的腿 B 模式(见 Pwm_SetHalfBridgeEnable 的说明) */

    Pwm_WriteReg(p, PWM_AXI_REG_PWM_EN, mask);
    p->enable_mask = mask & PWM_AXI_EN_MASK_ALL;
}

void Pwm_SetAllHalfBridges(Pwm_HandleTypeDef *p, int enable)
{
    uint32_t mask;

    if (p == 0) {
        return;
    }

    /* 同前: 只覆盖 [9:0], 上面的腿 B 模式位原样保留 */
    mask = Pwm_ReadReg(p, PWM_AXI_REG_PWM_EN);
    mask &= ~PWM_AXI_EN_MASK_ALL;
    if (enable) {
        mask |= PWM_AXI_EN_MASK_ALL;
    }

    Pwm_WriteReg(p, PWM_AXI_REG_PWM_EN, mask);
    p->enable_mask = mask & PWM_AXI_EN_MASK_ALL;
}

uint32_t Pwm_GetEnableMask(const Pwm_HandleTypeDef *p)
{
    if (p == 0) {
        return 0u;
    }
    return Pwm_ReadReg(p, PWM_AXI_REG_PWM_EN) & PWM_AXI_EN_MASK_ALL;
}

/*-----------------------------------------------------------------------------
 * Status
 *---------------------------------------------------------------------------*/
uint32_t Pwm_GetStatus(const Pwm_HandleTypeDef *p)
{
    if (p == 0) {
        return 0u;
    }
    return Pwm_ReadReg(p, PWM_AXI_REG_STATUS);
}

/* Live period counter, low 16 bits.  It runs 0..PERIOD-1 and is shared by all
 * 5 bridges, so it can be used to change duty values at a known point in the
 * PWM period. */
uint32_t Pwm_GetCounter(const Pwm_HandleTypeDef *p)
{
    return Pwm_GetStatus(p) >> PWM_AXI_STATUS_CNT_SHIFT;
}

/*===========================================================================*/
/**
 * @brief   Print a one line status report (console / monitor task).
 *
 * Added when the driver was ported into app923: it turns the shadow registers
 * into a single log line so the whole power stage can be checked at a glance.
 *
 * @param   p : Initialised handle.
 */
void Pwm_PrintStatus(const Pwm_HandleTypeDef *p)
{
    uint32_t mask;

    if (p == NULL) {
        return;
    }

    mask = Pwm_GetEnableMask(p);

    /* ⚠ 这一句是给"日志关掉时"用的: LOG_ENABLE 0 会把下面那行 LOG_I 整个变成空操作,
     * 到时候 mask 就只有赋值没有使用, -Wall 会报 "set but not used"。
     * 显式 (void) 一次, 两种日志开关下都干净(不改变任何行为)。 */
    (void)mask;

    /* 一条 LOG_I 打完整行: 桥数是编译期常量(5), 所以不需要分段打印 —— 分段
     * 打印会绕过日志锁, 和其它任务的输出搅在一起。 */
    LOG_I("id 0x%08X | %s | %u Hz (period %u) | en 0x%03X | oc now=%u latched=%u src=0x%02X | duty %u %u %u %u %u"
          " (permille) phase %u %u %u %u %u\r\n",
          (unsigned)Pwm_ReadId(p),
          (Pwm_IsGlobalEnabled(p) != 0) ? "ON " : "OFF",
          (unsigned)Pwm_GetFrequencyHz(p),
          (unsigned)Pwm_GetPeriodCycles(p),
          (unsigned)mask,
          (unsigned)Pwm_OcNow(p), (unsigned)Pwm_OcLatched(p),
          (unsigned)Pwm_OcSource(p),
          (unsigned)Pwm_GetDutyPermille(p, 0), (unsigned)Pwm_GetDutyPermille(p, 1),
          (unsigned)Pwm_GetDutyPermille(p, 2), (unsigned)Pwm_GetDutyPermille(p, 3),
          (unsigned)Pwm_GetDutyPermille(p, 4),
          (unsigned)Pwm_GetPhaseCycles(p, 0), (unsigned)Pwm_GetPhaseCycles(p, 1),
          (unsigned)Pwm_GetPhaseCycles(p, 2), (unsigned)Pwm_GetPhaseCycles(p, 3),
          (unsigned)Pwm_GetPhaseCycles(p, 4));
}
/*===========================================================================*/
/**
 * @brief   Is an overcurrent input low right now?
 * @return 1 = yes (at least one bridge is being held off by the live level).
 */
int Pwm_OcNow(const Pwm_HandleTypeDef *p)
{
    return ((Pwm_GetStatus(p) & PWM_AXI_STATUS_OC_NOW) != 0u) ? 1 : 0;
}

/**
 * @brief   Has an overcurrent happened since the last acknowledge?
 * @return 1 = yes (the sticky latch is set, the affected bridge(s) stay off).
 */
int Pwm_OcLatched(const Pwm_HandleTypeDef *p)
{
    return ((Pwm_GetStatus(p) & PWM_AXI_STATUS_OC_LATCHED) != 0u) ? 1 : 0;
}

/**
 * @brief   Which bridges have tripped (sticky).
 * @return Bit i set = bridge i tripped at least once.
 */
uint32_t Pwm_OcSource(const Pwm_HandleTypeDef *p)
{
    return (Pwm_GetStatus(p) & PWM_AXI_STATUS_OC_SRC_MASK) >>
           PWM_AXI_STATUS_OC_SRC_SHIFT;
}

/**
 * @brief   Acknowledge an overcurrent fault and restore normal operation.
 *
 * The RTL clears OC_LATCHED / OC_SRC on a CTRL.GLB_EN 0 -> 1 edge, so this
 * writes GLB_EN = 0 and then GLB_EN = 1 again (INV_PWM is preserved).  The
 * half bridge enable mask is untouched.
 *
 * @return PWM_AXI_OK, or PWM_AXI_ERR_ARG.
 *
 * @note    This does NOT restart a bridge whose oc_n pin is still low: that is
 *          the live term, which only the hardware can clear.  Check
 *          Pwm_OcNow() before blaming the software.
 */
int Pwm_AcknowledgeFault(Pwm_HandleTypeDef *p)
{
    uint32_t ctrl;

    if (p == 0) {
        return PWM_AXI_ERR_ARG;
    }

    ctrl = Pwm_ReadReg(p, PWM_AXI_REG_CTRL);

    Pwm_WriteReg(p, PWM_AXI_REG_CTRL, ctrl & ~PWM_AXI_CTRL_GLB_EN);   /* 0 */
    Pwm_WriteReg(p, PWM_AXI_REG_CTRL, ctrl | PWM_AXI_CTRL_GLB_EN);    /* 1 */

    return PWM_AXI_OK;
}
/**
 * @brief   Format an OC source mask as a readable bridge list ("1,3" / "-").
 */
void Pwm_FormatOcSource(uint32_t src, char *dst, uint16_t size)
{
    uint16_t pos = 0U;
    uint32_t i;
    uint8_t  first = 1U;

    if ((dst == NULL) || (size == 0U)) {
        return;
    }

    dst[0] = '\0';

    if (src == 0U) {
        dst[0] = '-';
        if (size > 1U) {
            dst[1] = '\0';
        }
        return;
    }

    for (i = 0U; i < PWM_AXI_BRIDGE_COUNT; i++) {
        if (((src >> i) & 1U) == 0U) {
            continue;
        }
        if ((uint16_t)(pos + 3U) >= size) {
            break;
        }
        if (first == 0U) {
            dst[pos++] = ',';
        }
        dst[pos++] = (char)('0' + i);
        first = 0U;
    }

    dst[pos] = '\0';
}
/*===========================================================================*/
/** Minimum time between two alarm lines (ms) - console sanity, not protection.
 *  Kept below the caller's period so every poll during a fault prints a line. */
#define PWM_ALARM_MIN_GAP_MS   500u

void Pwm_Alarm_Poll(Pwm_HandleTypeDef *p, int auto_ack)
{
    static uint8_t  faulted_seen;
    static uint32_t last_ms;

    uint32_t status;
    uint8_t  faulted;
    uint32_t now_ms;
    char     list[16];

    if (p == 0) {
        return;
    }

    status  = Pwm_GetStatus(p);
    now_ms  = Log_UptimeMs();

    /* ------------------------------------------------------------------ */
    /* 自动恢复: 引脚已经回到高电平, 但粘滞锁存还在 -> 软件代为确认        */
    /* (GLB_EN 0->1), 于是桥自动恢复、警告自动消失.                        */
    /* 只在引脚**确实已经为高**时做, 所以真实过流不会被掩盖: 引脚还低时     */
    /* 实时项 ~oc_n 继续把桥关着, 这里不会确认。                           */
    /* ------------------------------------------------------------------ */
    if ((auto_ack != 0) &&
        ((status & PWM_AXI_STATUS_OC_NOW) == 0u) &&
        ((status & PWM_AXI_STATUS_OC_LATCHED) != 0u)) {
        (void)Pwm_AcknowledgeFault(p);
        status = Pwm_GetStatus(p);

        if (faulted_seen != 0u) {
            LOG_I("overcurrent auto-cleared: oc_n is HIGH again -> latch acknowledged, "
                  "bridge(s) re-enabled (src=0x%02X status=0x%08X)\r\n",
                  (unsigned)Pwm_OcSource(p), (unsigned)status);
            faulted_seen = 0u;
            last_ms      = now_ms;
        }
    }

    faulted = ((status & (PWM_AXI_STATUS_OC_NOW | PWM_AXI_STATUS_OC_LATCHED)) != 0u) ? 1u : 0u;

    /* ------------------------------------------------------------------ */
    /* 故障中: 每次调用都打一行(持续报警, 直到故障消失)                    */
    /* ------------------------------------------------------------------ */
    if (faulted != 0u) {
        if ((faulted_seen != 0u) && ((uint32_t)(now_ms - last_ms) < PWM_ALARM_MIN_GAP_MS)) {
            return;                         /* 只是防止调用者轮询太快时刷屏 */
        }

        Pwm_FormatOcSource(Pwm_OcSource(p), list, (uint16_t)sizeof(list));

        LOG_W(">>> OVERCURRENT: bridge(s) %s <<< oc_n low now=%u latched=%u src=0x%02X "
              "status=0x%08X | that bridge's outputs are held OFF until the next "
              "CTRL.GLB_EN 0->1 edge (the brake restart does it)\r\n",
              list, (unsigned)Pwm_OcNow(p), (unsigned)Pwm_OcLatched(p),
              (unsigned)Pwm_OcSource(p), (unsigned)status);

        last_ms      = now_ms;
        faulted_seen = 1u;
        return;
    }

    /* ------------------------------------------------------------------ */
    /* 故障消失: 只在 "刚才还在故障" 时打一条收尾行                          */
    /* ------------------------------------------------------------------ */
    if (faulted_seen != 0u) {
        LOG_I("overcurrent cleared: oc_n low now=%u latched=%u src=0x%02X status=0x%08X\r\n",
              (unsigned)Pwm_OcNow(p), (unsigned)Pwm_OcLatched(p),
              (unsigned)Pwm_OcSource(p), (unsigned)status);

        last_ms      = now_ms;
        faulted_seen = 0u;
    }
}