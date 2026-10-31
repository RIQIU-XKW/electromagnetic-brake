# current_loop_test —— 电流闭环的主机测试

把真的 `service_pid.c` + `service_current_loop.c` 拿到 PC 上编,ADS 反馈、PWM 输出、
OSAL、日志全部换成桩,然后把那个 10 ms 的控制任务**一拍一拍**地驱动,每拍之后做断言。

不需要板子,也不用连 Vitis:

```powershell
.\tools\current_loop_test\run.ps1
# gcc 不在默认路径时:
.\tools\current_loop_test\run.ps1 -Gcc D:\mingw64\bin\gcc.exe
```

最后一行 `RESULT: all checks passed` 就是过。

## 测的是什么

| # | 断言 | 为什么值得测 |
|---|------|--------------|
| 0 | 上电空转(目标 0)时一次 PWM 都不写 | 别在没人下命令时动功率级 |
| 1 | 积分是 float 累加 | 需求明确要求"浮点小数, 不用整数定点模拟"; 定点实现在这里会被截断成 0 |
| 2 | 顶到上限后积分冻结 | 抗饱和: 不冻结的话目标一降要很久才退饱和 |
| 3 | 每拍输出增量 ≤ 爬坡步长 | 占空比要平滑爬升, 不能一步拍到上限 |
| 4 | 5 个桥各钳在自己的上限 37/40/28/45/75 % | 上限是硬指标; 还顺带验证 5 路不是同一个值 |
| 5 | 目标 0 时爬回 0, 然后自己关桥 + 关调制 | "不要电流"必须真的停输出 |
| 6 | 外部停之后不许自己把桥重新打开 | **安全**: 0xA5 停制动 / 过流保护 / 切回占空比模式时, 闭环下一拍不能把桥又打开 |
| 7 | 停之后再收到指令能重新接管 | "最后一条命令说了算" |
| 8 | 5 路反馈各读各的 | 5 个独立闭环, 不许共用同一个采样 |
| 4 | 投入时死区默认 100 ns, 且只写一次 | 上位机在 PWM 栏发 `0xDC` 帧改掉之后, 闭环不许每拍刷回 100 ns |

## 文件

```
run.ps1            编译 + 跑(ASCII only: PowerShell 5.1 按 ANSI 读 .ps1)
test_current_loop.c  断言(中文注释)
host_stubs.c/.h    桩实现: 假电流 / 记 PWM 输出 / Osal_DelayMs 单步
stub/              覆盖真头文件的桩: service/Samp_module/service_ads.h,
                   service/brake_module/service_pwm.h,
                   service/monitor_module/log/log.h, osal/osal.h
```

`stub/` 必须排在 `-I$src` **前面**,否则 `#include "service/..."` 会编到真的头文件,
一路拖出 `xparameters.h` / `xil_printf.h`(Xilinx 才有的头),在 PC 上直接编不过。

## 单步是怎么做的

`Svc_CurLoop_Task()` 是 `for(;;)` 死循环。桩里的 `Osal_DelayMs()` 每被调一次算走完
一拍,走够 `Host_Step()` 要求的拍数就 `longjmp` 回测试代码。所有状态都在静态变量里,
所以"分成很多段跑"和"一口气跑"对被测逻辑完全等价。

想把闭环自己打的那几行日志打出来看,把 `test_current_loop.c` 里的
`Host_SetLogVerbose(0)` 改成 `1`。
