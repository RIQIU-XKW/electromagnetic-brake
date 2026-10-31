/*=============================================================================
 * pwm_axi_ip_bsp.h
 *-----------------------------------------------------------------------------
 * Register map and a small BSP layer for the pwm_axi_ip AXI4-Lite peripheral
 * (5 x H-bridge / 20 x complementary PWM generator for Zynq-7020).
 *
 * The register map matches hdl/pwm_axi_ip.v exactly.
 *
 * GEOMETRY
 *   There are 5 bridges (H-bridges) and each bridge has two half bridges,
 *   leg A and leg B.  With bridge index i = 0..4:
 *
 *       pwm_out[4*i + 0] = AH   leg A high side switch
 *       pwm_out[4*i + 1] = AL   leg A low side  switch
 *       pwm_out[4*i + 2] = BH   leg B high side switch
 *       pwm_out[4*i + 3] = BL   leg B low side  switch
 *
 *       gate_en[2*i + 0] = leg A gate-driver enable
 *       gate_en[2*i + 1] = leg B gate-driver enable
 *
 *   AH/AL are one complementary pair (leg A) and BH/BL are the other (leg B).
 *   The bridge is driven in lock anti-phase (bipolar) mode, so the DIAGONAL
 *   switches conduct together:  AH is in phase with BL (forward diagonal,
 *   AH+BL on) and AL is in phase with BH (reverse diagonal, AL+BH on).
 *   Shoot-through can only happen inside one leg and is impossible by
 *   construction.
 *
 * WAVEFORM AND DEAD TIME
 *   The period counter cnt runs 0 .. PERIOD-1 and is shared by all 5 bridges,
 *   so the bridges stay phase locked.  For bridge i with DUTY = D, leg A dead
 *   time = DTA and leg B dead time = DTB:
 *
 *       AH on for cnt in [DTA, D)        AL on for cnt in [D + DTA, PERIOD)
 *       BL on for cnt in [DTB, D)        BH on for cnt in [D + DTB, PERIOD)
 *
 *   The high-side on-time is therefore (D - DT) cycles and the low-side
 *   on-time is (PERIOD - D - DT) cycles: dead time always eats into the
 *   effective duty cycle, which is inherent to dead-time insertion, not a bug.
 *
 *       effective high-side duty = (D - DTleg) / PERIOD
 *
 *   Pwm_GetEffectiveHighPermille() reports that value.  With DTA = DTB the AH
 *   and BL edges are exactly aligned.  All the edge cases are safe: DTA = 0
 *   gives a fully complementary pair with no dead time, D <= DT means that
 *   leg's high side never turns on, D + DT >= PERIOD means that leg's low side
 *   never turns on, and D > PERIOD keeps the high side on for the whole period.
 *
 *   All 20 outputs are registered, so they are glitch free and lag the counter
 *   by exactly one s00_axi_aclk cycle.
 *
 * Typical use (Vitis / standalone BSP), s00_axi_aclk = FCLK_CLK0 = 50 MHz,
 * so one count is 20 ns:
 *
 *     #include "pwm_axi_ip_bsp.h"
 *
 *     static pwm_axi_t pwm;
 *     uint32_t d;
 *
 *     Pwm_Init(&pwm, XPAR_PWM_AXI_IP_0_S00_AXI_BASEADDR, 50000000u);
 *     Pwm_GlobalEnable(&pwm, 0);              // modulation off while configuring
 *     Pwm_SetFrequencyHz(&pwm, 20000u);       // 20 kHz, PERIOD = 2500 @ 50 MHz
 *     Pwm_SetBridgeDeadtime(&pwm, 0, 25u);    // 500 ns = 25 counts, both legs
 *     Pwm_SetDutyPermille(&pwm, 0, 300u);     // 30.0 % on bridge 0, DUTY = 750
 *     Pwm_SetAllHalfBridges(&pwm, 1);         // enable all 10 gate drivers
 *     Pwm_GlobalEnable(&pwm, 1);              // start modulating
 *
 *     // bridge 0 really puts (750 - 25) / 2500 = 29.0 % on the high side,
 *     // because the 25 counts of dead time are subtracted from the DUTY value
 *
 *     // ramp the duty later (change it whenever your control loop wants to)
 *     for (d = 0u; d <= 1000u; d += 5u) {
 *         Pwm_SetDutyPermille(&pwm, 0, d);
 *     }
 *     Pwm_SetDutyPermille(&pwm, 0, 300u);
 *===========================================================================*/

#ifndef PWM_AXI_IP_BSP_H
#define PWM_AXI_IP_BSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------------------------------
 * Register offsets (64 byte window, base address comes from the block design,
 * typically 0x43C10000)
 *---------------------------------------------------------------------------*/
#define PWM_AXI_REG_CTRL        0x00u   /* RW  [0] GLB_EN  [1] INV_PWM           */
#define PWM_AXI_REG_PERIOD      0x04u   /* RW  PWM period in aclk cycles, 1..max */
#define PWM_AXI_REG_DUTY0       0x08u   /* RW  bridge 0 compare value            */
#define PWM_AXI_REG_DUTY1       0x0Cu   /* RW  bridge 1 compare value            */
#define PWM_AXI_REG_DUTY2       0x10u   /* RW  bridge 2 compare value            */
#define PWM_AXI_REG_DUTY3       0x14u   /* RW  bridge 3 compare value            */
#define PWM_AXI_REG_DUTY4       0x18u   /* RW  bridge 4 compare value            */
#define PWM_AXI_REG_DEAD0       0x1Cu   /* RW  bridge 0 dead times, packed       */
#define PWM_AXI_REG_DEAD1       0x20u   /* RW  bridge 1 dead times, packed       */
#define PWM_AXI_REG_DEAD2       0x24u   /* RW  bridge 2 dead times, packed       */
#define PWM_AXI_REG_DEAD3       0x28u   /* RW  bridge 3 dead times, packed       */
#define PWM_AXI_REG_DEAD4       0x2Cu   /* RW  bridge 4 dead times, packed       */
#define PWM_AXI_REG_PWM_EN      0x30u   /* RW  [9:0] half-bridge enables         */
#define PWM_AXI_REG_SCRATCH     0x34u   /* RW  scratch / AXI read-back test      */
#define PWM_AXI_REG_ID          0x38u   /* RO  0x50574D31 ("PWM1")               */
#define PWM_AXI_REG_STATUS      0x3Cu   /* RO  status / live counter             */
/* Phase offsets live in a second bank of registers, which is why the address
 * window is 128 bytes (C_S00_AXI_ADDR_WIDTH = 7) and not 64. */
#define PWM_AXI_REG_PHASE0      0x40u   /* RW  bridge 0 phase offset in cycles   */
#define PWM_AXI_REG_PHASE1      0x44u   /* RW  bridge 1 phase offset             */
#define PWM_AXI_REG_PHASE2      0x48u   /* RW  bridge 2 phase offset             */
#define PWM_AXI_REG_PHASE3      0x4Cu   /* RW  bridge 3 phase offset             */
#define PWM_AXI_REG_PHASE4      0x50u   /* RW  bridge 4 phase offset             */
#define PWM_AXI_PHASE_STRIDE    0x04u   /* distance between PHASEn registers     */

/* CTRL bits (register 0x00).  Bits[31:2] are reserved. */
#define PWM_AXI_CTRL_GLB_EN     (1u << 0)   /* global modulation enable          */
#define PWM_AXI_CTRL_INV_PWM    (1u << 1)   /* invert all 20 output levels       */

/* DEADx register packing: two 16-bit dead times in one 32-bit register */
#define PWM_AXI_DEAD_LEG_A_MASK 0x0000FFFFu
#define PWM_AXI_DEAD_LEG_B_MASK 0xFFFF0000u

/* ---------------------------------------------------------------------------
 * Leg B mode, carried in PWM_EN[19:10] (2 bits per bridge).  No extra register
 * was needed for this: the field used to be reserved.
 *
 *   NORMAL      diagonal drive, leg B uses DUTY like leg A.  V = Vbus*(2d-1).
 *               This is the motor / H-bridge behaviour and the reset default.
 *   STATIC_LOW  BL on, BH off.  With leg A PWMs the load sees +Vbus during the
 *               on-time and 0 V during the off-time, freewheeling through the
 *               two LOW side switches.  That is SLOW DECAY: the low-ripple
 *               hold current a coil wants.
 *   STATIC_HIGH BH on, BL off.  With leg A static low the load sees -Vbus,
 *               which forces a coil current down fast (fast decay).
 *
 * A static leg ignores its dead time (the IP forces it to zero), so it stays
 * genuinely static instead of dropping out for a few cycles every period.
 * -------------------------------------------------------------------------*/
#define PWM_AXI_MODE_NORMAL       0u    /* diagonal (default)                */
#define PWM_AXI_MODE_STATIC_LOW   1u    /* BL on, BH off                     */
#define PWM_AXI_MODE_STATIC_HIGH  2u    /* BH on, BL off                     */
#define PWM_AXI_MODE_SHIFT        10u   /* PWM_EN[19:10]                     */
#define PWM_AXI_MODE_FIELD_MASK   3u
#define PWM_AXI_DEAD_LEG_B_SHIFT 16u
#define PWM_AXI_DEAD_MAX        0xFFFFu     /* one leg dead time is 16 bits      */

/* STATUS bits (register 0x3C) */
#define PWM_AXI_STATUS_GLB_EN   (1u << 0)   /* mirror of CTRL.GLB_EN             */
#define PWM_AXI_STATUS_PERIOD_OK (1u << 1)  /* PERIOD != 0                       */
#define PWM_AXI_STATUS_CNT_ZERO (1u << 2)   /* live counter is at 0              */
#define PWM_AXI_STATUS_ANY_EN   (1u << 3)   /* OR of PWM_EN[9:0]                 */
#define PWM_AXI_STATUS_CNT_SHIFT 16u        /* [31:16] = live counter low 16 bit */

/* IP identification */
#define PWM_AXI_ID_VALUE        0x50574D31u

/* Geometry */
#define PWM_AXI_BRIDGE_COUNT      5u    /* H-bridges                         */
#define PWM_AXI_LEG_COUNT         2u    /* legs (half bridges) per bridge     */
#define PWM_AXI_HALF_BRIDGE_COUNT 10u   /* half bridges / gate_en outputs     */
#define PWM_AXI_PWM_OUTPUT_COUNT  20u   /* pwm_out outputs                    */

/* Leg constants */
#define PWM_AXI_LEG_A           0       /* half bridge with AH (high) / AL    */
#define PWM_AXI_LEG_B           1       /* half bridge with BH (high) / BL    */

/* ---------------------------------------------------------------------------
 * Index mapping.
 *
 * pwm_out bit index of each switch of bridge i (i = 0..4):
 *   AH = 4*i+0  leg A high side   AL = 4*i+1  leg A low side
 *   BH = 4*i+2  leg B high side   BL = 4*i+3  leg B low side
 *
 * PWM_EN / gate_en bit index of each half bridge:
 *   index = PWM_AXI_HB_INDEX(bridge, leg) = 2*bridge + leg
 *---------------------------------------------------------------------------*/
#define PWM_AXI_OUT_AH(i)       (4u * (uint32_t)(i) + 0u)
#define PWM_AXI_OUT_AL(i)       (4u * (uint32_t)(i) + 1u)
#define PWM_AXI_OUT_BH(i)       (4u * (uint32_t)(i) + 2u)
#define PWM_AXI_OUT_BL(i)       (4u * (uint32_t)(i) + 3u)
#define PWM_AXI_HB_INDEX(bridge, leg) \
                                (2u * (uint32_t)(bridge) + (uint32_t)(leg))
#define PWM_AXI_EN_BIT(index)   (1u << (uint32_t)(index))
#define PWM_AXI_EN_MASK_ALL     0x3FFu  /* all 10 half bridges                */

/* Return codes */
#define PWM_AXI_OK              0
#define PWM_AXI_ERR_ARG        (-1)
#define PWM_AXI_ERR_RANGE      (-2)
#define PWM_AXI_ERR_ID         (-3)

/*-----------------------------------------------------------------------------
 * Handle
 *
 * The shadow copies mirror what was last written, so the getters work without
 * touching the AXI bus and the periodic setters can rescale a value when the
 * period changes.  Pwm_Get* functions still read the hardware.
 *---------------------------------------------------------------------------*/
typedef struct {
    uint32_t base;      /* AXI base address of the peripheral                   */
    uint32_t aclk_hz;   /* frequency of s00_axi_aclk (FCLK_CLK0 = 50 MHz here)  */
    uint32_t period;    /* shadow copy of the PERIOD register                   */
    uint32_t duty[PWM_AXI_BRIDGE_COUNT];
                        /* shadow copies of the DUTY0..DUTY4 registers         */
    uint32_t dead[PWM_AXI_BRIDGE_COUNT][PWM_AXI_LEG_COUNT];
                        /* shadow copies of the 10 dead times (per leg)         */
    uint32_t enable_mask;
                        /* shadow copy of PWM_EN[9:0]                           */
    uint32_t phase[PWM_AXI_BRIDGE_COUNT];
                        /* shadow copies of the PHASE0..PHASE4 registers        */
} pwm_axi_t;

/*-----------------------------------------------------------------------------
 * Life cycle / self test
 *---------------------------------------------------------------------------*/
int      Pwm_Init(pwm_axi_t *p, uint32_t base, uint32_t aclk_hz);
int      Pwm_SelfTest(pwm_axi_t *p);
uint32_t Pwm_ReadId(const pwm_axi_t *p);

/*-----------------------------------------------------------------------------
 * Global control
 *---------------------------------------------------------------------------*/
void Pwm_GlobalEnable(pwm_axi_t *p, int enable);
int  Pwm_IsGlobalEnabled(const pwm_axi_t *p);
void Pwm_SetInvert(pwm_axi_t *p, int invert);
void Pwm_Stop(pwm_axi_t *p);            /* CTRL = 0 and all gate enables cleared */

/*-----------------------------------------------------------------------------
 * Period / frequency
 *---------------------------------------------------------------------------*/
void     Pwm_SetPeriodCycles(pwm_axi_t *p, uint32_t cycles);
int      Pwm_SetFrequencyHz(pwm_axi_t *p, uint32_t hz);
uint32_t Pwm_GetPeriodCycles(const pwm_axi_t *p);
uint32_t Pwm_GetFrequencyHz(const pwm_axi_t *p);   /* actual, after rounding   */

/*-----------------------------------------------------------------------------
 * Duty cycle, per bridge (0..4).  One DUTY value drives both legs of the
 * bridge, which is what lock anti-phase (bipolar) operation needs.
 *---------------------------------------------------------------------------*/
int      Pwm_SetDutyCycles(pwm_axi_t *p, int bridge, uint32_t cycles);
int      Pwm_SetDutyPermille(pwm_axi_t *p, int bridge, uint32_t permille); /* 0..1000 */
int      Pwm_SetDutyPercent(pwm_axi_t *p, int bridge, uint32_t percent);   /* 0..100  */
uint32_t Pwm_GetDutyCycles(const pwm_axi_t *p, int bridge);
uint32_t Pwm_GetDutyPermille(const pwm_axi_t *p, int bridge);
uint32_t Pwm_GetEffectiveHighPermille(const pwm_axi_t *p, int bridge, int leg);

/*-----------------------------------------------------------------------------
 * Phase offset, per bridge (0..4), in s00_axi_aclk cycles, range [0, PERIOD).
 *
 * ALL FIVE BRIDGES SHARE ONE PERIOD COUNTER; the phase offset only changes
 * which point of that counter each bridge compares against.  Two consequences
 * worth remembering:
 *
 *   - The complementary relationship INSIDE a leg is completely unaffected.
 *     Both switches of a leg use the same shifted counter, and the two
 *     switching conditions ("counter < DUTY" for the upper switch, "counter
 *     >= DUTY" for the lower) are mutually exclusive for any counter value.
 *     So the dead time and the no-shoot-through guarantee are exactly the same
 *     at any phase setting.
 *
 *   - AH stays in phase with BL and AL stays in phase with BH, at any phase.
 *
 * The usual reason to use this is INTERLEAVING: give N bridges evenly spaced
 * offsets (see Pwm_Interleave) so their input current pulses do not line up,
 * which cuts the ripple the supply capacitor has to absorb.
 *
 * Keep the value below PERIOD.  A larger value is not dangerous (the two
 * switches of a leg still cannot conduct together) but it produces a
 * distorted duty cycle, because the shifted counter leaves its [0, PERIOD)
 * range.
 *---------------------------------------------------------------------------*/
int      Pwm_SetPhaseCycles(pwm_axi_t *p, int bridge, uint32_t cycles);
int      Pwm_SetPhasePermille(pwm_axi_t *p, int bridge, uint32_t permille); /* of PERIOD */
uint32_t Pwm_GetPhaseCycles(const pwm_axi_t *p, int bridge);
/* Spread `count` bridges evenly over one period: bridge i gets i*PERIOD/count.
 * Pwm_Interleave(&pwm, 5) is the 0 / 72 / 144 / 216 / 288 degree case. */
int      Pwm_Interleave(pwm_axi_t *p, uint32_t count);

/*-----------------------------------------------------------------------------
 * Leg B mode (see the PWM_AXI_MODE_* comment above)
 *---------------------------------------------------------------------------*/
int      Pwm_SetLegBMode(pwm_axi_t *p, int bridge, uint32_t mode);
uint32_t Pwm_GetLegBMode(const pwm_axi_t *p, int bridge);

/*-----------------------------------------------------------------------------
 * Coil / eddy current brake helpers.
 *
 * An eddy current brake or damper coil wants a smooth UNIDIRECTIONAL current,
 * not the bipolar drive a motor wants.  These four states cover it:
 *
 *   Pwm_CoilDrive(bridge, 300)   chopper at 30 %, slow decay -> low ripple hold
 *   Pwm_CoilDc(bridge)           clean DC, NO switching at all
 *   Pwm_CoilRelease(bridge)      -Vbus across the coil -> fast decay
 *   Pwm_CoilShort(bridge)        both terminals at GND -> gentle decay
 *   Pwm_GlobalEnable(0)          coast (all switches off)
 *
 * Pwm_CoilDc / Pwm_CoilRelease / Pwm_CoilShort also set the leg A dead time to
 * 0, because those three states want a genuinely static leg A.
 *---------------------------------------------------------------------------*/
int      Pwm_CoilDrive(pwm_axi_t *p, int bridge, uint32_t duty_permille);
int      Pwm_CoilDc(pwm_axi_t *p, int bridge);
int      Pwm_CoilRelease(pwm_axi_t *p, int bridge);
int      Pwm_CoilShort(pwm_axi_t *p, int bridge);

/*-----------------------------------------------------------------------------
 * Dead time, per bridge (0..4) and per leg (PWM_AXI_LEG_A / PWM_AXI_LEG_B).
 * These use a read-modify-write of the packed DEADx register, so changing one
 * leg never clobbers the other.  One leg dead time is 16 bits (0..0xFFFF).
 *---------------------------------------------------------------------------*/
int      Pwm_SetDeadtimeCycles(pwm_axi_t *p, int bridge, int leg, uint32_t cycles);
int      Pwm_SetDeadtimeNs(pwm_axi_t *p, int bridge, int leg, uint32_t ns);
int      Pwm_SetBridgeDeadtime(pwm_axi_t *p, int bridge, uint32_t cycles);
uint32_t Pwm_GetDeadtimeCycles(const pwm_axi_t *p, int bridge, int leg);
uint32_t Pwm_GetDeadtimeNs(const pwm_axi_t *p, int bridge, int leg);

/*-----------------------------------------------------------------------------
 * Half-bridge gate-driver enables.  The flat index is 0..9 and equals
 * 2*bridge + leg, i.e. it is the bit position inside PWM_EN.
 *---------------------------------------------------------------------------*/
void     Pwm_SetHalfBridgeEnable(pwm_axi_t *p, int index, int enable);
void     Pwm_SetLegEnable(pwm_axi_t *p, int bridge, int leg, int enable);
void     Pwm_SetBridgeEnable(pwm_axi_t *p, int bridge, int enable);
void     Pwm_SetAllHalfBridges(pwm_axi_t *p, int enable);
uint32_t Pwm_GetEnableMask(const pwm_axi_t *p);

/*-----------------------------------------------------------------------------
 * Status
 *---------------------------------------------------------------------------*/
uint32_t Pwm_GetStatus(const pwm_axi_t *p);
uint32_t Pwm_GetCounter(const pwm_axi_t *p);   /* live period counter, low 16 bits */

/* Low level access, if you need something the API above does not cover */
uint32_t Pwm_ReadReg(const pwm_axi_t *p, uint32_t offset);
void     Pwm_WriteReg(const pwm_axi_t *p, uint32_t offset, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* PWM_AXI_IP_BSP_H */
