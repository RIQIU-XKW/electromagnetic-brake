/*=============================================================================
 * pwm.h
 *-----------------------------------------------------------------------------
 * pwm_axi_ip AXI4-Lite 外设的 BSP 层(5 个 H 桥 / 20 路互补 PWM, Zynq-7020)。
 * 寄存器映射与 hdl/pwm_axi_ip.v 完全一致。
 *
 * 结构
 *   5 个桥, 每个桥两个半桥(leg A / leg B)。桥 i = 0..4:
 *       pwm_out[4*i+0] = AH  leg A 上管      pwm_out[4*i+1] = AL  leg A 下管
 *       pwm_out[4*i+2] = BH  leg B 上管      pwm_out[4*i+3] = BL  leg B 下管
 *       gate_en[2*i+0] = leg A 驱动使能      gate_en[2*i+1] = leg B 驱动使能
 *       ⚠ EM 脚**低有效**: 引脚低 = 开输出, 引脚高 = 关断。IP 内部已取反, 所以
 *         PWM_EN 的使能位仍然是 "1 = 使能"; 复位与过流时引脚为高(关断)。
 *   AH/AL 是一对互补, BH/BL 是另一对。桥按对角(双极性)驱动: AH 与 BL 同相,
 *   AL 与 BH 同相, 所以直通只可能发生在同一个 leg 内部, 结构上不可能发生。
 *
 * 波形与死区
 *   period 计数器 cnt 跑 0..PERIOD-1, 5 个桥共用(所以彼此锁相)。桥 i 占空比 D、
 *   leg A/B 死区 DTA/DTB 时:
 *       AH 在 cnt∈[DTA, D) 导通        AL 在 cnt∈[D+ DTA, PERIOD) 导通
 *       BL 在 cnt∈[DTB, D) 导通        BH 在 cnt∈[D+ DTB, PERIOD) 导通
 *   上管导通 (D - DT) 个周期, 下管导通 (PERIOD - D - DT) 个: 死区必然吃掉有效占空比,
 *   这是插入死区的固有现象, 不是 bug。Pwm_GetEffectiveHighPermille() 报这个值。
 *   20 路输出都过寄存器, 无毛刺, 恰好比计数器晚一个 aclk。
 *
 * 典型用法(s00_axi_aclk = FCLK_CLK0 = 50 MHz, 一个计数 = 20 ns):
 *     Pwm_Init(&pwm, XPAR_PWM_AXI_IP_0_S00_AXI_BASEADDR, 50000000u);
 *     Pwm_GlobalEnable(&pwm, 0);              // 配置期间关调制
 *     Pwm_SetFrequencyHz(&pwm, 20000u);       // 20 kHz, PERIOD = 2500
 *     Pwm_SetBridgeDeadtime(&pwm, 0, 25u);    // 500 ns = 25 计数
 *     Pwm_SetDutyPermille(&pwm, 0, 300u);     // 30.0 %, DUTY = 750
 *     Pwm_SetAllHalfBridges(&pwm, 1);         // 打开 10 个门驱动
 *     Pwm_GlobalEnable(&pwm, 1);              // 开始调制
 *===========================================================================*/

#ifndef PWM_H
#define PWM_H

#include <stdint.h>

#include "xparameters.h"

/* --------------------------------------------------------------------------
 * 板级绑定(base_system / soc_923 平台):
 *   pl.dtsi: pwm_axi_ip_0: pwm_axi_ip@43c10000 (range 0x10000), FCLK_CLK0
 * 平台 BSP 重建后会生成 XPAR_PWM_AXI_IP_0_BASEADDR; 在那之前用下面的常量。
 * ------------------------------------------------------------------------ */
#ifdef XPAR_PWM_AXI_IP_0_BASEADDR
#define PWM_BASEADDR            XPAR_PWM_AXI_IP_0_BASEADDR
#else
#define PWM_BASEADDR            0x43C10000U
#endif

/** s00_axi_aclk = FCLK_CLK0(50 MHz)。 */
#define PWM_ACLK_HZ             50000000U

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------------------------------
 * 寄存器偏移(窗口 128 字节, 基址通常在 0x43C10000)
 *---------------------------------------------------------------------------*/
#define PWM_AXI_REG_CTRL        0x00u   /* RW  [0] GLB_EN  [1] INV_PWM           */
#define PWM_AXI_REG_PERIOD      0x04u   /* RW  PWM 周期(aclk 周期数)             */
#define PWM_AXI_REG_DUTY0       0x08u   /* RW  桥 0 比较值                       */
#define PWM_AXI_REG_DUTY1       0x0Cu   /* RW  桥 1 比较值                       */
#define PWM_AXI_REG_DUTY2       0x10u   /* RW  桥 2 比较值                       */
#define PWM_AXI_REG_DUTY3       0x14u   /* RW  桥 3 比较值                       */
#define PWM_AXI_REG_DUTY4       0x18u   /* RW  桥 4 比较值                       */
#define PWM_AXI_REG_DEAD0       0x1Cu   /* RW  桥 0 死区(两个 leg 打包)         */
#define PWM_AXI_REG_DEAD1       0x20u   /* RW  桥 1 死区                         */
#define PWM_AXI_REG_DEAD2       0x24u   /* RW  桥 2 死区                         */
#define PWM_AXI_REG_DEAD3       0x28u   /* RW  桥 3 死区                         */
#define PWM_AXI_REG_DEAD4       0x2Cu   /* RW  桥 4 死区                         */
#define PWM_AXI_REG_PWM_EN      0x30u   /* RW  [9:0] 半桥使能                   */
#define PWM_AXI_REG_SCRATCH     0x34u   /* RW  scratch / AXI 回读测试           */
#define PWM_AXI_REG_ID          0x38u   /* RO  0x50574D31 ("PWM1")               */
#define PWM_AXI_REG_STATUS      0x3Cu   /* RO  状态 / 实时计数器                 */
/* 相位偏移在第二批寄存器里, 所以窗口是 128 字节而不是 64。 */
#define PWM_AXI_REG_PHASE0      0x40u   /* RW  桥 0 相位偏移(周期数)            */
#define PWM_AXI_REG_PHASE1      0x44u   /* RW  桥 1 相位偏移                    */
#define PWM_AXI_REG_PHASE2      0x48u   /* RW  桥 2 相位偏移                    */
#define PWM_AXI_REG_PHASE3      0x4Cu   /* RW  桥 3 相位偏移                    */
#define PWM_AXI_REG_PHASE4      0x50u   /* RW  桥 4 相位偏移                    */
#define PWM_AXI_PHASE_STRIDE    0x04u   /* PHASEn 之间的间距                    */

/* CTRL 位(0x00), [31:2] 保留。 */
#define PWM_AXI_CTRL_GLB_EN     (1u << 0)   /* 全局调制使能                      */
#define PWM_AXI_CTRL_INV_PWM    (1u << 1)   /* 翻转 20 路输出电平                */

/* DEADx 打包: 一个 32 位寄存器装两个 16 位死区 */
#define PWM_AXI_DEAD_LEG_A_MASK 0x0000FFFFu
#define PWM_AXI_DEAD_LEG_B_MASK 0xFFFF0000u

/* ---------------------------------------------------------------------------
 * leg B 模式, 放在 PWM_EN[19:10](每桥 2 位):
 *   NORMAL           对角驱动, leg B 和 leg A 用同一个 DUTY。V = Vbus*(2d-1)。复位默认。
 *   STATIC_LOW       BL 常通 / BH 常断, leg A 照常 PWM: 导通期 +Vbus, 关断期经两个下管
 *                    续流 —— 慢衰减, 线圈要的低纹波保持电流。
 *   STATIC_HIGH      BH 常通 / BL 常断, 配 leg A 静态低可得到 -Vbus, 电流快速下降。
 *   LEG_A_STATIC_HIGH leg A 钉成静态高(AH 常通 / AL 常断, 死区强制 0), 由 **leg B**
 *                    按 DUTY/DEAD 互补斩波: 负载 = D/PERIOD*Vbus, 单向电流、慢衰减。
 *                    本工程制动用这个(左桥 Q1/Q3 常开常关 + 右桥 Q2/Q4 斩波)。
 * 静态 leg 会忽略自己的死区(IP 把它强制清 0), 所以真的不会每周期掉几个计数。
 * -------------------------------------------------------------------------*/
#define PWM_AXI_MODE_NORMAL       0u    /* 对角驱动(默认)                    */
#define PWM_AXI_MODE_STATIC_LOW   1u    /* BL 通, BH 断                      */
#define PWM_AXI_MODE_STATIC_HIGH  2u    /* BH 通, BL 断                      */
#define PWM_AXI_MODE_LEG_A_STATIC_HIGH 3u /* AH 通, AL 断; 腿 B 按 DUTY 斩波  */
#define PWM_AXI_MODE_SHIFT        10u   /* PWM_EN[19:10]                     */
#define PWM_AXI_MODE_FIELD_MASK   3u
#define PWM_AXI_DEAD_LEG_B_SHIFT 16u
#define PWM_AXI_DEAD_MAX        0xFFFFu     /* 单个 leg 死区 16 位              */

/* STATUS 位(0x3C) */
#define PWM_AXI_STATUS_GLB_EN   (1u << 0)   /* CTRL.GLB_EN 的镜像                */
#define PWM_AXI_STATUS_PERIOD_OK (1u << 1)  /* PERIOD != 0                       */
#define PWM_AXI_STATUS_CNT_ZERO (1u << 2)   /* 实时计数器在 0                    */
#define PWM_AXI_STATUS_ANY_EN   (1u << 3)   /* PWM_EN[9:0] 的按位或              */
#define PWM_AXI_STATUS_CNT_SHIFT 16u        /* [31:16] = 计数器低 16 位          */

/* IP 标识 */
#define PWM_AXI_ID_VALUE        0x50574D31u

/* 几何 */
#define PWM_AXI_BRIDGE_COUNT      5u    /* H 桥数                            */
#define PWM_AXI_LEG_COUNT         2u    /* 每桥 leg 数                        */
#define PWM_AXI_HALF_BRIDGE_COUNT 10u   /* 半桥 / gate_en 数                  */
#define PWM_AXI_PWM_OUTPUT_COUNT  20u   /* pwm_out 数                         */

/* leg 常量 */
#define PWM_AXI_LEG_A           0       /* 带 AH(上) / AL(下) 的半桥          */
#define PWM_AXI_LEG_B           1       /* 带 BH(上) / BL(下) 的半桥          */

/* ---------------------------------------------------------------------------
 * 索引映射: 桥 i 的各开关在 pwm_out 里的位号
 *   AH = 4*i+0   AL = 4*i+1   BH = 4*i+2   BL = 4*i+3
 * 半桥在 PWM_EN / gate_en 里的位号 = 2*bridge + leg
 *---------------------------------------------------------------------------*/
#define PWM_AXI_OUT_AH(i)       (4u * (uint32_t)(i) + 0u)
#define PWM_AXI_OUT_AL(i)       (4u * (uint32_t)(i) + 1u)
#define PWM_AXI_OUT_BH(i)       (4u * (uint32_t)(i) + 2u)
#define PWM_AXI_OUT_BL(i)       (4u * (uint32_t)(i) + 3u)
#define PWM_AXI_HB_INDEX(bridge, leg) \
                                (2u * (uint32_t)(bridge) + (uint32_t)(leg))
#define PWM_AXI_EN_BIT(index)   (1u << (uint32_t)(index))
#define PWM_AXI_EN_MASK_ALL     0x3FFu  /* 10 个半桥全开                      */

/* 返回码 */
#define PWM_AXI_OK              0
#define PWM_AXI_ERR_ARG        (-1)
#define PWM_AXI_ERR_RANGE      (-2)
#define PWM_AXI_ERR_ID         (-3)

/*-----------------------------------------------------------------------------
 * 句柄。影子副本记着最后一次写进去的值, 所以 getter 不必访问 AXI 总线,
 * 周期变化时 setter 也能按新周期重算。Pwm_Get* 仍然读硬件。
 *---------------------------------------------------------------------------*/
typedef struct {
    uint32_t base;      /* 外设 AXI 基址                                        */
    uint32_t aclk_hz;   /* s00_axi_aclk 频率(FCLK_CLK0 = 50 MHz)                */
    uint32_t period;    /* PERIOD 寄存器的影子副本                              */
    uint32_t duty[PWM_AXI_BRIDGE_COUNT];
                        /* DUTY0..DUTY4 的影子副本                              */
    uint32_t dead[PWM_AXI_BRIDGE_COUNT][PWM_AXI_LEG_COUNT];
                        /* 10 个死区的影子副本(按 leg)                          */
    uint32_t enable_mask;
                        /* PWM_EN[9:0] 的影子副本                               */
    uint32_t phase[PWM_AXI_BRIDGE_COUNT];
                        /* PHASE0..PHASE4 的影子副本                            */
} Pwm_HandleTypeDef;

/*-----------------------------------------------------------------------------
 * 生命周期 / 自检
 *---------------------------------------------------------------------------*/
int      Pwm_Init(Pwm_HandleTypeDef *p, uint32_t base, uint32_t aclk_hz);
int      Pwm_SelfTest(Pwm_HandleTypeDef *p);
uint32_t Pwm_ReadId(const Pwm_HandleTypeDef *p);

/*-----------------------------------------------------------------------------
 * 全局控制
 *---------------------------------------------------------------------------*/
void Pwm_GlobalEnable(Pwm_HandleTypeDef *p, int enable);
int  Pwm_IsGlobalEnabled(const Pwm_HandleTypeDef *p);
void Pwm_SetInvert(Pwm_HandleTypeDef *p, int invert);
void Pwm_Stop(Pwm_HandleTypeDef *p);            /* CTRL = 0, 清所有门使能 */

/*-----------------------------------------------------------------------------
 * 周期 / 频率
 *---------------------------------------------------------------------------*/
void     Pwm_SetPeriodCycles(Pwm_HandleTypeDef *p, uint32_t cycles);
int      Pwm_SetFrequencyHz(Pwm_HandleTypeDef *p, uint32_t hz);
uint32_t Pwm_GetPeriodCycles(const Pwm_HandleTypeDef *p);
uint32_t Pwm_GetFrequencyHz(const Pwm_HandleTypeDef *p);   /* 取整后的实际值 */

/*-----------------------------------------------------------------------------
 * 占空比(按桥, 0..4)。一个 DUTY 驱动桥的两个 leg, 对角(双极性)就需要这样。
 *---------------------------------------------------------------------------*/
int      Pwm_SetDutyCycles(Pwm_HandleTypeDef *p, int bridge, uint32_t cycles);
int      Pwm_SetDutyPermille(Pwm_HandleTypeDef *p, int bridge, uint32_t permille); /* 0..1000 */
int      Pwm_SetDutyPercent(Pwm_HandleTypeDef *p, int bridge, uint32_t percent);   /* 0..100  */
uint32_t Pwm_GetDutyCycles(const Pwm_HandleTypeDef *p, int bridge);
uint32_t Pwm_GetDutyPermille(const Pwm_HandleTypeDef *p, int bridge);
uint32_t Pwm_GetEffectiveHighPermille(const Pwm_HandleTypeDef *p, int bridge, int leg);

/*-----------------------------------------------------------------------------
 * 相位偏移(按桥, 0..4), 单位 aclk 周期, 范围 [0, PERIOD)。
 *
 * 5 个桥共用一个周期计数器, 相位偏移只改变每个桥比较的起点, 所以:
 *   - leg 内部的互补关系完全不受影响(同一个 leg 的两个开关用同一个移位计数器,
 *     "cnt < DUTY" 和 "cnt >= DUTY" 互斥), 死区和防直通保证和相位无关;
 *   - AH 始终与 BL 同相, AL 始终与 BH 同相。
 * 常用它做交错(见 Pwm_Interleave): N 个桥均匀错开, 电流脉冲不重叠, 母线电容纹波小。
 * 值要小于 PERIOD: 大了不危险(同 leg 仍不会直通), 但占空比会畸变。
 *---------------------------------------------------------------------------*/
int      Pwm_SetPhaseCycles(Pwm_HandleTypeDef *p, int bridge, uint32_t cycles);
int      Pwm_SetPhasePermille(Pwm_HandleTypeDef *p, int bridge, uint32_t permille); /* 相对 PERIOD */
uint32_t Pwm_GetPhaseCycles(const Pwm_HandleTypeDef *p, int bridge);
/* 把 count 个桥均匀铺在一个周期上: 桥 i 取 i*PERIOD/count。
 * Pwm_Interleave(&pwm, 5) 就是 0 / 72 / 144 / 216 / 288 度。 */
int      Pwm_Interleave(Pwm_HandleTypeDef *p, uint32_t count);

/*-----------------------------------------------------------------------------
 * leg B 模式(见上面 PWM_AXI_MODE_* 的说明)
 *---------------------------------------------------------------------------*/
int      Pwm_SetLegBMode(Pwm_HandleTypeDef *p, int bridge, uint32_t mode);
uint32_t Pwm_GetLegBMode(const Pwm_HandleTypeDef *p, int bridge);

/*-----------------------------------------------------------------------------
 * 线圈 / 电涡流制动用的一组状态(线圈要平滑的单向电流, 不是电机的双极性):
 *
 *   Pwm_CoilDrive(bridge, 300)   30 % 斩波, 慢衰减 -> 低纹波保持
 *   Pwm_CoilDc(bridge)           纯直流, 完全不开关
 *   Pwm_CoilRelease(bridge)      线圈两端 -Vbus -> 快速衰减
 *   Pwm_CoilShort(bridge)        两端接地 -> 缓慢衰减
 *   Pwm_GlobalEnable(0)          自由滑行(所有开关关断)
 *
 * 后三个还会把 leg A 死区设成 0, 因为那三种状态要 leg A 真的静态。
 *---------------------------------------------------------------------------*/
int      Pwm_CoilDrive(Pwm_HandleTypeDef *p, int bridge, uint32_t duty_permille);
int      Pwm_CoilDc(Pwm_HandleTypeDef *p, int bridge);
int      Pwm_CoilRelease(Pwm_HandleTypeDef *p, int bridge);
int      Pwm_CoilShort(Pwm_HandleTypeDef *p, int bridge);

/*-----------------------------------------------------------------------------
 * 死区(按桥 0..4 / 按 leg)。读改写打包的 DEADx 寄存器, 改一个 leg 不会碰另一个。
 * 单个 leg 死区 16 位(0..0xFFFF)。
 *---------------------------------------------------------------------------*/
int      Pwm_SetDeadtimeCycles(Pwm_HandleTypeDef *p, int bridge, int leg, uint32_t cycles);
int      Pwm_SetDeadtimeNs(Pwm_HandleTypeDef *p, int bridge, int leg, uint32_t ns);
int      Pwm_SetBridgeDeadtime(Pwm_HandleTypeDef *p, int bridge, uint32_t cycles);
uint32_t Pwm_GetDeadtimeCycles(const Pwm_HandleTypeDef *p, int bridge, int leg);
uint32_t Pwm_GetDeadtimeNs(const Pwm_HandleTypeDef *p, int bridge, int leg);

/*-----------------------------------------------------------------------------
 * 半桥门驱动使能。扁平索引 0..9 = 2*bridge + leg, 就是 PWM_EN 里的位号。
 *---------------------------------------------------------------------------*/
void     Pwm_SetHalfBridgeEnable(Pwm_HandleTypeDef *p, int index, int enable);
void     Pwm_SetLegEnable(Pwm_HandleTypeDef *p, int bridge, int leg, int enable);
void     Pwm_SetBridgeEnable(Pwm_HandleTypeDef *p, int bridge, int enable);
void     Pwm_SetAllHalfBridges(Pwm_HandleTypeDef *p, int enable);
uint32_t Pwm_GetEnableMask(const Pwm_HandleTypeDef *p);

/*-----------------------------------------------------------------------------
 * 状态
 *---------------------------------------------------------------------------*/
uint32_t Pwm_GetStatus(const Pwm_HandleTypeDef *p);
uint32_t Pwm_GetCounter(const Pwm_HandleTypeDef *p);   /* 实时计数器低 16 位 */

/* 上面没覆盖到的, 直接读写寄存器 */
uint32_t Pwm_ReadReg(const Pwm_HandleTypeDef *p, uint32_t offset);
void     Pwm_WriteReg(const Pwm_HandleTypeDef *p, uint32_t offset, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* PWM_H */

/*-----------------------------------------------------------------------------
 * 诊断(app923 加的)
 *---------------------------------------------------------------------------*/
void Pwm_PrintStatus(const Pwm_HandleTypeDef *p);
/*-----------------------------------------------------------------------------
 * 过流状态(STATUS 寄存器, 只读)
 *
 * RTL 用 oc_br_off = ~oc_n | oc_src 关掉每个桥:
 *   ~oc_n    实时电平, 引脚变高就自动消失
 *   oc_src   粘滞锁存, 要软件 acknowledge 才清
 * Acknowledge = CTRL.GLB_EN 出 0 -> 1 边沿(Pwm_AcknowledgeFault() 写两次)。
 * 只把 oc_n 引脚拉高不会让桥重新工作, 那只是去掉实时项。
 *---------------------------------------------------------------------------*/
#define PWM_AXI_STATUS_OC_NOW      (1u << 4)    /* 此刻有 oc_n 输入为低          */
#define PWM_AXI_STATUS_OC_LATCHED  (1u << 5)    /* 发生过过流, 粘滞              */
#define PWM_AXI_STATUS_OC_SRC_MASK 0x00001F00u  /* STATUS[12:8]: 哪几个桥        */
#define PWM_AXI_STATUS_OC_SRC_SHIFT 8u

uint32_t Pwm_GetStatus(const Pwm_HandleTypeDef *p);
int      Pwm_OcNow(const Pwm_HandleTypeDef *p);        /* 1 = 此刻有引脚为低    */
int      Pwm_OcLatched(const Pwm_HandleTypeDef *p);    /* 1 = 发生过过流        */
uint32_t Pwm_OcSource(const Pwm_HandleTypeDef *p);     /* bit i = 桥 i 报过     */
int      Pwm_AcknowledgeFault(Pwm_HandleTypeDef *p);   /* CTRL.GLB_EN 0 -> 1    */
int      Pwm_OcLatched(const Pwm_HandleTypeDef *p);    /* 1 = 发生过过流        */
uint32_t Pwm_OcSource(const Pwm_HandleTypeDef *p);     /* bit i = 桥 i 报过     */
int      Pwm_AcknowledgeFault(Pwm_HandleTypeDef *p);   /* CTRL.GLB_EN 0 -> 1    */

/** @brief  把过流来源掩码格式化成可读的桥列表(0 -> "-", 否则 "0,2")。 */
void     Pwm_FormatOcSource(uint32_t src, char *dst, uint16_t size);

/**
 * @brief  巡检过流状态, 每次故障只打一行警告(由 monitor 周期任务调)。
 * @param  auto_ack : 1 = 恢复后自动 Pwm_AcknowledgeFault()。
 * @note   边沿触发(看 STATUS.OC_NOW | OC_LATCHED), 有最小间隔 PWM_ALARM_MIN_GAP_MS。
 *         硬件在 ~2 ns 内已经关掉了故障桥, 这里只负责报, 从不清锁存。
 */
void Pwm_Alarm_Poll(Pwm_HandleTypeDef *p, int auto_ack);
