/**
 *******************************************************************************
 * @file    bsp_oc.c
 * @brief   过流(OC)采集驱动实现 —— 读 PL PWM IP 的 STATUS 位(见 bsp_oc.h 的说明)
 *
 * 为什么这么薄: 过流信号进了 PL, 硬件已经帮我们做好"分桥锁存"和"清标志"了,
 * PS 这边只需要把位解释成 5 路掩码。**没有任何业务判断**放在这里。
 *******************************************************************************
 */

#define LOG_ENABLE 1   /* 排障期打开: 绑定 PWM 时打一行 OC 状态 */
#define LOG_TAG "oc"
#include "service/monitor_module/log/log.h"

#include "bsp/oc/bsp_oc.h"

static Pwm_HandleTypeDef *gPwm;     /**< 借来的 PWM 句柄(不拥有) */

void Bsp_Oc_Init(Pwm_HandleTypeDef *PwmHandle)
{
    gPwm = PwmHandle;

    if (PwmHandle == NULL) {
        LOG_E("no pwm handle - overcurrent readings will always be zero\r\n");
        return;
    }

    LOG_I("bound to pwm base 0x%08X, %u channels, status 0x%08X\r\n",
          (unsigned)PwmHandle->base, (unsigned)BSP_OC_CHANNELS,
          (unsigned)Bsp_Oc_RawStatus());
}

int Bsp_Oc_IsReady(void)
{
    return (gPwm != NULL) ? 1 : 0;
}

uint32_t Bsp_Oc_RawStatus(void)
{
    if (gPwm == NULL) {
        return 0U;
    }

    return Pwm_GetStatus(gPwm);
}

int Bsp_Oc_AnyNow(void)
{
    if (gPwm == NULL) {
        return 0;
    }

    return Pwm_OcNow(gPwm);
}

uint32_t Bsp_Oc_NowMask(void)
{
    if ((gPwm == NULL) || (Bsp_Oc_AnyNow() == 0)) {
        return 0U;
    }

    /*
     * ⚠ 硬件只给了"合并的 now"(STATUS[4]), 没有"每路此刻的电平"。既然现在确实有输入
     *   在报, 就把"曾经报过的桥"当成正在报的桥 —— 这是能在不改 PL 的前提下拿到的最
     *   接近的信息, 用途是让 app 知道"哪几路", 不是拿它当逐路实时电平用。
     *   如果哪天真需要逐路实时值, 得改 PL(把 oc_n 同时挂到 PS GPIO, 或加一个
     *   "每路 now"的寄存器)。
     */
    return Pwm_OcSource(gPwm) & BSP_OC_MASK_ALL;
}

uint32_t Bsp_Oc_LatchedMask(void)
{
    if (gPwm == NULL) {
        return 0U;
    }

    return Pwm_OcSource(gPwm) & BSP_OC_MASK_ALL;
}

int Bsp_Oc_AnyLatched(void)
{
    if (gPwm == NULL) {
        return 0;
    }

    return Pwm_OcLatched(gPwm);
}

int Bsp_Oc_Acknowledge(void)
{
    if (gPwm == NULL) {
        return 0;
    }

    return (Pwm_AcknowledgeFault(gPwm) == PWM_AXI_OK) ? 1 : 0;
}

void Bsp_Oc_FormatMask(uint32_t Mask, char *Dst, uint16_t Size)
{
    uint16_t pos = 0U;
    uint32_t i;

    if ((Dst == NULL) || (Size == 0U)) {
        return;
    }

    Dst[0] = '\0';

    for (i = 0U; i < BSP_OC_CHANNELS; i++) {
        if ((Mask & (1U << i)) == 0U) {
            continue;
        }

        if (pos != 0U) {
            if ((pos + 1U) >= Size) {
                break;
            }
            Dst[pos] = ' ';
            pos++;
        }

        /* "B1" .. "B5": B = bridge */
        if ((pos + 2U) >= Size) {
            break;
        }
        Dst[pos] = 'B';
        pos++;
        Dst[pos] = (char)('1' + (char)i);
        pos++;
        Dst[pos] = '\0';
    }

    if (Mask == 0U) {
        /* 什么都不写 = 空串, 调用方自己决定显示什么 */
    }
}
