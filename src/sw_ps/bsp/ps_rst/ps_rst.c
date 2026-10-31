/**
 *******************************************************************************
 * @file    ps_rst.c
 * @brief   PS 软复位实现(SLCR.PSS_RST_CTRL.SOFT_RST)
 *
 * 寄存器(Zynq-7000 TRM, SLCR 基地址 0xF8000000):
 *
 *   0xF8000008  SLCR_UNLOCK      写 0xDF0D 解锁(不写这一步, 下面的写会被忽略)
 *   0xF8000200  PSS_RST_CTRL     bit0 SOFT_RST: 写 1 -> PS 复位(调试口不断)
 *
 * 为什么用裸寄存器而不是 Xilinx 的某个 API: 标准外设库里**没有**"PS 复位"这个
 * 函数(standalone BSP 只提供看门狗复位)。Xilinx 自己的例程(比如 FSBL 的复位路径、
 * 各种 XAPP)也都是直接写这两个寄存器, 所以这里照做, 并把地址和位定义写在上面。
 *
 * 另一个路子是用 SCU 看门狗超时来复位(XScuWdt), 那个能复位得更狠(连 PL 一起),
 * 但要先把看门狗配起来 —— 本工程用不到那个力度, 而且 SCU 定时器被 FreeRTOS 当
 * tick 用(见 tools/check_invariants.ps1 里那条"不许碰 XScuTimer"), 所以不走那条。
 *******************************************************************************
 */

#define LOG_ENABLE 1
#define LOG_TAG "rst"
#include "service/monitor_module/log/log.h"

#include "bsp/ps_rst/ps_rst.h"

#include "xil_io.h"
#include "xil_cache.h"

/* SLCR */
#define PS_RST_SLCR_UNLOCK          0xF8000008U
#define PS_RST_UNLOCK_KEY           0x0000DF0DU

#define PS_RST_PSS_RST_CTRL         0xF8000200U
#define PS_RST_PSS_RST_SOFT_RST     0x00000001U

void PsRst_SoftReset(void)
{
    LOG_E("PS soft reset requested (SLCR.PSS_RST_CTRL) - the board is restarting\r\n");
    LOG_E("NOTE: PL bitstream is NOT reloaded; on JTAG boot the ROM will wait for the "
          "debugger\r\n");

    /*
     * 1) 刷 cache 并关掉。
     *    复位会把 DDR 控制器也重启, 缓存里没写回的东西就丢了。刷干净再按。
     *    关了之后后面的 Xil_Out32 不受影响 —— 设备寄存器本来就是非缓存的。
     */
    Xil_DCacheFlush();
    Xil_DCacheDisable();
    Xil_ICacheDisable();

    /*
     * 2) 解锁 SLCR, 然后写 SOFT_RST。
     *
     *    解锁必须在**同一个函数里紧挨着**做: SLCR 解锁状态是全局的, 但如果中间隔着
     *    别的任务/中断又去写 SLCR, 会把这个状态弄乱。这里中间只有两条写, 没有分支。
     */
    Xil_Out32(PS_RST_SLCR_UNLOCK, PS_RST_UNLOCK_KEY);
    Xil_Out32(PS_RST_PSS_RST_CTRL, PS_RST_PSS_RST_SOFT_RST);

    /*
     * 3) 正常情况到不了这里。
     *    真到了说明复位没生效(比如 SLCR 被别的代码又锁上了, 或者根本不是 Zynq 的
     *    PS 寄存器), 那就别死循环 —— 让调用者打日志继续跑, 至少板子还能用。
     */
    LOG_E("soft reset did NOT take effect (still running) - check the SLCR access\r\n");
}
