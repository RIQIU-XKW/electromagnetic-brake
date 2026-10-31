#ifndef WIDGET_H
#define WIDGET_H

#include <QAbstractSocket>
#include <QByteArray>
#include <QColor>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QCloseEvent;
class QKeyEvent;
class QLabel;
class QTcpSocket;
class QTimer;
class PlotWidget;

QT_BEGIN_NAMESPACE
namespace Ui {
class Widget;
}
QT_END_NAMESPACE

/*!
 * \brief 电磁制动试验平台主界面。
 *
 * 与参考图一致的三块区域: 顶部大标题 / 左侧"实时数据 + 曲线" / 右侧连接与操作按钮。
 * 通信部分由串口改为网口: 用 QTcpSocket 连到板子的 TCP 文本命令服务
 * (app923 的 net 组件, 默认 192.168.1.10:7), 一条命令一个 TCP 报文。
 *
 * 曲线一共 7 条: 线圈1~5 电流 + 速度 + 母线电压, 在"实时采集数据"页可以用勾选框挑显示哪几条。
 * 线圈那 5 条是**线圈电流(A)**(下位机按 I = (1.5V - 2.5)*10 换算过), 母线那条是
 * **母线电压(V)**(下位机按 11:1 还原), 速度那条是 CH6 的引脚电压(V)。
 *
 * 认得的遥测行(关键字不分大小写, 数值之间用空格或逗号隔开, 行首的 [tag] 前缀会被去掉):
 *   DATA <t> <i1> <i2> <i3> <i4> <i5> <speed> [vbus]
 *                                               第 1 个是横坐标(秒), 后面按通道顺序
 *                                               —— 板子实际发的就是这一条(8 个数);
 *                                               只发 7 个(老固件)也认, 母线那条留空
 *   CUR <i1> <i2> <i3> <i4> <i5>                5 个线圈电流, 横坐标 = 采样序号(也认 CURRENTS)
 *   COIL <n> <i>                                单个线圈(n = 1..5)
 *   SPEED <v> / VEL <v>                         只给速度
 *   i1=.. i2=.. i5=.. speed=/vel= t=/time=/x=      key=value 形式, 认哪几个通道就填哪几个
 *   裸数字行                                     5 个数 = 5 个线圈; 6 个数 = 5 线圈 + 速度;
 *                                                其他个数按通道顺序填, 多的丢掉; 横坐标 = 采样序号
 * 解析不出数值的行一律返回 false, 不会被画进曲线。
 *
 * ★ 除了上面这些**文本行**, 同一条网口上还有一套**单字节二进制**的短报文(下位机主动推):
 *   下行 [0xA0] 制动整体使能 / [0xA1] 制动整体启动 / [0xA5] 关闭制动(停止时序 + 去使能);
 *        [0xA2]/[0xA3]/[0xA4] 预设占空比 低/中/高(30%/50%/70%)
 *   上行 [0xB0][mask] 光幕状态(bit i = 第 i+1 路被遮挡, 变化时才推)
 *        [0xC0][src]  过流告警(src 的 bit i = 第 i+1 个桥过流, 检测到就推)
 *   二进制字节和文本行**可能粘在同一个 readAll() 里, 也可能被拆成两包**, 所以
 *   onReadyRead() 是按字节扫描、带跨包缓冲的, 见那里的注释。
 */
class Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Widget(QWidget *parent = nullptr);
    ~Widget() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;   ///< Esc = 退出"铺满"

private:
    /*! 曲线通道。索引同时也是 PlotWidget 里的曲线序号。 */
    enum CurveId {
        CurveCoil1 = 0,   // 线圈1 的电流(A)
        CurveCoil2,       // 线圈2 的电流
        CurveCoil3,       // 线圈3 的电流
        CurveCoil4,       // 线圈4 的电流
        CurveCoil5,       // 线圈5 的电流
        CurveSpeed,       // 激光测速 mm/s(DATA 行第 7 个数; 丢失目标时是 0)
        CurveVoltage,     // 母线电压 vbus(DATA 行的第 8 个数, 已按 11:1 还原)
        CurveLaser,       // 激光位移 mm(DATA 行的第 9 个数, 2026/10 新增)
        CurveCount        // = 8
    };

    /*! 板上 4 路呼吸灯。索引就是 CTRL 寄存器里的 bit0..3, 也就是命令里的灯号。 */
    enum LedId {
        Led0 = 0,         // K16 · 同时是"网口在线"指示灯
        Led1,             // J16
        Led2,             // K14
        Led3,             // J14
        LedCount          // = 4
    };

    /*! 缓存区里的一行采样。 */
    struct Sample {
        double x = 0.0;
        double value[CurveCount] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        bool   valid[CurveCount] = {false, false, false, false, false,
                                    false, false, false};
    };

    /* ---- 初始化 ---------------------------------------------------------- */
    void setupUiExtras();
    void setupTopLogo();      ///< 顶部左上角的校徽(和标题同一行, 见 .cpp 里的说明)
    void connectSignals();
    void applyButtonStyle();
    void loadSettings();
    void saveSettings();

    /* ---- 通用工具 -------------------------------------------------------- */
    void    appendLog(const QString &text,
                      const QColor   &color = QColor(0x20, 0x20, 0x20),
                      bool            withStamp = true);
    void    sendCommand(const QString &command, bool logIt = true);
    void    sendSequence(const QStringList &commands, int gapMs = 30);
    void    setLinkState(bool connected, const QString &text);
    void    updateConnectButton(bool connected);
    void    updateStartButton();                    ///< "开始采集/停止采集"开关的样式
    void    updateStatusLabel();
    void    connectToBoard();
    bool    isConnected() const;

    /* ---- 过流(PWMFAULT) -------------------------------------------------- */
    bool    parseOcReply(const QString &line);   ///< 是过流行就解析并返回 true
    void    pollOc();                            ///< 定时巡检: 发一条 PWMFAULT
    void    onOcPollToggled(bool on);
    void    updateOcPollTimer();

    /* ---- 4 路呼吸灯(LEDxxx) ---------------------------------------------- */
    int     ledMaskFromUi() const;               ///< 4 个勾选框 → bit0..3 掩码
    void    setLedMaskToUi(int mask);            ///< 掩码 → 4 个勾选框(不触发信号)
    QString ledMaskText(int mask) const;         ///< 掩码 → "LED0+LED2" / "四路全灭"
    bool    parseLedReply(const QString &line);  ///< 是 LED 回复就同步界面并返回 true
    void    onApplyLed();                        ///< 下发 0xFE <频率/10> + 0xFF <掩码>
    void    onLedAllOn();                        ///< 四路全亮
    void    onLedAllOff();                       ///< 四路全灭
    void    reloadLedFromBoard();                ///< 发 0xF4 并按回复回填界面


    /* ---- 右侧按钮 -------------------------------------------------------- */
    void onConnectButton();
    void onStartAcquire();
    void onLaunchCar();
    void onBrake();
    void onSnapshot();
    void onClearCache();
    void onStopAll();

    /* ---- 单字节二进制协议(制动 / 光幕 / 过流) ---------------------------- */
    void    sendByte(unsigned char code, const QString &note);  ///< 发 1 个原始字节
    void    sendBytes(const QByteArray &bytes, const QString &note);  ///< 发一串原始字节(命令+参数)
    void    handleBinaryFrame(unsigned char cmd, unsigned char arg);  ///< 2 字节上报的一条
    void    updateBrakeButton();                 ///< "启动制动/关闭制动"开关的文字与配色
    void    setupStatusIcons();                  ///< 制动按钮下面那两个圆形图标
    void    updatePositionIcon();                ///< 光幕掩码 → 左侧"小车运行位置"
    void    updateSystemIcon();                  ///< 过流状态 → 右侧"系统状态"
    void    resetBinaryState();                  ///< 连上/断开: 制动开关 + 两个圆点 + 接收缓冲复位

    /* ---- 制动占空比 / 死区(控制参数预设页) ------------------------------- */
    void    sendBrakeDutyFrame(int percent);     ///< 发 [0xDA][hi][lo][0xDB]
    void    sendBrakeDeadFrame(int ns);          ///< 发 [0xDC][hi][lo][0xDD]
    void    onBrakeDutySliderMoved(int percent); ///< 滑块拖动(节流下发)
    void    onBrakeDutySliderReleased();         ///< 松手补发一次最终值
    void    onApplyBrakeDead();                  ///< "下发死区"按钮
    void    onCalibrateCurrentZero();            ///< "电流零点校准"按钮(0xE3)
    void    applyDutyDeadCoupling(int percent);  ///< 占空比 0 ⇒ 死区必须 0(反之恢复)

    /* ---- 电流指令(0xBA..0xAB) -------------------------------------------- */
    void    sendCurrentFrame(int milliAmps);     ///< 发 [0xBA][lo][hi][0xAB]
    void    onCurrentSliderMoved(int milliAmps); ///< 滑块拖动(节流下发)
    void    onCurrentSliderReleased();           ///< 松手补发最终值
    void    onCurrentZeroClicked();              ///< "电流归零(0 A)"
    void    resetCurrentCmdUi();                 ///< 滑块摆回 0(不触发下发)
    void    updateCurrentCmdEnabled();           ///< 制动没开: 电流指令归 0 并锁住滑块

    /* ---- 网口 ------------------------------------------------------------ */
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketErrorOccurred(QAbstractSocket::SocketError error);
    void onReadyRead();

    /* ---- 数据解析 -------------------------------------------------------- */
    void handleLine(const QString &line);
    bool parseTelemetry(const QString &line,
                        double                     *x,
                        QVector<QPair<int, double>> *values,
                        bool                       *hasExplicitX);
    void feedSample(double x, const QVector<QPair<int, double>> &values);

    /* ---- 本地模拟(无硬件调试) -------------------------------------------- */
    void onDemoToggled(bool on);
    void onDemoTick();

    /* ---- 预设 / 显示设置 -------------------------------------------------- */
    void onApplyDisplaySettings();
    void onAutoScaleYToggled(bool on);          ///< "纵轴自动缩放"勾选框
    void onAutoScaleYTakenOver();               ///< Shift+滚轮缩了纵轴, 自动缩放被接管
    void syncCurveVisibility();                 ///< 曲线勾选框 → PlotWidget 的显示集合
    QList<QCheckBox *> curveBoxes() const;      ///< 曲线勾选框, 按 CurveId 顺序

    /*!
     * 曲线 → 图的归属(2026/11 拆成左右两个图)。
     *
     * 左边 plotWidget 画"电气量"(线圈 1~5 电流 + 母线电压), 右边 plotWidgetMover 画
     * "动子"(速度 + 位移)。两个 PlotWidget 各自从 0 开始给自己的曲线编号, 所以每次
     * appendPoint/setCurveVisible 都要用**那条曲线在自己图里的局部序号**, 不能直接拿
     * CurveId 当序号 —— 这两张表就是干这个的。
     */
    enum PlotId { PlotElectrical = 0, PlotMover, PlotCount };
    PlotWidget *plot(PlotId id) const;
    PlotWidget *plotOfCurve(int curveId) const;  ///< 这条曲线归哪个图
    int         localOfCurve(int curveId) const; ///< 这条曲线在它那个图里的局部序号
    QString     curveName(int curveId) const;    ///< 曲线名(CSV 表头用, 取自它所在的图)

    /*!
     * 把某一张图铺满绘图区(隐藏另一张), 再调一次还原成左右双图。
     * @param id : PlotElectrical / PlotMover; 传 -1(见 PlotId) 表示还原。
     *
     * "铺满"只作用于**绘图区**(那条水平分隔条里), 不动整个窗口 —— 接收区、右侧控制栏
     * 都还在, 所以它不是软件窗口全屏, 只是把另一张图让出来。
     */
    void setMaximizedPlot(int id);
    void onMaxLeftToggled(bool on);
    void onMaxRightToggled(bool on);
    void onMaximizeRequested();                 ///< 某张图被 Ctrl+双击了

    /*! 制动强度下拉(低/中/高) → 下发 0xA2 / 0xA3 / 0xA4 三个字节。 */
    void onBrakingLevelChanged(int index);

    QString exportCsv();

    Ui::Widget *ui;

    /*! 每条曲线登记在哪个图、那个图里的局部序号(见 PlotId 的说明)。 */
    PlotId m_curvePlot[CurveCount]  = { PlotElectrical, PlotElectrical, PlotElectrical,
                                        PlotElectrical, PlotElectrical, PlotMover,
                                        PlotElectrical, PlotMover };
    int    m_curveLocal[CurveCount] = { 0, 1, 2, 3, 4, 0, 5, 1 };

    /*! 现在铺满的是哪张图(-1 = 左右双图)。 */
    int m_maxPlot = -1;

    QTcpSocket *m_socket    = nullptr;
    QTimer     *m_demoTimer = nullptr;

    /*! 遥测不进接收区时, 只提示一次。 */
    bool m_telemetryNoticeShown = false;

    /*!
     * 过流告警(0xC0 上报)带回来的"哪几个桥过流"位图, 显示用(0 = 没收到/没报)。
     * ⚠ 这里原来还有 m_ocPollTimer / m_ocReplyQuiet / m_ocNow / m_ocLatched —— 那套是
     *   "定时发 PWMFAULT 再解析文本回复"的巡检机制, 下位机 PWM 命令表删掉后整块去掉了:
     *   过流改成下位机**主动上报** 0xC0(m_ocAlarm + m_ocSrc)。
     */
    QString m_ocSrc = QStringLiteral("0");

    /*! 状态标签的文字/颜色由这几项拼出来。 */
    QString m_linkText = QStringLiteral("未连接");
    bool    m_linkConnected = false;

    /*!
     * updateStatusLabel() 的去重/节流状态(2026-10-16 加, 修"图像绘制过多导致卡顿")。
     *
     * ⚠ 这个函数是**每一条遥测**(板子 100 Hz)都会调用的, 而它里面每次都要
     *   setStyleSheet() —— Qt 设样式表要重新解析 + repolish 整个控件, 是重量级操作;
     *   100 Hz 地刷"颜色/粗体"纯属浪费, 也是界面卡顿的一部分。
     *   所以: 样式表只在"状态真的变了"(未连接 / 正常 / 过流)时设一次;
     *         文字里的"已收 N 点"每帧都在变, 限成最多 200 ms 刷一次。
     */
    int     m_statusLastState  = -1;    ///< 上次设样式时的状态(-1 = 还没设过)
    QString m_statusLastText;           ///< 上次设进去的文字
    qint64  m_statusLastTextMs = 0;     ///< 上次设文字的时刻(ms)

    QByteArray m_rxBuffer;

    /*!
     * 制动开关(点"启动制动"翻上去, 点"关闭制动"翻回来)。
     * 只表示**上位机这一侧**的意图 —— 界面上那两个圆点才是板子回上来的真实状态。
     */
    bool m_brakeOn = false;

    /*! 光幕(0xB0)上报: 本次连接里收到过没有 + 最后一次的遮挡掩码(bit i = 第 i+1 路)。 */
    bool m_lsSeen = false;
    int  m_lsMask = 0;

    /*!
     * 按键(0xAA)上报: 本次连接里收到过没有 + 最后一次的按键掩码。
     *   bit0 = KEY1 按着(板子马上软复位) / bit1 = KEY2 按着(板子马上重启 TCP 服务端)。
     * 和光幕一样"同一状态重复到达就不重复写日志"。重连时一起清(resetBinaryState),
     * 这样重连后板子推的第一条(通常是 0x00 都松开)一定会被记下来。
     */
    bool m_keySeen = false;
    int  m_keyMask = 0;

    /*! 过流(0xC0)上报: 本次连接里收到过就锁存(和 m_ocLatched 一个规矩, 重连才清)。 */
    bool m_ocAlarm = false;

    /*!
     * "红点至少亮够这么久"(ms) + 两个配套变量。
     *
     * ⚠ 为什么上位机也要兜这一道(下位机那边已经用保持窗口兜了):
     *   真实过流时 PL 硬件一钳位, 电流瞬间消失, oc_n 只低**几微秒** —— 如果下位机
     *   (或任何一版固件)把"出现"和"恢复"两条报文几乎同时发上来, 界面就会"红了 5 ms
     *   又立刻变绿", 人眼看到的结果是**从来没红过**。所以这里记下"开始红"的时刻,
     *   收到"恢复"时若还没亮够 kOcMinRedMs 就**推迟**变绿(期间又来告警则取消推迟)。
     *   ⚠ 这只是显示层的下限, 不改变语义: 下位机说恢复, 界面最终一定跟着恢复。
     */
    qint64  m_ocRedStartMs = 0;      ///< 本轮红点开始亮的时刻(ms)
    quint64 m_ocSeq        = 0;      ///< 告警轮次: 推迟变绿时用它判断"期间是否又报警了"

    /*! 制动按钮下面那两个圆形状态图标: 左 = 小车运行位置, 右 = 系统状态。 */
    QLabel *m_iconPosition = nullptr;
    QLabel *m_iconSystem   = nullptr;

    /*! 右圆下面那行说明小字: 平时是"系统状态", 过保时改成"过保: 1,3"(点名哪几路)。 */
    QLabel *m_iconSystemCaption = nullptr;

    bool       m_acquiring   = false;
    qint64     m_sampleIndex = 0;
    quint64    m_rxLines     = 0;
    quint64    m_txCommands  = 0;

    /*! 占空比滑块的下发节流: 拖动时最多每 kDutySendGapMs 发一帧, 松手再补一帧最终值。 */
    qint64     m_dutyLastSentMs = 0;   ///< 上次发占空比帧的时刻(ms)
    int        m_dutyLastSent   = -1;  ///< 上次发出去的占空比(去重用)

    /*! 占空比 0 ⇒ 死区必须 0: 这里记住"占空比非 0 时用的死区", 回到非 0 时恢复。 */
    int        m_deadNsRemembered = 500;

    /*! 电流指令滑块的去重/节流(和占空比滑块同一套路)。 */
    qint64     m_curLastSentMs = 0;
    int        m_curLastSent   = -1;

    /*! 点"开始采集"时如果还没连接: 连上之后自动开始采集(不想让用户点两次)。 */
    bool m_acquireAfterConnect = false;

    /*!
     * (原来这里是 eMMC 回放的分页状态: 板子一次回一批(EMCDUMP <start> <count>),
     *  上位机看到 more=1 就继续发下一条, 直到 more=0。)
     *  ⚠ 2026-10-16: eMMC 测试栏与那一套命令整体删除, 这几个成员也一起没了 ——
     *  要看卡上的数据现在直接读卡(data.csv), 不再走网口回放。
     */

    double m_demoT   = 0.0;
    double m_demoTau = 2.0;

    QVector<Sample> m_cache;
};

#endif // WIDGET_H
