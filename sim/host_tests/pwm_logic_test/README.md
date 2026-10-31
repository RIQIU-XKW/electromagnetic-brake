# PWM 功率级 IP(`pwm_axi_ip` v1.0)驱动宿主机测试

把移植进 `vitis2/app923/src/bsp/pwm/pwm.c` 的**真实驱动**编到 PC 上跑,AXI 那一层用 `stub/xil_io.h` + `host_regfile.c` 的 64 KB 寄存器文件替身(并模拟只读的 `ID` / `STATUS` 寄存器)。

## 验证什么(41 项)

| 组 | 内容 |
|---|---|
| [1] | 初始化、基地址(0x43C10000,与 `pl.dtsi` 一致)、`ID` = `0x50574D31`、寄存器读写自检、`Pwm_Stop` 清空使能 |
| [2] | 频率 ↔ 周期:`20 kHz→2500`、`100 kHz→500`、`3 kHz→16667`(四舍五入)、`hz=0` 与周期会算成 0 的 hz 被拒绝 |
| [3] | 占空比:`30.0%→DUTY0=750`、`25%→DUTY1=625`、`100%→2500`、`1001‰` 与越界桥号被拒绝 |
| [4] | 相位与交错:5 相交错 = `0/500/1000/1500/2000`、单桥 50% 相位 = 1250 |
| [5] | 死区打包:两条腿在同一个寄存器(A 低 16 位 / B 高 16 位),可单独设某条腿 |
| [6] | 总使能 / 反相 / 半桥使能位 / `Pwm_Stop` 把 `CTRL` 与 `PWM_EN` 都清 0 |
| [7] | 线圈四态:`CoilDrive`(斩波)、`CoilDc`(纯直流)、`CoilRelease`(快衰减)、`CoilShort`(短路制动);**并验证 `CoilXxx` 只配置不使能**(安全设计) |
| [8] | `Pwm_PrintStatus()` 不崩(监视任务用的那一行) |

## 怎么跑

```powershell
powershell -ExecutionPolicy Bypass -File tools\pwm_logic_test\run.ps1
```

全部通过返回 0,失败返回 1。当前状态:**41 项全部通过**。

## 边界说明

* 这里验证的是**驱动的寄存器编码与逻辑**。IP 内部的时序(死区插入、互补输出、过流 `oc_n` 保护、20 路输出的相位关系)由 IP 自带仿真(`ip_manage/pwm_ip/ip_repo/.../tb_pwm.v`)和上板示波器验证。
* 寄存器文件替身按 IP 手册模拟了 `STATUS` 的 `GLB_EN / PERIOD_OK / CNT_ZERO / ANY_EN` 位 —— 驱动的自检正是靠 `PERIOD_OK` 判断寄存器通路。
