# 上位机（Qt6）

`src/sw_host/Electromagnetic_braking/`
- `widget.cpp/.h/.ui` — 主窗口：连接、曲线勾选、预设页控件、日志、CSV 导出
- `plotwidget.cpp/.h` — 自绘实时曲线（纯 QPainter，不依赖 QtCharts）
- `ledindicator.cpp/.h` — 右上角那对圆形状态灯

## TCP 与发送

- `sendCommand(QString)`：文本命令，`write(cmd.toUtf8() + "\r\n")`，一条一次 write。
- `sendByte(uchar)` / `sendBytes(QByteArray)`：**裸字节**（制动 0xA0..0xA5、灯 0xFE/0xFF、
  制动帧 0xDA/0xDC、电流帧 0xBA..0xAB），绝不走文本封装，否则板子收到的是 `"A0\r\n"` 三个字符。
- 带参数的帧/命令必须**一次 write 发完**（同一 QByteArray）。

## 连上/断开的默认动作

`onSocketConnected()`：
1. `resetBinaryState()`（制动开关、两个圆点、接收缓冲复位 → 顺带锁住电流指令滑块）
2. 发 `PING`、`STATUS`，60 ms 后发 `0xF4` 把灯勾选框刷成板子真实状态
3. **默认"制动关闭"**：`applyDutyDeadCoupling()` → 发 `0xDA` 占空比帧 → 发 `0xDC` 死区帧
   → 发一条 `0xBA` **0 mA** 电流帧（清掉上一次可能残留的电流目标，注意不是把滑块旧值发过去）
   → 40 ms 后发 `0xA5`，按钮保持"启动制动"
4. 若勾了"连上就采集"，补发 `0xE0`

断开时发 `0xA5`（关制动）并复位状态。

## 「控制参数预设」页（制动那一组）

| 控件 | 名字 | 规则 |
|---|---|---|
| 占空比滑块 | `sliderBrakeDuty` | 0..70；拖动实时下发 `0xDA` 帧，**50 ms 节流**（`kDutySendGapMs`），松手补发最终值 |
| 占空比数值 | `labelBrakeDutyValue` | 显示 `NN %` |
| 死区 | `spinBoxBrakeDeadNs` | 100..10000 ns（步进 20 ns = 1 aclk 周期）；占空比 0 时置灰并强制 0 |
| 下发死区 | `pushButtonApplyBrakeDead` | 发 `0xDC` 帧；占空比 0 时禁用 |
| 电流指令滑块 | `sliderCurrentCmd` | 0..15000 mA；拖动实时下发 `0xBA` 帧（50 ms 节流）。**制动没开时禁用并强制归 0** |
| 电流归零 | `pushButtonCurrentZero` | 发 0 mA；同样只在制动开着时可用 |
| 电流零点校准 | `pushButtonBrakeZero` | 发 `0xE3`；要求"采样中 + 制动关闭"，否则界面先拦下 |

关键常量与联动（`widget.cpp` 顶部 / `applyDutyDeadCoupling()`）：

- `kDutyMaxPercent = 70`：`sendBrakeDutyFrame()` 里 `qBound(0, percent, 70)`。
- `kDeadMinNs = 100`：`sendBrakeDeadFrame()` 把 1..99 ns 夹到 100；`onApplyBrakeDead()` 手输小于 100
  会改成 100 并提示。
- `kCurrentMaxMa = 15000`、`kBinCurrentHead/Tail = 0xBA/0xAB`：`sendCurrentFrame()` 组
  `[0xBA][低][高][0xAB]`（**低字节在前**）。
- `applyDutyDeadCoupling(percent)`：占空比 0 → 死区置 0、置灰、下发 0 并记住原值；
  回到 >0 → 恢复记住的值（不足 100 按 100）并立刻下发，避免板子停在 0 死区上斩波。
- `updateCurrentCmdEnabled()`（挂在 `updateBrakeButton()` 末尾）：
  **制动没开 → 电流滑块/归零按钮/标签一起置灰 + 滑块归 0（不发帧）**，
  并且 `onCurrentSliderMoved/Released/onCurrentZeroClicked` 里再挡一道 `if (!m_brakeOn) return;`
  —— 电流闭环只要目标 > 0 就会自己开桥，这是安全问题不是界面问题。

## 制动按钮

"启动制动"一次发四样：`0xDA` 占空比帧 → `0xDC` 死区帧 → `0xA0`（开调制）→ `0xA1`（五桥同时开）；
再点一次发 `0xA5`。文字/配色由 `updateBrakeButton()` 统一维护。

## 绘图（`PlotWidget`，**左右两张图**）

界面是**两张并排的图**（`splitterPlots` 里左右各一列，每列上面一行自己的勾选框）：

| 图 | 控件 | 画什么 |
|---|---|---|
| 左 | `plotWidget` | 线圈 1~5 电流(A) + 母线电压(V) |
| 右 | `plotWidgetMover` | 动子速度(mm/s) + 动子位移(mm) |

- 8 条曲线：`线圈1..5电流(A)`、`动子速度(mm/s)`、`母线电压(V)`、`动子位移(mm)`。
  名字/颜色/单位只在 `kCurves[]` 一处定义（图例、勾选框颜色、CSV 表头都从它来）。
- **曲线归属表** `m_curvePlot[]` / `m_curveLocal[]`（`widget.h`）：两个 PlotWidget 各自从 0
  编号，所以 `appendPoint` / `setCurveVisible` 必须用**局部序号**（`plotOfCurve()` / `localOfCurve()`）。
  注册时按到达顺序自动分配局部序号，加曲线只改 `kCurves[]` + `m_curvePlot[]`。
- **铺满**：每列右上角一个 `铺满/还原` 按钮，或 **Ctrl+双击**这张图 → 隐藏另一列让它占满绘图区
  （只作用于绘图区，不动窗口）；**Esc** 也能还原。实现就是 `setVisible` 另一列，见 `setMaximizedPlot()`。
- 坐标框居中（对称边距 padX=60 / padY=44），**纵轴不画标题**，数值和单位由鼠标准星给出。
- 准星读数：`t = ...` + 每曲线 `名称 = 值 单位`（3 位小数）。
- **游标**：**双击**图内任意位置加一个游标（橙色点划竖线 + 读数框，框里是横轴值 + 每条显示中曲线
  的值）；双击同一处（±6 px）删掉；第二个起多出 `Δ` 行（相对第一个游标）。右键菜单可
  "清除游标 / 回到默认视图 / 显示隐藏图例"。游标只存 X，读数每帧按当前数据重算。
- 纵轴自动缩放默认开（**每张图各自**按当前勾选且有数据的曲线，上下留 8%）；`Shift`+滚轮缩放纵轴时
  会**自动接管**（取消自动缩放、把当前区间回填到输入框，两张图都能触发，用 `sender()` 分辨是哪张），
  滚轮缩横轴，左键拖动平移。
- 数据集：`DATA` 行 **10 个数**（时间 + 5 电流 + 动子速度 + 母线电压 + 动子位移 + 激光电压）；
  `EMCD` 回放同一套解析。多出来的数按 `(i-1) < CurveCount` 截断，所以固件加字段不会打乱曲线。
- 纵轴设置的 ini 键是 `view/yTitleA|yMinA|yMaxA`（换过口径：电压 → 电流），旧键故意作废。

## 本地模拟

`onDemoToggled()` 起一个定时器造 `DATA` 数据：线圈电流随制动过程爬升 + 纹波，母线 24 V 左右回落；
动子按"1 s 匀加速到 280 mm/s → 指数制动"，**位移是速度的解析积分、一路减小**（480 → ~144 mm）。
用于没有板子时验证勾选/自动缩放/游标/CSV 导出。

⚠ 每 8 s 回卷一次：回卷时会把动子那两条曲线的数据清掉 —— 否则位移从 144 跳回 480 会在
10 ms 差分里造出一个 500 mm/s 的**假尖峰**（真板子不会跳）。

