# led_test —— 4 路呼吸灯(breath_led IP)宿主机测试

把 **app923 的真实生产代码**编到 PC 上跑，验证 4 路呼吸灯的驱动和命令链路。
不需要板子，几秒钟出结果。

```
powershell -ExecutionPolicy Bypass -File tools\led_test\run.ps1
```

## 编的是什么

一条链路到底，全是板子上跑的那份源码：

```
Svc_Net_Cmd_Dispatch()        service/inter_module/net_cmd.c  命令注册表 + 精确/兜底分发
  └─ Monitor_App_StateCommand app/monitor_app.c   LED_STA_0..3（老 RS-485 命令）
     Monitor_App_Command      app/monitor_app.c   LEDSTAT/LEDMASK/LEDON/LEDOFF/LEDFREQ
       └─ Svc_Led_Request*  →  队列  →  led 任务   service/monitor_module/led/service_led.c
          └─ Led_SetEnable / SetMask / SetFreq / SetMode   bsp/led/led.c
             └─ Xil_Out32                                  host_regfile.c（64 KB 寄存器文件替身）
```

⚠ 命令表由**测试自己**拼一份（`test_led.c` 里的 `LedTest_Commands`）：板子上那张全板统一的
命令地图在 `app/inter_app.c`，而它把制动/发射/采样/按键几个模块都拉进来了 —— 为了测灯去编
那 4 个模块 + 它们的 service/bsp 不值得。处理函数用的仍是生产代码里的那两个。

只有三处是替身：

| 替身 | 文件 | 说明 |
|---|---|---|
| AXI 寄存器文件 | `host_regfile.c` | 64 KB 数组，`CTRL`/`PERIOD` 原样存，**不夹取、不截断** |
| OSAL | `host_osal.c` | 用 pthread 实现真队列 + 真任务，测试扮演应用层自己 `Osal_TaskCreate()` 起任务 |
| `Svc_Net_Reply` | `host_osal.c` | 把回复文本抓到全局缓冲，供断言检查 |

寄存器替身故意不做任何加工：IP 的夹取（10..1000）和 `PERIOD` 只有 10 位这两件事
必须由**驱动**保证，替身一旦"帮忙"就会把驱动的 bug 藏起来。

## 覆盖范围

**A. 驱动寄存器级**

- 初始化：`CTRL = 0`、掩码 0、`PERIOD = 100`
- 单路开关只动自己那一位（`LED0`→`0x01`、`+LED2`→`0x05`、`+LED3`→`0x0D`、`+LED1`→`0x0F`）
- `Index >= 4` / `On > 1` 被拒且**不碰硬件**
- 整体掩码：`0xFF` 的高位被吃掉（IP 只有 `bit[3:0]`）
- 频率夹取：`5 → 10`、`5000 → 1000`，且写进寄存器的值不超 10 位
- **老坑回归**：掩码为 0 时 `Led_SetFreq()` 必须点亮指示灯（`CTRL` 不能是 0）
- **影子诚实性**：`SetFreq(5000)` 之后 `Led_GetState()` 必须报夹取后的 1000，
  不能报 5000（报 5000 的话 `STATUS`/`LEDSTAT` 会印出板子上不存在的频率）
- 模式语义：`OFF` = 四路全灭；其它模式只在"全灭"时点亮指示灯，**不会关掉**
  用户已经点亮的其它路
- 换算表与边界（40/41/400/401）

**B. 命令级（真实分发链路）**

- `LEDMASK 0-15`、`LEDON/LEDOFF 0-3`、`LEDFREQ 10-1000`、`LEDSTAT`
- 参数越界全部被拒：`LEDMASK 16`、`LEDMASK 0xF`（只认十进制）、`LEDON 4`、
  `LEDFREQ 6`/`LEDFREQ 5000`
- 被拒的命令**一个字节都不进队列**，硬件不变
- `LEDSTAT` 是纯查询：只回话，不入队、不改硬件
- 老命令 `LED_STA_0..3` 的回复文本和寄存器效果完全不变，而且不会把用户点亮的
  其它路一起关掉
- **命名空间**：LED 兜底项只认 `LED*`，`PWMID`/`MOTORSTAT`/`STATUS` 一律返回 0
  （不被抢走）；`LED_STA_0` 仍然命中精确匹配里的老处理函数
- **网口在线指示**：上线/断线只动 `LED0`，用户用 `LEDON` 点亮的 `LED1/LED3`
  保持点亮（4 路扩宽前是整颗灯的模式命令，那会四路全灭）

## 断言里钉住的三个真实缺陷

这个测试在写的过程中抓出来的，不是事后补的：

1. **`LEDON` 被当成 `LEDOFF`**（`service_led.c`）—— 区分两者的是下标 4 的
   `'N'`/`'F'`，原来写成了下标 3，而两边都是 `'O'`。后果是 `LEDON 1` 的参数起点
   被算成 7，长度 7 的报文解析出"没有参数"，只回一句用法提示、什么都不做。
2. **`LEDFREQ` 没查下界**（`service_led.c`）—— 只查了 `> 1000`，于是 `LEDFREQ 6`
   被收下、驱动夹成 10，回复却写着 `led freq -> 6 Hz`。
3. **影子存的是未夹取的频率**（`led.c`）—— `SetFreq(5000)` 后
   `Led_GetState()` 报 5000，硬件按 1000 跑。

## 需要注意的实现细节

- **要 `-lpthread`**：led 任务是 pthread 真跑起来的。测试的同步方式不是 sleep 猜
  时间，而是等 `Svc_Led_GetSnapshot().Commands` 涨到期望值 —— 那个计数在寄存器写
  **之后**才加，看到它就说明写完了。
- **超时用 `GetTickCount` 而不是数 `Sleep(1)`**：Windows 上 `Sleep(1)` 的实际粒度
  是计时器 tick（常见 15.6 ms），数 2000 次会变成等 31 秒；命令一旦没被执行，
  整个测试要跑十几分钟，看起来就像卡住了。
- **不能为了改几个字用 PowerShell 的 `Set-Content -Encoding UTF8` 重写源文件**：
  Windows PowerShell 5.1 会用 ANSI(GBK) 读入本工程的 UTF-8 中文注释，再写成
  UTF-8，中文会变成乱码。用编辑器或 `write` 类工具改。
- **需要 lwIP 头**：`service/inter_module/service_net.h` 会拉进 `bsp/enet/enet.h`，而它内嵌
  整个 `struct netif`。`run.ps1` 把 Vitis 导出的平台 include 根加进了搜索路径
  （`vitis2\soc_923\export\soc_923\sw\freertos_ps7_cortexa9_0\include`），
  所以要先构建过平台；`stub\` 排在它前面，所以 `xil_io.h`/`xparameters.h` 仍走替身。

## 文件

```
run.ps1            编译 + 运行（ASCII only：PS 5.1 按 ANSI 读 .ps1）
test_led.c         断言（A 驱动段 + B 命令段）
host_regfile.c     AXI 寄存器文件替身 + 日志/xil_printf 桩
host_osal.c        OSAL 的 pthread 实现 + Svc_Net_Reply 抓取
stub/xil_io.h      Xil_In32/Out32 声明
stub/xparameters.h XPAR_BREATH_LED_0_BASEADDR = 0x43C00000
stub/xil_printf.h  xil_printf 声明
```

## 不覆盖的部分

IP 自身的时序（呼吸波形的 ramp 计数、共享频率四路同相）由 IP 的 RTL 仿真
（`xsim`）和上板示波器验证；这里验证的是**驱动逻辑、寄存器编码和命令语义**。
