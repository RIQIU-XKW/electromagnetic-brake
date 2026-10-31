#include "widget.h"
#include "./ui_widget.h"

#include "ledindicator.h"
#include "plotwidget.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFont>
#include <QHash>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QKeyEvent>
#include <QLabel>
#include <QPixmap>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSignalBlocker>
#include <QTcpSocket>
#include <QTextStream>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace {

/* 日志配色 */
const QColor kColorDefault(0x20, 0x20, 0x20);
const QColor kColorTx     (0x15, 0x54, 0x9C);   // 发出去的
const QColor kColorRx     (0x20, 0x20, 0x20);   // 收到的
const QColor kColorOk     (0x11, 0x7A, 0x2B);   // ok / pass / pong
const QColor kColorErr    (0xB3, 0x1B, 0x1B);   // err / fault / fail
const QColor kColorInfo   (0x6A, 0x35, 0xA0);   // 板子主动广播
const QColor kColorGray   (0x77, 0x77, 0x77);   // 本地提示

/* 缓存区上限: 防止长时间运行把内存吃光 */
const int kMaxCacheRows = 500000;

/* ---- 单字节二进制协议: 下行命令码 ---------------------------------------- */
/*
 * ⚠ 这几个是**裸字节**, 不是文本行 —— 走 m_socket->write() 原样发出去, 不带 \r\n,
 *   也不做 "一条命令一个报文" 的文本封装(见 sendByte())。
 */
const unsigned char kBinBrakeEnable = 0xA0;   // 制动使能(开全局调制, 桥仍关着)
const unsigned char kBinBrakeStart  = 0xA1;   // 启动制动(五路桥同时打开, 用当前占空比)
const unsigned char kBinBrakeLevelLow  = 0xA2;  // 预设占空比 低档 30%
const unsigned char kBinBrakeLevelMid  = 0xA3;  // 预设占空比 中档 50%
const unsigned char kBinBrakeLevelHigh = 0xA4;  // 预设占空比 高档 70%(= 上限)
const unsigned char kBinBrakeStop   = 0xA5;   // 停止制动(五桥全关 + 关全局调制)

/*
 * 制动参数帧(4 字节, 必须同一次 write):
 *   [0xDA][高字节][低字节][0xDB]  占空比 0..70(%)   —— 五路桥同步
 *   [0xDC][高字节][低字节][0xDD]  死区   0 或 100..10000(ns)
 */
const unsigned char kBinBrakeDutyHead = 0xDA;
const unsigned char kBinBrakeDutyTail = 0xDB;
const unsigned char kBinBrakeDeadHead = 0xDC;
const unsigned char kBinBrakeDeadTail = 0xDD;

/** 拖动占空比滑块时的最小下发间隔(ms), 防止一次拖动刷爆 TCP。 */
const int kDutySendGapMs = 50;

/** 占空比上限(%): 现场限制不超过 70, 上位机和下位机都按这个卡。 */
const int kDutyMaxPercent = 70;

/** 斩波时死区允许的最小值(ns): 1..99 一律夹到 100(小于 100 等于没插死区, 有直通风险)。 */
const int kDeadMinNs = 100;

/*
 * Mover(发射小车, 下位机 app/Mover_app.c 那边叫 0xD0..0xD6):
 *   0xD0            = 发射(用下位机**当前档位**)
 *   0xD1 .. 0xD6    = 选 SS1..SS6 档 **并立刻发射**(0xD1 = 最慢, 0xD6 = 最快)
 * ⚠ 0xD1..0xD6 和"内圈的六档速度"是同一件事: 选档会记住, 之后 0xD0 就用最后选的档。
 */
const unsigned char kBinMoverLaunch = 0xD0;   // 发射小车(当前档位)
const unsigned char kBinMoverGear1  = 0xD1;   // SS1 + 发射(0xD6 = SS6)

/*
 * Samp(采集, 下位机 app/samp_app.c 那边是 0xE0..0xE2):
 *   0xE0 = 开始采样(板子每 10 ms 扫一轮 ADC + 每拍回一行 DATA; 同时开始录卡)
 *   0xE1 = 停止采样(任务停下: 不发 SPI、不广播、也不写卡)
 *   0xE2 = 查状态(回一行文本)
 * ⚠ 遥测 DATA 行**仍然是文本**, 没改成二进制 —— 上位机解析照旧。
 * ⚠ eMMC 那一套命令(EMCSTAT/EMCSTART/EMCSTOP/EMCCLR/EMCDUMP/EMCMOUNT)连同上位机的
 *   eMMC 测试栏 2026-10-16 一起删了: 录制现在是板子自动做的, 要看数据直接读卡。
 */
const unsigned char kBinSampStart  = 0xE0;
const unsigned char kBinSampStop   = 0xE1;
const unsigned char kBinSampZero   = 0xE3;   // 电流零点校准(采当前电压当零电流电压)

/*
 * 电流指令帧(4 字节, 必须同一次 write):
 *   [0xBA][数据低字节][数据高字节][0xAB]   数据 = 目标电流(mA), 0..15000
 * 用 0xBA 当帧头是为了避开 0xAA(那是"查按键状态"的单字节命令)。
 */
const unsigned char kBinCurrentHead = 0xBA;
const unsigned char kBinCurrentTail = 0xAB;

/** 电流指令电流上限(mA) = 15 A。 */
const int kCurrentMaxMa = 15000;

/** 拖动电流滑块时的最小下发间隔(ms)。 */
const int kCurrentSendGapMs = 50;
const unsigned char kBinSampStatus = 0xE2;

/*
 * 按键(下位机 app/inter_app.c 那边是 0xAA..0xAC):
 *   0xAA = 查按键状态(回一行文本)
 *   0xAB = 软复位(= 按板子上的 KEY1; 板子会重启, 连接断)
 *   0xAC = 重启 TCP 服务端(= 按 KEY2; 客户端掉线后重连)
 * ⚠ 按键**主要不是靠命令工作的**: 板子上的 key 任务自己轮询, 按下就是本地动作, 而且
 *   会**主动上报** [0xAA][掩码](见 kBinKeyReport)。这三条是"够不着板子"时的替代手段。
 * ⚠ 2026-10-16: 文本命令 KEYSTAT/KEYRESET/KEYNET 已删。
 */
const unsigned char kBinKeyStatus = 0xAA;   // 查按键状态
const unsigned char kBinKeyReset  = 0xAB;   // 软复位(KEY1 的动作)
const unsigned char kBinKeyNet    = 0xAC;   // 重启 TCP 服务端(KEY2 的动作)

/*
 * 呼吸灯(下位机 app/monitor_app.c 那边是 0xF0..0xFF):
 *   0xF0            = 四路全灭(顺带把"网口在线"的 LED0 也灭掉)
 *   0xF1/0xF2/0xF3  = 慢 / 标准 / 快呼吸(老 RS-485 的 LED_STA_0..3)
 *   0xF4            = 查状态(回一行文本, 界面据此回填勾选框和频率)
 *   0xF5            = 四路全亮
 *   0xF6..0xF9      = 单独点亮 LED0..LED3     0xFA..0xFD = 单独熄灭 LED0..LED3
 *   0xFE <频率/10>  = 设共享频率(1..100 = 10..1000 Hz, 正好等于 SpinBox 的步进)
 *   0xFF <掩码>     = 设四路掩码(0..15)
 * ⚠ 0xFE / 0xFF 是**两字节**命令: 参数必须和命令在同一次 write 里发出(见 sendBytes)。
 * ⚠ 2026-10-16: 文本命令 LEDSTAT/LEDMASK/LEDON/LEDOFF/LEDFREQ 已删。
 */
const unsigned char kBinLedAllOff = 0xF0;   // 四路全灭
const unsigned char kBinLedSlow   = 0xF1;   // 慢呼吸
const unsigned char kBinLedNormal = 0xF2;   // 标准呼吸
const unsigned char kBinLedFast   = 0xF3;   // 快呼吸
const unsigned char kBinLedStatus = 0xF4;   // 查状态
const unsigned char kBinLedAllOn  = 0xF5;   // 四路全亮
const unsigned char kBinLedOn1    = 0xF6;   // 单独点亮 LED0(0xF9 = LED3)
const unsigned char kBinLedOff1   = 0xFA;   // 单独熄灭 LED0(0xFD = LED3)
const unsigned char kBinLedFreq   = 0xFE;   // 后面跟 1 字节: 频率/10
const unsigned char kBinLedMask   = 0xFF;   // 后面跟 1 字节: 四路掩码

/*!
 * 0xFE 的参数字节 = 频率 / 这个数(板子那边乘回来就是 Hz)。
 * 10 正好等于界面上 SpinBox 的 singleStep, 所以 10..1000 Hz 是无损换算, 不会出现
 * "界面上是 250, 下发完变成 250 但板子记成 25"这种对不上的事。
 */
const int kLedFreqUnit = 10;

/* ---- 单字节二进制协议: 上行报文的帧头 ------------------------------------ */
const unsigned char kBinLsReport  = 0xB0;     // [0xB0][mask] 光幕(LS)状态; bit i = 第 i+1 路被遮挡
const unsigned char kBinOcReport  = 0xC0;     // [0xC0][src]  过流(OC)告警; bit i = 第 i+1 个桥过流
const unsigned char kBinKeyReport = 0xAA;     // [0xAA][mask] 按键状态; bit0 = KEY1 按着, bit1 = KEY2 按着

/*! 圆形状态图标的直径(px)。border-radius 取一半就成了正圆, 和字体一起算好尺寸。 */
const int kIconDiameter = 78;

/*!
 * 遥测切分用的**共用**正则(空白分隔)。
 *
 * ⚠ 原来是每次解析都现构造一个 `QRegularExpression("\\s+")`: 构造 + 编译一次模式
 *   要几十微秒, 而遥测是 100 Hz 来的, 积少成多也不小(而且完全没必要, 模式恒定)。
 *   QRegularExpression 本身可以复用(它是隐式共享的, 匹配用 const 方法, 线程安全)。
 */
const QRegularExpression &splitWhitespace()
{
    static const QRegularExpression re(QStringLiteral("\\s+"));
    return re;
}

/*!
 * 过流红点**至少亮多久**(ms) —— 真实过流的 oc_n 只低几微秒, 下位机两条报文可能
 * 挨着上来("出现"紧跟"恢复"), 严格照做就是"红了 5 ms 立刻变绿", 人眼等于没红。
 * 所以"恢复"报文若来得太早, 界面推迟到亮满这个时长再变绿(见 handleBinaryFrame)。
 * ⚠ 只影响显示时长, 不改变语义: 下位机说恢复, 界面最终一定跟着恢复。
 */
const int kOcMinRedMs = 1500;

/*!
 * 掩码里**最高**置位的 bit 序号(bit0 → 0), 全 0 返回 -1。
 *
 * ⚠ 光幕是"一竖排"装的, mask=0x03 表示第 1、2 路都被挡 —— 小车挡住的总是最上面
 *   (编号最大)的那一路, 所以按最高位置显示"线圈 N", 而不是最低位。
 */
int highestSetBit(int mask)
{
    for (int i = 7; i >= 0; --i) {          // 掩码只有一个字节
        if (mask & (1 << i))
            return i;
    }
    return -1;
}

/*!
 * 造一个圆形图标(用 QLabel + stylesheet 的 border-radius, 不引第三方依赖)。
 *
 * ⚠ setFixedSize 是必须的: border-radius 只有在宽高确定时才能算出正圆,
 *   丢进布局里让 Qt 自己伸缩的话会变成一个圆角方块。
 */
QLabel *makeCircleIcon(QWidget *parent)
{
    QLabel *circle = new QLabel(parent);
    circle->setFixedSize(kIconDiameter, kIconDiameter);
    circle->setAlignment(Qt::AlignCenter);
    circle->setWordWrap(true);
    return circle;
}

/*! 圆形图标的配色/字体: 底色 + 白字 + 描边, 半径正好是直径的一半。 */
QString circleStyle(const char *background, const char *border)
{
    return QStringLiteral("QLabel{background-color:%1;color:#FFFFFF;border:2px solid %2;"
                          "border-radius:%3px;font-size:11pt;font-weight:bold;}")
        .arg(QString::fromLatin1(background),
             QString::fromLatin1(border))
        .arg(kIconDiameter / 2);
}

/*!
 * 圆点 + 下面一行小字说明, 竖着排成一列(横向并排两列 = 两个圆点)。
 *
 * @param outCaption : 非空时把那一行小字的 QLabel 交回去, 调用者以后可以改它的
 *                     文字/配色(过流那种"要显示哪一路"的场景就靠它)。
 */
QLayout *makeIconColumn(QLabel *circle, const QString &caption, QLabel **outCaption = nullptr)
{
    QVBoxLayout *column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);

    column->addWidget(circle, 0, Qt::AlignHCenter);

    QLabel *text = new QLabel(caption);
    text->setAlignment(Qt::AlignHCenter);
    text->setStyleSheet(QStringLiteral("QLabel{color:#5A5A5A;font-size:9pt;}"));
    column->addWidget(text, 0, Qt::AlignHCenter);

    if (outCaption != nullptr) {
        *outCaption = text;
    }

    return column;
}

/*! 过流源掩码 → "1,3" 这种桥号列表(src 的 bit i = 第 i+1 个桥过流)。 */
QString ocSourceText(int src)
{
    QStringList bridges;
    for (int i = 0; i < 8; ++i) {
        if (src & (1 << i))
            bridges << QString::number(i + 1);
    }

    return bridges.isEmpty() ? QStringLiteral("-") : bridges.join(QLatin1Char(','));
}

/*!
 * 一个字节 → "0xA0" 这种写法(数字大写、前缀 x 保持小写, 和工程里其它地方一致)。
 *
 * ⚠ 别图省事写成 `QStringLiteral("0x%1").arg(...).toUpper()`: toUpper() 会把**整条
 *   消息**都变大写 —— 日志会变成 "MASK=0X05" / ">> 0XA0 (制动整体使能)" 这种读着别扭
 *   的样子(试过, 截图里一看就发现了)。所以只把数字部分大写。
 */
QString byteHex(int value)
{
    const QString digits = QString::number(uint(value) & 0xFFu, 16)
                               .rightJustified(2, QLatin1Char('0'))
                               .toUpper();
    return QStringLiteral("0x") + digits;
}

/*!
 * 一串字节 → "0xFE 0x0A" 这种写法(给两字节命令记日志用)。
 *
 * 单字节的走 byteHex(), 保持 "[0xA0] (说明)" 那种一眼就能认出来的老样子;
 * 只有真的多于一个字节时才用这个, 免得每条日志都变成 "0xA0" 之外的形状。
 */
QString byteHexList(const QByteArray &bytes)
{
    QStringList parts;

    for (const char b : bytes)
        parts << byteHex(int(static_cast<unsigned char>(b)));

    return parts.join(QLatin1Char(' '));
}

/* ⚠ 这里原来有个 brakingLevelToDuty(): 把"制动强度"下拉换成占空比千分比, 再发文本命令
 *   PWMDUTY 给下位机。现在强度由 0xA2/0xA3/0xA4 三个字节直接表达, 档位对应的最大占空比
 *   (30/50/70%)是下位机 brake_ctl_app 里定死的, 上位机不再需要这个换算 —— 整块删掉了。 */

/*
 * 发射速度下拉 → mm/s。
 * 六档 0.5~1.0 m/s 就是原 STM32 工程 control_car() 里的 SS1..SS6:
 *   SS1 = 0.5 m/s -> 0x0007A120(500000) ... SS6 = 1.0 m/s -> 0x000F4240(1000000)
 * 0.5 m/s = 500 mm/s, 而下位机里 0x6081 的单位是 1 mm/s = 1000, 所以发 500
 * 算出来正好是 500000 —— 和原工程的帧逐字节相同。
 */
int launchIndexToMmps(int index)
{
    return 500 + (index * 100);
}

/*!
 * 把一个 token 当数值解析。
 * 支持 "123" / "-12.5" / "147.3mm/s" 这类带单位的写法;
 * 十六进制(0x..)一律不认, 免得把 "0x3F" 读成 0。
 */
bool tokenToDouble(const QString &token, double *out)
{
    const QString s = token.trimmed();
    if (s.isEmpty() || out == nullptr)
        return false;

    if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        return false;

    int i = 0;
    if (s.at(i) == QLatin1Char('+') || s.at(i) == QLatin1Char('-'))
        ++i;

    const int start = i;
    while (i < s.size()) {
        const QChar c = s.at(i);
        if (c.isDigit() || c == QLatin1Char('.')) {
            ++i;
        } else if ((c == QLatin1Char('e') || c == QLatin1Char('E')) && i > start) {
            ++i;
            if (i < s.size() && (s.at(i) == QLatin1Char('+') || s.at(i) == QLatin1Char('-')))
                ++i;
        } else {
            break;
        }
    }

    if (i == start)
        return false;

    bool ok = false;
    const double value = s.mid(0, i).toDouble(&ok);
    if (!ok)
        return false;

    *out = value;
    return true;
}

/*! 整行只有数字/符号/空白分隔符 → 当作裸数据行。 */
bool isPureNumericLine(const QString &line)
{
    bool hasDigit = false;

    for (const QChar &c : line) {
        if (c.isDigit()) {
            hasDigit = true;
        } else if (c.isSpace() || c == QLatin1Char(',') || c == QLatin1Char(';')
                   || c == QLatin1Char(':') || c == QLatin1Char('+') || c == QLatin1Char('-')
                   || c == QLatin1Char('.') || c == QLatin1Char('[') || c == QLatin1Char(']')) {
            continue;
        } else {
            return false;
        }
    }

    return hasDigit;
}

/*! 去掉行首的 [tag] 前缀(板子的日志/广播都带这个)。 */
QString stripLeadingTags(const QString &line)
{
    QString s = line.trimmed();

    while (s.startsWith(QLatin1Char('['))) {
        const int end = s.indexOf(QLatin1Char(']'));
        if (end <= 0 || end > 24)
            break;

        // "[zynq] LS 0x01" → 去掉标签; 但 "blocked[0]" 这种在中间的不管
        s = s.mid(end + 1).trimmed();
    }

    return s;
}

QSettings appSettings()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("Zynq_Base_Platform"),
                     QStringLiteral("Electromagnetic_braking"));
}

} // namespace

/* ========================================================================== */
/*  构造 / 析构                                                                */
/* ========================================================================== */

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
{
    ui->setupUi(this);

    m_socket = new QTcpSocket(this);
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);   // TCP_NODELAY

    m_demoTimer = new QTimer(this);
    m_demoTimer->setTimerType(Qt::PreciseTimer);

    setupUiExtras();
    connectSignals();
    loadSettings();

    /*
     * ⚠ 这一行带上**编译时间**(编译器内置的 __DATE__/__TIME__, 不是我手写的常量):
     *   现场排查"我改了上位机但没生效"这类问题时, 一眼就能确认现在跑的 exe 是哪一版 ——
     *   不用再去比文件时间戳。改完源码重新编译, 这行自然就变了。
     */
    appendLog(QStringLiteral("电磁制动试验平台 v1.0 —— 网口版(串口已换成 TCP)"
                             "  [本机编译于 " __DATE__ " " __TIME__ "]"),
              kColorDefault);
    appendLog(QStringLiteral("目标: %1:%2 ; 一条命令一个 TCP 报文, 以 \\r\\n 结尾")
                  .arg(ui->lineEditIp->text())
                  .arg(ui->spinBoxPort->value()),
              kColorGray);
    appendLog(QStringLiteral("点\"开始采集\"连接板子; 没有硬件时可在\"控制参数预设\"里勾选本地模拟数据。"),
              kColorGray);
}

Widget::~Widget()
{
    delete ui;
}

void Widget::setupUiExtras()
{
    /*
     * 启动永远停在第一页("实时采集数据")。
     *
     * 为什么不只靠 .ui 里的 currentIndex: 用 Qt Creator 的 Designer 打开这个 .ui
     * 时, 它把"你当时正在看的那一页"当成 currentIndex 存回去 —— 在第二页上保存
     * 一次, 程序下次启动就变成了开在"控制参数预设", 看着像"发射控件不在第一页"。
     * (2026 年就踩过这一次。) 所以这里显式再设一遍。
     */
    ui->tabWidget->setCurrentIndex(0);

    /* ---- 左侧上下分栏: 接收区 : 曲线 ≈ 1 : 2 ----------------------------- */
    ui->splitterDisplay->setStretchFactor(0, 1);
    ui->splitterDisplay->setStretchFactor(1, 2);
    ui->splitterDisplay->setSizes(QList<int>() << 170 << 360);

    /* ---- 接收区用等宽字体, 方便看对齐 ------------------------------------ */
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(9);
    ui->plainTextRecv->setFont(mono);
    ui->plainTextRecv->setPlaceholderText(
        QStringLiteral("网口接收区: 板子回的每一行都会显示在这里(命令回显以 >> 开头)"));

    /* ---- 曲线通道: 左图 = 电气量, 右图 = 动子 ------------------------------ */
    /*
     * 名字/颜色只在这里登记一次: 图例、第一页勾选框的文字颜色、CSV 表头(走 curveName())
     * 三处都从这里出来, 以后改颜色不会三处不一致。
     *
     * 顺序必须和 CurveId 一致 —— 板子的 DATA 行就是按这个顺序给的数
     * (第 1 个是时间, 后面依次 线圈1..5 电流 / 动子速度 / 母线电压 / 动子位移)。
     *
     * 图怎么分(2026/11): **左右两张图**, 各画各的量级 ——
     *   左 plotWidget      : 线圈 1~5 电流(A) + 母线电压(V)
     *   右 plotWidgetMover : 动子速度(mm/s) + 动子位移(mm)
     * 分图的原因: 一张图只有一根纵轴, 位移几百 mm 和电流十几 A 画在一起, 自动缩放会把
     * 小的那条压成一条直线。归属表是 m_curvePlot[]/m_curveLocal[](见 widget.h)。
     *
     * !! 前 5 条是**线圈电流(A)**: 下位机在 service_ads 里按 I = (1.5*V - 2.5)*10
     *    把 ADS8688 采到的引脚电压换算成电流再发, 上位机直接用, 不再折一次。
     *    "母线电压"是**母线电压(V)**: 下位机已按 11:1 分压比还原。
     *    "动子速度"是**速度(mm/s)**: 下位机做状态机 + 差分测速, 丢失目标时发 0。
     *    "动子位移"是**位移(mm)**: 下位机按两点标定 x = x1 + (x2-x1)/(y2-y1)*(V-y1) 折好再发。
     */
    static const struct { const char *name; const char *color; const char *unit; } kCurves[CurveCount] = {
        { "线圈1电流", "#1F6FEB", "A" },
        { "线圈2电流", "#D29922", "A" },
        { "线圈3电流", "#2DA44E", "A" },
        { "线圈4电流", "#8250DF", "A" },
        { "线圈5电流", "#1B7C83", "A" },
        { "动子速度",  "#CF222E", "mm/s" },
        { "母线电压",  "#B35900", "V" },
        { "动子位移",  "#0969DA", "mm" },
    };

    /*
     * 登记曲线: 每条曲线交给它所属的那张图, 并按**到达顺序**分配局部序号。
     * 这样 m_curveLocal[] 就不用和哪张图有几条曲线手工对表了(加曲线/换图只改 kCurves
     * 那一行 + widget.h 里的 m_curvePlot[], 序号自动算)。
     */
    {
        int nextLocal[PlotCount] = { 0, 0 };

        for (int i = 0; i < CurveCount; ++i) {
            PlotWidget *target = plot(m_curvePlot[i]);

            m_curveLocal[i] = nextLocal[m_curvePlot[i]]++;
            target->addCurve(QString::fromUtf8(kCurves[i].name), QColor(kCurves[i].color),
                             QString::fromLatin1(kCurves[i].unit));
        }
    }

    /* 勾选框的文字用对应曲线的颜色, 一眼就能对上是哪条线 */
    const QList<QCheckBox *> boxes = curveBoxes();
    for (int i = 0; i < boxes.size() && i < CurveCount; ++i) {
        boxes.at(i)->setStyleSheet(QStringLiteral("QCheckBox{color:%1;}")
                                       .arg(QString::fromLatin1(kCurves[i].color)));
    }

    /* 两张图共用横轴标题/窗口宽度; 纵轴各自自动缩放(量级差太远), 手动区间两边都给 */
    for (int p = 0; p < PlotCount; ++p) {
        PlotWidget *w = plot(PlotId(p));

        w->setAxisTitles(ui->lineEditXTitle->text());
        w->setYRange(ui->doubleSpinBoxYMin->value(), ui->doubleSpinBoxYMax->value());
        w->setXSpan(ui->spinBoxXSpan->value());
        w->setAutoScaleY(ui->checkBoxAutoScaleY->isChecked());
    }

    /* 左右各占一半(可以用分隔条再拖) */
    ui->splitterPlots->setStretchFactor(0, 1);
    ui->splitterPlots->setStretchFactor(1, 1);

    /* ---- IP 输入框只收 IPv4 ---------------------------------------------- */
    ui->lineEditIp->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^(\\d{1,3}\\.){3}\\d{1,3}$")),
        this));

    /* ---- 制动强度: 低/中/高 三档, 选中就发对应的自定义命令 ----------------- */
    connect(ui->comboBoxLevel, &QComboBox::currentIndexChanged,
            this, &Widget::onBrakingLevelChanged);

    /* ⚠ 不再"开机就发一次": A2/A3/A4 只是**改档位**, 下位机在 0xA1 启动时序时才会用到
     *   当前档位; 上电/刚连上就发字节没意义, 而且会让下位机日志里多一行无谓的记录。
     *   默认档位由下位机自己定(低档 30%), 界面显示的和它一致(index 0)。 */

    /* ---- 制动按钮下面的两个圆形状态图标(光幕位置 / 系统状态) -------------- */
    setupStatusIcons();

    /* ---- 顶部左上角的校徽(和标题同一行, 不额外占竖直空间) ------------------ */
    setupTopLogo();

    applyButtonStyle();
}

/*!
 * 在**顶部左上角**放校徽(海军工程大学), 和"电磁制动试验平台"标题同一行。
 *
 * ⚠ 为什么不是在 .ui 里加一个 QLabel:
 *   那样会**多出一整行**(标题被挤下去, 界面整体下移); 这里把 .ui 里的标题从竖直布局里
 *   取出来, 和校徽一起放进一个新的水平行 —— 竖直方向占用不变, 校徽正好落在窗口左上角。
 *   (和在制动按钮下面插两个圆形图标是同一个做法: 少动 .ui, Designer 来回保存也不会挤掉。)
 *
 * ⚠ 图片走 **Qt 资源**(:/branding/logo.png, 见 resources.qrc): 编进 exe, 不依赖旁边有没有
 *   图片文件、也不依赖 imageformats 插件(JPEG 要插件, PNG 是 QtGui 内置的)。
 *
 * ⚠ 高度是**逻辑像素**(kLogoHeight), 用 SmoothTransformation 缩放; 嵌入的 png 是它的 3 倍
 *   (216 px), 所以在 150% 缩放的高分屏上也清楚。图片本身按宽高比缩, 不会拉变形。
 */
void Widget::setupTopLogo()
{
    /* 显示高度(逻辑像素)。想更大/更小改这一个数就行(宽度按比例自己算)。 */
    constexpr int kLogoHeight = 72;

    const QPixmap logo(QStringLiteral(":/branding/logo.png"));
    if (logo.isNull()) {
        /* 资源丢了也不至于把界面弄乱: 保持原来的样子, 只是没有校徽 */
        appendLog(QStringLiteral("× 找不到校徽资源(:/branding/logo.png), 顶部留空"), kColorErr);
        return;
    }

    QLabel *logoLabel = new QLabel(this);
    logoLabel->setObjectName(QStringLiteral("labelLogo"));
    logoLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    logoLabel->setToolTip(QStringLiteral("海军工程大学 (Naval University of Engineering)"));

    /*
     * 校徽本身是**透明底**的 PNG(见生成脚本/资源说明): 底色不匹配的问题就是这么解决的 ——
     * 透明底放在任何界面底色上都不会出现"一块方框"。
     */
    const QPixmap scaled = logo.scaledToHeight(kLogoHeight, Qt::SmoothTransformation);
    logoLabel->setPixmap(scaled);

    /* 把标题从竖直布局里摘出来, 和校徽放进同一个水平行 */
    ui->verticalLayoutMain->removeWidget(ui->labelTitle);

    QWidget     *titleRow = new QWidget(this);
    QHBoxLayout *row      = new QHBoxLayout(titleRow);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);

    row->addWidget(logoLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
    row->addWidget(ui->labelTitle, 1);

    /*
     * ⚠ 空档必须加在**标题右边**(和"校徽 + 间距"等宽), 不能加在校徽和标题之间:
     *   加在中间等于把标题往右推 —— 标题的中心就跑到窗口中心的右边去了(现场就是这个问题);
     *   加在右边, 标题的可用区间左右对称, 它才真的居中。
     */
    row->addSpacing(scaled.width() + row->spacing());

    ui->verticalLayoutMain->insertWidget(0, titleRow);
}

void Widget::applyButtonStyle()
{
    /* "开始采集/停止采集"是开关, 样式跟着状态走, 见 updateStartButton() */
    updateStartButton();

    /*
     * 底部那个红色"停止"和上面的"停止采集"**不是一回事**, 在提示里写清楚,
     * 免得想冻住曲线的人按了它把网口一起断了(或者反过来)。
     */
    ui->pushButtonStop->setToolTip(QStringLiteral(
        "停止一切: 关 PWM 输出(PWMSTOP) + 停止采集 + 断开网口 + 导出 CSV。\n"
        "只想冻住曲线看数, 用上面的\"停止采集\"。"));

    ui->pushButtonLaunchCar->setStyleSheet(QStringLiteral(
        "QPushButton{background-color:#2BE02B;border:1px solid #1E7A22;border-radius:3px;"
        "padding:4px 6px;font-weight:bold;}"
        "QPushButton:hover{background-color:#5CEB5C;}"
        "QPushButton:pressed{background-color:#18C418;}"));

    /*
     * "启动制动/关闭制动"是开关, 文字和样式都跟着状态走 —— 和上面的"开始采集"一样,
     * 统一放在 updateBrakeButton() 里出, 别在 .ui 和这里两处各写一份。
     */
    updateBrakeButton();

    ui->pushButtonStop->setStyleSheet(QStringLiteral(
        "QPushButton{background-color:#F2D5D5;border:1px solid #A33;border-radius:3px;padding:4px 6px;}"
        "QPushButton:hover{background-color:#F7E3E3;}"
        "QPushButton:pressed{background-color:#E4B9B9;}"));

    /* ⚠ 这里原来还给预设页那个"紧急停止 PWMSTOP"按钮上红色样式 —— 那个按钮随 PWM 模块
     *   一起删了(下位机没有 PWMSTOP 命令了, 紧急停止现在就是制动按钮的"关闭制动"0xA5)。 */

    updateConnectButton(false);
}

void Widget::updateConnectButton(bool connected)
{
    if (connected) {
        ui->pushButtonConnect->setText(QStringLiteral("断开连接"));
        ui->pushButtonConnect->setStyleSheet(QStringLiteral(
            "QPushButton{background-color:#F2D5D5;border:1px solid #A33;border-radius:3px;"
            "padding:4px 6px;font-weight:bold;}"
            "QPushButton:hover{background-color:#F7E3E3;}"));
    } else {
        ui->pushButtonConnect->setText(QStringLiteral("连接网口"));
        ui->pushButtonConnect->setStyleSheet(QStringLiteral(
            "QPushButton{background-color:#D6E4F0;border:1px solid #4A7EB0;border-radius:3px;"
            "padding:4px 6px;font-weight:bold;}"
            "QPushButton:hover{background-color:#E4EEF8;}"));
    }
}

void Widget::connectSignals()
{
    /* ---- 网口 ------------------------------------------------------------ */
    connect(m_socket, &QTcpSocket::connected,    this, &Widget::onSocketConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &Widget::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,    this, &Widget::onReadyRead);
    connect(m_socket, &QAbstractSocket::errorOccurred, this, &Widget::onSocketErrorOccurred);

    /* ---- 右侧按钮 -------------------------------------------------------- */
    connect(ui->pushButtonConnect,  &QPushButton::clicked, this, &Widget::onConnectButton);
    connect(ui->pushButtonStart,    &QPushButton::clicked, this, &Widget::onStartAcquire);
    connect(ui->pushButtonLaunchCar, &QPushButton::clicked, this, &Widget::onLaunchCar);
    connect(ui->pushButtonBrake,    &QPushButton::clicked, this, &Widget::onBrake);
    connect(ui->pushButtonSnapshot, &QPushButton::clicked, this, &Widget::onSnapshot);
    connect(ui->pushButtonClear,    &QPushButton::clicked, this, &Widget::onClearCache);
    connect(ui->pushButtonStop,     &QPushButton::clicked, this, &Widget::onStopAll);


    /* ---- 预设页: 4 路呼吸灯 ---------------------------------------------- */
    connect(ui->pushButtonLedApply, &QPushButton::clicked, this, &Widget::onApplyLed);
    connect(ui->pushButtonLedAllOn, &QPushButton::clicked, this, &Widget::onLedAllOn);
    connect(ui->pushButtonLedAllOff,&QPushButton::clicked, this, &Widget::onLedAllOff);
    connect(ui->pushButtonLedStat,  &QPushButton::clicked, this, &Widget::reloadLedFromBoard);


    /* ---- 预设页: 本地模拟 / 显示设置 / 预设方案 --------------------------- */
    connect(ui->checkBoxDemo, &QCheckBox::toggled, this, &Widget::onDemoToggled);
    connect(m_demoTimer, &QTimer::timeout, this, &Widget::onDemoTick);

    connect(ui->pushButtonApplyDisplay, &QPushButton::clicked, this, &Widget::onApplyDisplaySettings);

    /* ---- 控制参数预设页: 制动占空比滑块 + 死区 ------------------------- */
    connect(ui->sliderBrakeDuty, &QSlider::valueChanged, this, &Widget::onBrakeDutySliderMoved);
    connect(ui->sliderBrakeDuty, &QSlider::sliderReleased, this, &Widget::onBrakeDutySliderReleased);
    connect(ui->pushButtonApplyBrakeDead, &QPushButton::clicked, this, &Widget::onApplyBrakeDead);
    connect(ui->pushButtonBrakeZero, &QPushButton::clicked, this, &Widget::onCalibrateCurrentZero);

    /* ---- 电流指令: 滑块 0..15000 mA(0..15 A) --------------------------- */
    connect(ui->sliderCurrentCmd, &QSlider::valueChanged, this, &Widget::onCurrentSliderMoved);
    connect(ui->sliderCurrentCmd, &QSlider::sliderReleased, this, &Widget::onCurrentSliderReleased);
    connect(ui->pushButtonCurrentZero, &QPushButton::clicked, this, &Widget::onCurrentZeroClicked);

    /* ---- 第一页: 曲线的显示勾选框 ---------------------------------------- */
    const QList<QCheckBox *> boxes = curveBoxes();
    for (QCheckBox *box : boxes)
        connect(box, &QCheckBox::toggled, this, &Widget::syncCurveVisibility);

    /* ---- 显示设置页: 纵轴自动缩放 ---------------------------------------- */
    connect(ui->checkBoxAutoScaleY, &QCheckBox::toggled, this, &Widget::onAutoScaleYToggled);

    /* 两张图都会"接管"纵轴, 都要连(见 onAutoScaleYTakenOver 里的 sender() 判断) */
    for (int p = 0; p < PlotCount; ++p) {
        connect(plot(PlotId(p)), &PlotWidget::autoScaleYTakenOver,
                this, &Widget::onAutoScaleYTakenOver);

        /* Ctrl+双击某张图 = 铺满它 / 还原 */
        connect(plot(PlotId(p)), &PlotWidget::maximizeRequested,
                this, &Widget::onMaximizeRequested);
    }

    /* ---- 单图铺满: 每张图上面那个"铺满/还原"按钮 ------------------------- */
    connect(ui->pushButtonMaxLeft, &QPushButton::toggled, this, &Widget::onMaxLeftToggled);
    connect(ui->pushButtonMaxRight, &QPushButton::toggled, this, &Widget::onMaxRightToggled);
}

/* ========================================================================== */
/*  日志 / 发送                                                                */
/* ========================================================================== */

void Widget::appendLog(const QString &text, const QColor &color, bool withStamp)
{
    const QString stamp = withStamp ? QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz "))
                                    : QString();
    const QString html = QStringLiteral("<span style=\"color:%1;\">%2</span>")
                             .arg(color.name(), (stamp + text).toHtmlEscaped());

    ui->plainTextRecv->appendHtml(html);
}

bool Widget::isConnected() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

void Widget::sendCommand(const QString &command, bool logIt)
{
    const QString cmd = command.trimmed();
    if (cmd.isEmpty())
        return;

    if (!isConnected()) {
        appendLog(QStringLiteral("× 未连接, 命令未发送: %1").arg(cmd), kColorErr);
        return;
    }

    /*
     * 板子那边是 "一个 TCP 报文 = 一条命令"(整条报文精确比较), 所以这里必须
     * 一条命令一次 write, 并且打开 TCP_NODELAY, 不能攒着一起发。
     */
    m_socket->write(cmd.toUtf8() + "\r\n");
    m_socket->flush();
    ++m_txCommands;

    if (logIt)
        appendLog(QStringLiteral(">> %1").arg(cmd), kColorTx);
}

void Widget::sendSequence(const QStringList &commands, int gapMs)
{
    for (int i = 0; i < commands.size(); ++i) {
        const QString cmd = commands.at(i);
        QTimer::singleShot(i * gapMs, this, [this, cmd] { sendCommand(cmd); });
    }
}

/*!
 * 发一个**原始字节**(制动那套单字节协议: 0xA0 / 0xA1 / 0xA5 ...)。
 *
 * ⚠ 绝对不能用 sendCommand(): 那条路会 toUtf8() 再补 "\r\n", 板子收到的就是
 *   3 个字节("A0\r\n" 的文本), 而不是 1 个字节 0xA0。
 * ⚠ char(0xA0) 在 MinGW 上是**有符号**的(-96), 这里就要这个位模式: 只要
 *   sizeof(char)==1, 写进去线上的字节就是 0xA0。别改成数字字符串去"转义"。
 * ⚠ 每发一个字节都单独 write + flush: 下位机是**逐字节**解析这条流的, 而且
 *   0xA0(使能) 必须排在 0xA1(启动) 前面 —— 攒在一起发也得保证顺序和边界。
 */
void Widget::sendByte(unsigned char code, const QString &note)
{
    sendBytes(QByteArray(1, static_cast<char>(code)), note);
}

/*!
 * 发一串**原始字节** —— 给"命令 + 参数"那种两字节命令用(0xFE/0xFF: 频率、掩码)。
 *
 * ⚠ 参数必须和命令在**同一次 write** 里出去: 下位机是逐字节扫报文的, 0xFE/0xFF 会把
 *   紧跟其后的那个字节当成参数 —— 分两次 write 就有可能在板子那边被拆成两个报文,
 *   于是"命令到了、参数还没到", 板子只能回一句用法提示(它不会去猜, 见 monitor_app.c)。
 * ⚠ 相反地: 两条**独立**的字节命令(比如先频率后掩码)连着两次 write 是安全的 ——
 *   就算被 TCP 合并成一段, 下位机也是按字节流一条条处理的。
 */
void Widget::sendBytes(const QByteArray &bytes, const QString &note)
{
    if (bytes.isEmpty())
        return;

    if (!isConnected()) {
        appendLog(QStringLiteral("× 未连接, 字节 %1 未发送").arg(byteHexList(bytes)), kColorErr);
        return;
    }

    m_socket->write(bytes);
    m_socket->flush();
    ++m_txCommands;

    const QString hex = byteHexList(bytes);
    appendLog(note.isEmpty() ? QStringLiteral(">> %1").arg(hex)
                             : QStringLiteral(">> %1 (%2)").arg(hex, note),
              kColorTx);
}

/*!
 * 发占空比帧: **[0xDA][高字节][低字节][0xDB]**, 4 个字节必须在同一次 write 里。
 * @param percent : 0..kDutyMaxPercent(%), 下位机按 5 路 H 桥同步生效。
 *                  超过 70 一律夹到 70(下位机那边也会拒收 >70)。
 */
void Widget::sendBrakeDutyFrame(int percent)
{
    const int v = qBound(0, percent, kDutyMaxPercent);

    QByteArray f;
    f.append(static_cast<char>(kBinBrakeDutyHead));
    f.append(static_cast<char>((v >> 8) & 0xFF));
    f.append(static_cast<char>(v & 0xFF));
    f.append(static_cast<char>(kBinBrakeDutyTail));

    sendBytes(f, QStringLiteral("制动占空比 %1%").arg(v));
}

/*!
 * 发死区帧: **[0xDC][高字节][低字节][0xDD]**, 单位 ns。
 *
 * 取值范围只有两种合法形态:
 *   0            —— 仅当占空比为 0 时(右桥不斩波, 死区无意义);
 *   100..10000   —— 真正斩波时的死区。**1..99 ns 一律往上夹到 100 ns**:
 *                  几百 ns 的死区都嫌短, 小于 100 ns 基本等于"没插死区",
 *                  互补驱动下就是上下管直通的隐患, 所以不让它发得出去。
 */
void Widget::sendBrakeDeadFrame(int ns)
{
    int v = qBound(0, ns, 10000);

    if ((v > 0) && (v < kDeadMinNs))
        v = kDeadMinNs;

    QByteArray f;
    f.append(static_cast<char>(kBinBrakeDeadHead));
    f.append(static_cast<char>((v >> 8) & 0xFF));
    f.append(static_cast<char>(v & 0xFF));
    f.append(static_cast<char>(kBinBrakeDeadTail));

    sendBytes(f, QStringLiteral("制动死区 %1 ns").arg(v));
}

/*!
 * 占空比 0 ⇒ 死区必须 0（反之恢复）。
 *
 * 理由: 占空比 0 时右桥根本不斩波，死区没有意义；而"死区 0 + 占空比 >0"是真正危险的
 * 组合(互补驱动上下管直通)。所以这里做成**单向绑死**:
 *   占空比 = 0  -> 死区强制 0、输入框/按钮置灰, 并把 0 下发给板子(记住原来的值);
 *   占空比 > 0  -> 恢复记住的死区(默认 500 ns)、重新可编辑, 并立刻下发一次,
 *                 避免板子还停在 0 死区上就开始斩波。
 */
void Widget::applyDutyDeadCoupling(int percent)
{
    const bool canSend = isConnected();     /* 启动/加载阶段只改界面, 不往板子发 */

    if (percent <= 0) {
        if (ui->spinBoxBrakeDeadNs->value() > 0)
            m_deadNsRemembered = ui->spinBoxBrakeDeadNs->value();

        if (ui->spinBoxBrakeDeadNs->value() != 0) {
            ui->spinBoxBrakeDeadNs->setValue(0);
            if (canSend)
                sendBrakeDeadFrame(0);
        }

        ui->spinBoxBrakeDeadNs->setEnabled(false);
        ui->pushButtonApplyBrakeDead->setEnabled(false);
        return;
    }

    const bool wasDisabled = !ui->spinBoxBrakeDeadNs->isEnabled();

    ui->spinBoxBrakeDeadNs->setEnabled(true);
    ui->pushButtonApplyBrakeDead->setEnabled(true);

    if ((ui->spinBoxBrakeDeadNs->value() == 0) && wasDisabled) {
        /* 恢复时也不能低于 100 ns */
        const int restore = (m_deadNsRemembered >= kDeadMinNs) ? m_deadNsRemembered : kDeadMinNs;

        ui->spinBoxBrakeDeadNs->setValue(restore);
        if (canSend)
            sendBrakeDeadFrame(restore);
    }
}

/*!
 * 占空比滑块: 只在"控制参数预设"页实时调节。
 *
 * ⚠ 拖动一次会来几十个 valueChanged, 每个都发一帧会把 TCP 和板子的日志刷满 ——
 *   所以按 kDutySendGapMs 节流, 松手时(sliderReleased)再补一帧最终值, 保证不丢最后一下。
 */
void Widget::onBrakeDutySliderMoved(int percent)
{
    ui->labelBrakeDutyValue->setText(QStringLiteral("%1 %").arg(percent));

    /* 占空比 0 <-> 非 0 的边界上要连带处理死区(见 applyDutyDeadCoupling) */
    applyDutyDeadCoupling(percent);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if ((percent == m_dutyLastSent) || ((now - m_dutyLastSentMs) < kDutySendGapMs))
        return;

    m_dutyLastSentMs = now;
    m_dutyLastSent   = percent;
    sendBrakeDutyFrame(percent);
}

void Widget::onBrakeDutySliderReleased()
{
    const int percent = ui->sliderBrakeDuty->value();

    if (percent == m_dutyLastSent)
        return;

    m_dutyLastSentMs = QDateTime::currentMSecsSinceEpoch();
    m_dutyLastSent   = percent;
    sendBrakeDutyFrame(percent);
}

void Widget::onApplyBrakeDead()
{
    /* 占空比 0 时死区只能是 0(按钮此时已置灰, 这里再兜一层) */
    if (ui->sliderBrakeDuty->value() <= 0) {
        appendLog(QStringLiteral("× 占空比为 0 时死区必须为 0, 未下发"), kColorErr);
        return;
    }

    /* 手输的死区低于 100 ns 一律提到 100 ns 再发(小于 100 等于没插死区) */
    int ns = ui->spinBoxBrakeDeadNs->value();
    if (ns < kDeadMinNs) {
        ns = kDeadMinNs;
        ui->spinBoxBrakeDeadNs->setValue(ns);
        appendLog(QStringLiteral("· 死区小于 %1 ns, 已按 %1 ns 下发").arg(kDeadMinNs), kColorInfo);
    }

    sendBrakeDeadFrame(ns);
}

/*!
 * 电流零点校准(0xE3): 让板子把**当前**这一路的引脚电压当成"零电流电压"。
 *
 * ⚠ 前提是制动关闭、线圈里没有电流 —— 否则标出来的是带负载的电压。
 *   板子那边也会再检查一遍(采样中 + 制动关), 不满足会拒绝。
 */
void Widget::onCalibrateCurrentZero()
{
    if (m_brakeOn) {
        appendLog(QStringLiteral("× 制动开着, 先点\"关闭制动\"(0xA5) 再做电流零点校准"), kColorErr);
        return;
    }

    if (!m_acquiring) {
        appendLog(QStringLiteral("× 还没开始采集, 先点\"开始采集\"(0xE0) 再做零点校准"), kColorErr);
        return;
    }

    appendLog(QStringLiteral("■ 电流零点校准: 制动保持关闭, 采 2 s 电压作为归零电压"), kColorErr);
    sendByte(kBinSampZero, QStringLiteral("电流零点校准 (0xE3)"));
}

/*!
 * 发电流指令帧: **[0xBA][低字节][高字节][0xAB]**, 4 个字节必须在同一次 write 里。
 * @param milliAmps : 目标电流(mA), 0..15000; 5 个桥共用这一条指令。
 */
void Widget::sendCurrentFrame(int milliAmps)
{
    const int v = qBound(0, milliAmps, kCurrentMaxMa);

    QByteArray f;
    f.append(static_cast<char>(kBinCurrentHead));
    f.append(static_cast<char>(v & 0xFF));          /* 低字节在前 */
    f.append(static_cast<char>((v >> 8) & 0xFF));
    f.append(static_cast<char>(kBinCurrentTail));

    sendBytes(f, QStringLiteral("电流指令 %1 mA (%2 A)").arg(v).arg(v / 1000.0, 0, 'f', 3));
}

/*!
 * 电流指令滑块: 拖动即时下发(50 ms 节流), 松手补发最终值。
 *
 * ⚠ 没启动制动时滑块是禁用的(见 updateCurrentCmdEnabled); 这里再挡一道, 免得有别的
 *   代码路径(比如 setValue)绕过禁用态把电流指令发出去 —— 电流闭环只要目标 > 0 就会
 *   自己开桥, 这是安全问题, 不是界面问题。
 */
void Widget::onCurrentSliderMoved(int milliAmps)
{
    ui->labelCurrentCmdValue->setText(QStringLiteral("%1 A").arg(milliAmps / 1000.0, 0, 'f', 2));

    if (!m_brakeOn)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if ((milliAmps == m_curLastSent) || ((now - m_curLastSentMs) < kCurrentSendGapMs))
        return;

    m_curLastSentMs = now;
    m_curLastSent   = milliAmps;
    sendCurrentFrame(milliAmps);
}

void Widget::onCurrentSliderReleased()
{
    if (!m_brakeOn)
        return;

    const int ma = ui->sliderCurrentCmd->value();

    if (ma == m_curLastSent)
        return;

    m_curLastSentMs = QDateTime::currentMSecsSinceEpoch();
    m_curLastSent   = ma;
    sendCurrentFrame(ma);
}

void Widget::onCurrentZeroClicked()
{
    if (!m_brakeOn) {
        appendLog(QStringLiteral("· 制动没开, 电流指令已经是 0 A"), kColorGray);
        return;
    }

    ui->sliderCurrentCmd->setValue(0);      /* 会触发 valueChanged -> 下发 0 mA */
    ui->labelCurrentCmdValue->setText(QStringLiteral("0.00 A"));
    appendLog(QStringLiteral("■ 电流归零: 下位机把 5 路占空比爬到 0 后关桥输出"), kColorErr);
}

/*!
 * 把电流指令滑块摆回 0, **不发帧**。
 *
 * 用在"制动被关掉"的那些地方(0xA5 / 停止一切 / 连上板子):
 *   · 下位机的 0xA5 已经让电流闭环把目标清 0 并放手了(见 Svc_CurLoop_Stop);
 *   · 界面上要是还停着 5 A, 操作员下次轻轻一动滑块, 就会从 5 A 重新接管功率级 ——
 *     看着像"只动了一点点", 实际一下子加满, 现场很危险。
 * 所以这里用 QSignalBlocker 把值摆回 0, 不触发 valueChanged(也就不会多发一条帧)。
 */
void Widget::resetCurrentCmdUi()
{
    const QSignalBlocker blocker(ui->sliderCurrentCmd);
    ui->sliderCurrentCmd->setValue(0);
    ui->labelCurrentCmdValue->setText(QStringLiteral("0.00 A"));
    m_curLastSent = 0;
}

/*!
 * 制动没开的时候把"电流指令"整块锁住: 滑块归 0 + 变灰不可拖。
 *
 * 为什么(现场要求): 电流闭环只要目标 > 0 就会自己开桥输出 —— 也就是说"动一下滑块"
 * 等于"让功率级出电流"。所以没启动制动时滑块必须是 0 而且**动不了**, 免得误碰:
 *   · 归 0 用 resetCurrentCmdUi(不触发 valueChanged, 不会再发一条 0 mA 帧);
 *   · 下位机那边 0xA5 已经把闭环目标清 0 并放手了, 界面跟着归零就不会出现
 *     "界面显示 5 A 但板子目标 0"这种错位, 也不会让操作员一动手就从旧值接管。
 * 制动开启(启动制动成功)后才放开, 这时拖滑块才是真的在下电流指令。
 */
void Widget::updateCurrentCmdEnabled()
{
    const bool on = m_brakeOn;

    if (!on) {
        resetCurrentCmdUi();                 /* 归 0, 不发帧 */
    }

    ui->sliderCurrentCmd->setEnabled(on);
    ui->pushButtonCurrentZero->setEnabled(on);
    ui->labelCurrentCmd->setEnabled(on);
    ui->labelCurrentCmdValue->setEnabled(on);
    ui->labelCurrentCmdTip->setEnabled(on);

    ui->sliderCurrentCmd->setToolTip(
        on ? QStringLiteral("拖动即下发电流指令帧 [0xBA][低字节][高字节][0xAB], 数据 = 目标电流(mA), "
                            "范围 0..15000。5 个桥共用这条指令; 0 mA = 停止(下位机把占空比爬到 0 后关桥输出)。")
           : QStringLiteral("制动没开, 电流指令被锁定为 0 A —— 先点\"启动制动\", 滑块才能动。"
                            "(电流闭环只要目标 > 0 就会自己开桥, 所以没制动时不许下电流指令)"));
}

void Widget::setLinkState(bool connected, const QString &text)
{
    m_linkConnected = connected;
    m_linkText      = text;

    ui->ledIndicator->setOn(connected);
    updateStatusLabel();
}

void Widget::updateStatusLabel()
{
    /*
     * ⚠⚠ 这个函数**每一条遥测都会被调用**(板子 10 ms 一行 = 100 Hz)。
     *
     *   原来它每次都 setText() + setStyleSheet(): setStyleSheet 会让 Qt 重新解析样式表
     *   并 repolish 控件 —— 一次几百微秒到毫秒级, 100 Hz 地做就是白白烧 CPU 并触发重绘,
     *   是"界面卡顿"的一部分(现场反馈: 图像绘制过多导致卡顿)。
     *
     *   改法:
     *     ① 样式表只在**状态真的变了**时设(未连接 / 正常 / 过流 —— 都是低频事件);
     *     ② 文字里的"已收 N 点"每帧都在变, 限成最多 200 ms 刷一次(人眼也看不出区别)。
     */
    const int state = (!m_linkConnected) ? 0 : (m_ocAlarm ? 2 : 1);

    if (state != m_statusLastState) {
        m_statusLastState = state;

        switch (state) {
        case 0:
            ui->labelLink->setStyleSheet(QStringLiteral("color:#8A8A8A;"));
            break;
        case 2:
            ui->labelLink->setStyleSheet(QStringLiteral("color:#C00000;font-weight:bold;"));
            break;
        default:
            ui->labelLink->setStyleSheet(QStringLiteral("color:#117A2B;font-weight:bold;"));
            break;
        }
    }

    QString text;
    if (state == 0) {
        text = m_linkText;
    } else if (state == 2) {
        /* 过流优先显示: 硬件关断桥是静默发生的, 界面必须显眼(来源是下位机主动上报的 0xC0)。 */
        text = QStringLiteral("⚠ 过流(0xC0) 桥[%1]").arg(m_ocSrc);
    } else {
        text = m_linkText;
        if (m_acquiring && !m_cache.isEmpty())
            text += QStringLiteral(" · %1 点").arg(m_cache.size());
    }

    if (text == m_statusLastText)
        return;                     /* 一个字都没变, 不用碰控件 */

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    /* 正常态的文字里带着"点数", 每帧都在变 —— 限到 5 Hz; 未连接/过流是低频事件, 立刻生效 */
    if ((state == 1) && ((now - m_statusLastTextMs) < 200))
        return;

    m_statusLastText   = text;
    m_statusLastTextMs = now;
    ui->labelLink->setText(text);
}

int Widget::ledMaskFromUi() const
{
    const QCheckBox *const boxes[LedCount] = {
        ui->checkBoxLed0, ui->checkBoxLed1, ui->checkBoxLed2, ui->checkBoxLed3,
    };

    int mask = 0;
    for (int i = 0; i < LedCount; ++i) {
        if (boxes[i]->isChecked())
            mask |= (1 << i);
    }

    return mask;
}

void Widget::setLedMaskToUi(int mask)
{
    QCheckBox *const boxes[LedCount] = {
        ui->checkBoxLed0, ui->checkBoxLed1, ui->checkBoxLed2, ui->checkBoxLed3,
    };

    for (int i = 0; i < LedCount; ++i) {
        /*
         * 回填是"跟着板子走", 不该被当成用户操作。现在这几个勾选框没有接 toggled,
         * 但挡住信号是免费的 —— 以后真加了"勾一下就下发", 这里就不会把板子回来的
         * 状态再原样发一遍。
         */
        const bool blocked = boxes[i]->blockSignals(true);
        boxes[i]->setChecked((mask & (1 << i)) != 0);
        boxes[i]->blockSignals(blocked);
    }
}

QString Widget::ledMaskText(int mask) const
{
    if (mask == 0)
        return QStringLiteral("四路全灭");

    QStringList on;
    for (int i = 0; i < LedCount; ++i) {
        if ((mask & (1 << i)) != 0)
            on << QStringLiteral("LED%1").arg(i);
    }

    return on.join(QLatin1Char('+'));
}

/*!
 * 认板子的 LED 回复, 并把界面同步成"板子上的真实状态"。
 *
 * 回复长这样(都以 "led" + 空格/冒号 开头; 括号里是**发出去的字节**):
 *   led mask=0x05 on=0,2 freq=100Hz state=normal   0xF4 查状态
 *   led mask -> 0x05 (on=0,2)                      0xF0 / 0xF5 / 0xFF <掩码>
 *   led 1 ON, mask=0x02 (on=1)                     0xF6..0xF9 / 0xFA..0xFD
 *   led freq -> 250 Hz (shared by all four ...)    0xFE <频率/10>
 *   led state -> normal (0xF2), freq 100 Hz        0xF1..0xF3(老 LED_STA 的语义)
 *   led: 0xFE <freq/10> ...                        参数被拒 / 参数没跟上来(不改硬件)
 *
 * ⚠ 板子那边换的是**命令形态**(文本 -> 单字节 0xF0..0xFF), 回话一个字没改 ——
 *   所以这里只认 "led " 前缀 + mask=0x.. / freq N 这两个模式, 不用跟着改。
 *
 * STATUS 那行是 "zynq | led=0x01/normal/100Hz | ...", 开头不是 led, 但里面也带了
 * 掩码和频率 —— 一并认掉, 这样点 STATUS 也能把这一页刷新。
 *
 * 为什么"以板子为准": 掩码可能不是从这里改的(比如板子启动是四路全灭, 或者别的
 * 客户端改了它), 界面照着回复改永远比照着"我发过什么"猜要准。
 */
bool Widget::parseLedReply(const QString &line)
{
    static const QRegularExpression reMask(QStringLiteral("mask=0x([0-9A-Fa-f]{1,2})"));
    static const QRegularExpression reMaskStatus(QStringLiteral("led=0x([0-9A-Fa-f]{1,2})"));
    static const QRegularExpression reFreq(QStringLiteral("freq[^0-9]*([0-9]+)"));

    const QString lower = line.toLower();
    const bool    isLed = lower.startsWith(QLatin1String("led "))
                          || lower.startsWith(QLatin1String("led:"));
    const bool    isStatus = line.contains(QLatin1String("led=0x"));

    if (!isLed && !isStatus)
        return false;

    QRegularExpressionMatch mMask = reMask.match(line);
    if (!mMask.hasMatch() && isStatus)
        mMask = reMaskStatus.match(line);

    if (mMask.hasMatch()) {
        bool      ok   = false;
        const int mask = mMask.captured(1).toInt(&ok, 16);

        /* 越界的值不往界面上写: 板子只可能回 0..15, 别的说明这行不是我们认的那条。 */
        if (ok && mask >= 0 && mask <= 0x0F)
            setLedMaskToUi(mask);
    }

    const QRegularExpressionMatch mFreq = reFreq.match(line);
    if (mFreq.hasMatch()) {
        bool      ok = false;
        const int hz = mFreq.captured(1).toInt(&ok);

        if (ok && hz >= ui->spinBoxLedFreq->minimum() && hz <= ui->spinBoxLedFreq->maximum())
            ui->spinBoxLedFreq->setValue(hz);
    }

    return true;
}

void Widget::onApplyLed()
{
    if (!isConnected()) {
        appendLog(QStringLiteral("× 未连接板子, LED 参数未下发"), kColorErr);
        return;
    }

    const int mask = ledMaskFromUi();
    const int freq = ui->spinBoxLedFreq->value();

    const QString maskHex = QStringLiteral("%1").arg(mask, 2, 16, QLatin1Char('0')).toUpper();

    appendLog(QStringLiteral("· 下发 LED: 掩码 0x%1 (%2), 共享频率 %3 Hz")
                  .arg(maskHex, ledMaskText(mask)).arg(freq),
              kColorTx);

    /*
     * 顺序有讲究: **先频率(0xFE) 再掩码(0xFF)**。
     *
     * 驱动里有个不变量: "设了频率却四路全灭" 是说不通的状态, 所以设频率在掩码还是 0
     * 的时候会顺手点亮指示灯(LED0)。掩码为 0 时如果反过来先发 0xFF 0、再发 0xFE,
     * 最后 LED0 反而被点亮了 —— 用户看到的"四路全灭"就没做到。掩码非 0 时设频率
     * 不会动掩码(宿主机测试 tools/led_test 钉住了这条)。
     *
     * ⚠ 两条命令**各发一次**(两条独立命令, 不是"命令+参数"): 就算被 TCP 合并成一段,
     *   板子那边也是按字节流一条条处理的(见 monitor_app.c 的扫描循环)。
     */
    sendBytes(QByteArray(1, char(kBinLedFreq))
                  + QByteArray(1, char(freq / kLedFreqUnit)),
              QStringLiteral("共享频率 %1 Hz").arg(freq));

    sendBytes(QByteArray(1, char(kBinLedMask)) + QByteArray(1, char(mask)),
              QStringLiteral("掩码 0x%1 (%2)").arg(maskHex, ledMaskText(mask)));

    if (mask == 0) {
        appendLog(QStringLiteral("· 掩码 0 = 四路全灭。注意 LED0 被板子借去做\"网口在线\"指示: "
                                 "上位机连上/断开的那一刻板子会自动点亮/熄灭它, 之后再回到这里的勾选。"),
                  kColorGray);
    }
}

void Widget::onLedAllOn()
{
    setLedMaskToUi(0x0F);
    onApplyLed();
}

void Widget::onLedAllOff()
{
    setLedMaskToUi(0);

    if (!isConnected()) {
        appendLog(QStringLiteral("× 未连接板子, LED 未熄灭"), kColorErr);
        return;
    }

    /*
     * 只发 0xF0(四路全灭), 不跟着发频率: 频率那条在掩码还是 0 时会先把 LED0 点亮一下
     * (见 onApplyLed 的说明), 再被掩码关掉 —— 没必要闪这一下。频率留到下次"下发勾选"
     * 时一起发。
     */
    appendLog(QStringLiteral("· 四路全灭 (0xF0)"), kColorTx);
    sendByte(kBinLedAllOff, QString());
}

void Widget::reloadLedFromBoard()
{
    if (!isConnected()) {
        appendLog(QStringLiteral("× 未连接板子, 读不到 LED 状态"), kColorErr);
        return;
    }

    appendLog(QStringLiteral("· 读板子 LED 状态 (0xF4)"), kColorTx);
    sendByte(kBinLedStatus, QString());
}


/* ========================================================================== */
/*  右侧按钮                                                                   */
/* ========================================================================== */

void Widget::connectToBoard()
{
    const QString ip = ui->lineEditIp->text().trimmed();

    QHostAddress address;
    if (!address.setAddress(ip)) {
        appendLog(QStringLiteral("× IP 地址不合法: %1").arg(ip), kColorErr);
        return;
    }

    setLinkState(false, QStringLiteral("连接中…"));
    updateConnectButton(false);
    ui->pushButtonConnect->setEnabled(false);          // 连的时候别让再点一次

    appendLog(QStringLiteral("… 正在连接 %1:%2")
                  .arg(ip)
                  .arg(ui->spinBoxPort->value()),
              kColorGray);

    m_socket->abort();
    m_socket->connectToHost(ip, quint16(ui->spinBoxPort->value()));
}

void Widget::onConnectButton()
{
    if (isConnected()) {
        /*
         * 断开之前先把制动停掉: 板子的功率级是独立跑的, TCP 断了它还会继续输出,
         * 线圈会一直带电 —— 这是功率级, 默认按安全的一侧处理。
         * ⚠ 用 0xA5(关闭制动: 停时序 + 全关桥 + 关全局调制 + 清过流), 不是老的 PWMSTOP
         *   文本命令 —— 下位机那条命令已经随 PWM 命令表一起删了, 现在发它只会被当未知命令。
         */
        sendByte(0xA5, QStringLiteral("断开前关闭制动"));

        // 顺手也让板子停止采样: 没人接收的话没必要继续占 SPI 总线
        sendByte(kBinSampStop, QStringLiteral("停止采样 (0xE1)"));

        QTimer::singleShot(120, this, [this] {
            if (m_socket->state() != QAbstractSocket::UnconnectedState)
                m_socket->disconnectFromHost();
        });
        return;
    }

    m_acquireAfterConnect = false;                     // 只连, 不自动开始采集
    connectToBoard();
}

void Widget::onStartAcquire()
{
    /*
     * 这是个**开关**: 采集中的时候再点一次就是"停止采集"。
     *
     * 以前这里只写 m_acquiring = true, 采起来就停不下来了 —— 想停只能按红色的
     * "停止", 而那个按钮会顺带关 PWM、断网口、导出 CSV, 对"我只是想冻住曲线看看"
     * 来说太重了。现在"停止采集"只停采集: 网口不断、缓存区和已经画出来的曲线都留着,
     * 再点一次接着采。
     *
     * ★ 2026-10-14: 它同时**真的控制板子** —— 开始发 ADSSTART(板子开始每 10 ms 采样),
     *   停止发 ADSSTOP(板子停止采样, SPI 总线安静)。板子上电默认不采, 所以不点它
     *   一条 DATA 都不会来。
     */
    if (m_acquiring) {
        m_acquiring = false;

        // 本地模拟是另一条数据来源, 不停掉的话曲线会"停了还在动"
        if (ui->checkBoxDemo->isChecked())
            ui->checkBoxDemo->setChecked(false);

        updateStartButton();

        /*
         * ★ 板子那边也要真的停: 0xE1 让采集任务停下来(不再发 SPI 事务、不再广播)。
         *   以前这里只改本地标志, 板子照采照发 —— 现在"停止采集"是两边一起停。
         */
        if (isConnected())
            sendByte(kBinSampStop, QStringLiteral("停止采样 (0xE1)"));

        appendLog(QStringLiteral("■ 已停止采集(已发 0xE1: 板子停止采样, 总线安静; 网口保持连接)"),
                  kColorInfo);
        return;
    }

    if (isConnected()) {
        m_acquiring = true;
        updateStartButton();

        /*
         * ★ 开始采集 = 板子开始采样: 发 0xE0 之后才每 10 ms 扫一轮并回一行 DATA。
         *   板子上电默认是**不采**的(见 service_ads.h 的 SVC_ADS_RUN_DEFAULT)。
         */
        sendByte(kBinSampStart, QStringLiteral("开始采样 (0xE0)"));
        appendLog(QStringLiteral("● 已开始采集(已发 0xE0: 板子开始采样并回遥测)"), kColorOk);
        sendCommand(QStringLiteral("STATUS"));

        /*
         * (原来这里还有"开始采集时自动从 eMMC 拉一遍数据刷新曲线", 2026-10-16 随 eMMC 测试栏删了)
         *
         * 先清掉曲线/缓存, 再把卡上录的整段拉回来回放 —— 这样每点一次"开始采集",
         * 看到的都是"当前卡上真正存着的东西", 而不是上一次会话留在界面上的旧曲线。
         * 之后的实时遥测会接着往后画。
         *
         * 用 singleShot 错开一拍, 让上面的 STATUS 先过去: 板子是一问一答的, 连着灌
         * 两条命令会让回复挤在一起, 日志里看不清哪句对哪句。
         */
        return;
    }

    // 没连就先连; 连上之后自动开始采集, 免得让用户点两次
    m_acquireAfterConnect = true;
    connectToBoard();
}

/*!
 * "开始采集 / 停止采集"按钮跟着采集状态走。
 *
 * 采集时按钮变成"停止采集"并换成琥珀色 —— 这个按钮从这个时刻起的作用是"停",
 * 颜色/文字不跟着变的话, 用户根本看不出它还能再点一次(这次的问题就是这么来的)。
 *
 * ★ 这个按钮**真的控制板子** —— 开始发 0xE0(板子开始采样),
 *   停止发 0xE1(板子停止采样, SPI 总线安静)。板子上电默认不采, 所以不点这个按钮
 *   就不会有任何 DATA 行。
 */
void Widget::updateStartButton()
{
    if (m_acquiring) {
        ui->pushButtonStart->setText(QStringLiteral("停止采集"));
        ui->pushButtonStart->setToolTip(QStringLiteral(
            "停止采集: 给板子发 0xE1 —— 板子停止采样(不再发 SPI 事务)、遥测也停。\n"
            "网口不断、已有曲线和缓存区都留着; 再点一次继续采集。\n"
            "(要连输出一起停, 用下面红色的\"停止\"。)"));
        ui->pushButtonStart->setStyleSheet(QStringLiteral(
            "QPushButton{background-color:#F5C26B;border:1px solid #A9701A;border-radius:3px;"
            "padding:4px 6px;font-weight:bold;}"
            "QPushButton:hover{background-color:#F8D28C;}"
            "QPushButton:pressed{background-color:#E0AC50;}"));
    } else {
        ui->pushButtonStart->setText(QStringLiteral("开始采集"));
        ui->pushButtonStart->setToolTip(QStringLiteral(
            "开始采集: 还没连就先连; 连上后给板子发 0xE0, 板子开始每 10 ms 采 8 路\n"
            "并回一行 DATA(不点它板子是不采的)。\n"
            "采集过程中再点一次 = 停止采集(发 0xE1)。"));
        ui->pushButtonStart->setStyleSheet(QStringLiteral(
            "QPushButton{background-color:#9FC5E8;border:1px solid #3D6E9E;border-radius:3px;"
            "padding:4px 6px;font-weight:bold;}"
            "QPushButton:hover{background-color:#B9D7F2;}"
            "QPushButton:pressed{background-color:#7FAEDC;}"));
    }
}

/*!
 * "启动制动 / 关闭制动"这个按钮的文字与配色。
 *
 * 这个按钮现在**点一下变一次身**:
 *   关着 → 点一下 = 发 0xA0(开全局调制) + 0xA1(五路桥同时打开), 然后变成"关闭制动";
 *   开着 → 点一下 = 发 0xA5(五桥全关 + 关调制), 再变回"启动制动"。
 * 占空比取"控制参数预设"页滑块的当前值(0xA1 用的是下位机记住的那个值), 拖动滑块
 * 会立刻把新占空比同步到五路。
 */
void Widget::updateBrakeButton()
{
    if (m_brakeOn) {        ui->pushButtonBrake->setText(QStringLiteral("关闭制动"));
        ui->pushButtonBrake->setToolTip(QStringLiteral(
            "关闭制动: 发 0xA5 —— 五路桥全部关断 + 关全局调制。\n"
            "再点一次 = 重新发 0xA0 + 0xA1 启动。"));
        ui->pushButtonBrake->setStyleSheet(QStringLiteral(
            "QPushButton{background-color:#F2D5D5;border:1px solid #A33;border-radius:3px;"
            "padding:4px 6px;font-weight:bold;}"
            "QPushButton:hover{background-color:#F7E3E3;}"
            "QPushButton:pressed{background-color:#E4B9B9;}"));
    } else {
        ui->pushButtonBrake->setText(QStringLiteral("启动制动"));
        ui->pushButtonBrake->setToolTip(QStringLiteral(
            "启动制动: 依次发两个字节 —— 0xA0(开全局调制) + 0xA1(五路桥同时打开)。\n"
            "五路用**同一个占空比**, 取\"控制参数预设\"页滑块的值。\n"
            "关的时候再点一次发 0xA5(五桥全关 + 关调制)。"));
        ui->pushButtonBrake->setStyleSheet(QStringLiteral(
            "QPushButton{background-color:#F5C26B;border:1px solid #A9701A;border-radius:3px;"
            "padding:4px 6px;font-weight:bold;}"
            "QPushButton:hover{background-color:#F8D28C;}"
            "QPushButton:pressed{background-color:#E0AC50;}"));
    }

    /* 电流指令滑块跟着制动状态走: 没开制动就锁住并归 0(见 updateCurrentCmdEnabled) */
    updateCurrentCmdEnabled();
}

void Widget::onLaunchCar()
{
    if (!isConnected()) {
        appendLog(QStringLiteral("× 未连接板子, 无法发射小车"), kColorErr);
        return;
    }

    const int gear  = ui->comboBoxLaunchSpeed->currentIndex();   /* 0..5 -> SS1..SS6 */
    const int mmps  = launchIndexToMmps(gear);
    const unsigned char code = static_cast<unsigned char>(kBinMoverGear1 + gear);

    appendLog(QStringLiteral("▲ 发射小车: SS%1 = %2 m/s (%3 mm/s), 发字节 %4")
                  .arg(gear + 1)
                  .arg(mmps / 1000.0, 0, 'f', 1)
                  .arg(mmps)
                  .arg(byteHex(code)),
              kColorInfo);

    /*
     * ⚠ 2026-10-16 起: **不再发文本命令** `MOTORLAUNCH <mm/s>`, 改发**单字节**
     *   0xD1..0xD6(= SS1..SS6 档 + 立刻发射), 和制动那套 0xA0..0xA5 一个路子。
     *   下位机侧对应 app/Mover_app.c 的 Mover_App_Command。
     *
     *   一条字节交给下位机走**整周期**(它内部是):
     *     0x6060=1(PP) + 控制字 6/7 + 0x000F 使能 + 0x6081 速度 + 0x607A 发射位 + 0x001F 启动
     *     -> 停在发射位约 1.5 s -> 0x000F + 0x607A=0 + 0x001F 回程 -> 约 1.8 s -> 0x0006 停机
     *   也就是原 STM32 工程 motor_init() + control_car() 的合并版, 所以点一下就能动。
     *
     * ⚠ 下位机那边是**非阻塞**的: 挂上时序就回一行文本("... armed"), 整周期约 5.5 s 跑完;
     *   这段时间界面照常刷新。回话可能是 armed / BUSY(上一条还在跑) / ERR(帧没发出去)。
     * ⚠ 发射位置不在这里给: 下位机用 MOVER_LAUNCH_POS(0x607A 的原始脉冲值, 不是 mm),
     *   免得现场多一个能填错的框。
     */
    sendByte(code, QStringLiteral("发射小车 SS%1 (%2 mm/s)").arg(gear + 1).arg(mmps));

    if (!m_acquiring) {
        appendLog(QStringLiteral("· 提示: 还没点\"开始采集\", 本次发射的数据不会进曲线"), kColorGray);
    }

    /* 发射完通常接着就要制动, 这里只提示一下, 不替用户做决定 */
    appendLog(QStringLiteral("· 需要减速时点\"启动制动\"(那是 PL 功率级的 PWM, 和这条链路独立)"),
              kColorGray);
}

/*!
 * 【启动制动 / 关闭制动】—— 一个开关, 走的是**单字节二进制协议**。
 *
 *   关 → 开: 先 0xA0(制动整体使能, 把制动模块全局打开), 再 0xA1(启动制动时序)。
 *            顺序不能反: 没使能就发启动, 下位机那边会当成无效命令丢掉。
 *   开 → 关: 0xA5(停止时序 + 去使能)。
 *
 * ⚠ 这里**不再**发 PWMF / PWMDEAD / PWMD 那三条文本命令了: 制动强度/时序现在全部
 *   由下位机的制动模块自己管(强度另有 0xA2/A3/A4 三个码, 本次没有接到界面控件上)。
 *   要单独把某一路桥拉到静态低/指定占空比, 去"控制参数预设"页点对应的按钮 ——
 *   那些按钮还照旧发文本命令, 没有被动过。
 * ⚠ m_brakeOn 只是"上位机这一侧的意图"; 板子到底有没有真的在制动, 看按钮下面
 *   右侧那个圆形图标(0xC0 过流上报)和接收区的日志。
 */
void Widget::onBrake()
{
    if (!isConnected()) {
        appendLog(QStringLiteral("× 未连接板子, 无法%1制动")
                      .arg(m_brakeOn ? QStringLiteral("关闭") : QStringLiteral("启动")),
                  kColorErr);
        return;
    }

    if (!m_brakeOn) {
        appendLog(QStringLiteral("■ 启动制动: 先同步占空比 %1%(0xDA 帧) → 0xA0 开调制 → 0xA1 五路桥同时打开")
                      .arg(ui->sliderBrakeDuty->value()),
                  kColorErr);

        // 先把滑块上的占空比/死区同步一次, 再使能 + 启动 —— 保证输出的就是界面显示的值
        applyDutyDeadCoupling(ui->sliderBrakeDuty->value());
        sendBrakeDutyFrame(ui->sliderBrakeDuty->value());
        sendBrakeDeadFrame(ui->spinBoxBrakeDeadNs->value());

        // 字节命令分两次 write + flush, 保证"先使能、后启动"这个先后顺序和报文边界
        sendByte(kBinBrakeEnable, QStringLiteral("制动使能(开全局调制)"));
        sendByte(kBinBrakeStart,  QStringLiteral("启动制动(五路桥同步)"));

        m_brakeOn = true;
    } else {
        appendLog(QStringLiteral("■ 关闭制动: 0xA5 五桥全关 + 关全局调制"), kColorErr);
        sendByte(kBinBrakeStop, QStringLiteral("停止制动"));
        m_brakeOn = false;

        /* 0xA5 已经让下位机的电流闭环把目标清 0, 界面也跟着归零(不发帧) */
        resetCurrentCmdUi();
    }

    updateBrakeButton();
}

void Widget::onSnapshot()
{
    const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/screenshots");
    if (!QDir().mkpath(dir)) {
        appendLog(QStringLiteral("× 无法创建截图目录: %1").arg(dir), kColorErr);
        return;
    }

    const QString path = QStringLiteral("%1/curve_%2.png")
                             .arg(dir, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));

    /* 两张图一起截进同一张图里(抓的是装它们的分隔条, 左右并排就是一张宽图) */
    const QPixmap shot = ui->splitterPlots->grab();
    if (shot.save(path, "PNG"))
        appendLog(QStringLiteral("√ 曲线已截图: %1").arg(path), kColorOk);
    else
        appendLog(QStringLiteral("× 截图保存失败: %1").arg(path), kColorErr);
}

void Widget::onClearCache()
{
    if (!m_cache.isEmpty()) {
        const QString path = exportCsv();
        if (!path.isEmpty())
            appendLog(QStringLiteral("· 清除前已导出缓存: %1").arg(path), kColorGray);
    }

    m_cache.clear();
    m_sampleIndex = 0;

    for (int p = 0; p < PlotCount; ++p)
        plot(PlotId(p))->clearAll();

    ui->plainTextRecv->clear();

    appendLog(QStringLiteral("√ 缓存区 / 接收区 / 曲线 已清除"), kColorOk);
}

void Widget::onStopAll()
{
    const bool hadData = !m_cache.isEmpty();

    m_acquiring = false;
    m_acquireAfterConnect = false;
    ui->checkBoxDemo->setChecked(false);      // 顺带停掉本地模拟
    updateStartButton();

    /*
     * "停止一切"里也得把制动关掉: 用 0xA5(停止时序 + 全关桥 + 关全局调制 + 清过流)。
     * ⚠ 这里以前还发一条 PWMSTOP —— 那条文本命令已经删了(下位机不认识), 0xA5 已经
     *   把功率级全关掉了, 所以不再发。
     */
    if (m_brakeOn) {
        sendByte(kBinBrakeStop, QStringLiteral("关闭制动(随\"停止\"一起)"));
        m_brakeOn = false;
        updateBrakeButton();
        resetCurrentCmdUi();        /* 同"关闭制动": 目标已清 0, 滑块也摆回 0 */
    }

    if (isConnected()) {
        sendByte(kBinSampStop, QStringLiteral("停止采样 (0xE1)"));   // 板子也停止采样

        // 等命令真的出去了再断, 否则刚发的那两个字节可能还没发走
        QTimer::singleShot(120, this, [this] {
            if (m_socket->state() != QAbstractSocket::UnconnectedState)
                m_socket->disconnectFromHost();
        });
    }

    if (hadData) {
        const QString path = exportCsv();
        if (!path.isEmpty())
            appendLog(QStringLiteral("· 已导出本次数据: %1").arg(path), kColorOk);
    }

    appendLog(QStringLiteral("■ 已停止(输出关断 + 断开网口)"), kColorErr);
    appendLog(QStringLiteral("· 本次运行累计: 发送 %1 条命令, 接收 %2 行, 缓存 %3 个采样点")
                  .arg(m_txCommands).arg(m_rxLines).arg(m_cache.size()),
              kColorGray);
}

/* ========================================================================== */
/*  网口                                                                       */
/* ========================================================================== */

void Widget::onSocketConnected()
{
    setLinkState(true, QStringLiteral("已连接"));
    updateConnectButton(true);
    ui->pushButtonConnect->setEnabled(true);

    // 新连接: 板子可能刚复位过, 遥测提示要重新给一次
    m_telemetryNoticeShown = false;

    // 制动开关 / 两个圆点 / 接收缓冲 / 过流锁存: 新链路从零开始, 别把上一条链路的
    // 残字节或旧状态带进来(resetBinaryState 里会把 m_ocAlarm 等一起清掉)。
    resetBinaryState();

    appendLog(QStringLiteral("√ 已连接 %1:%2")
                  .arg(m_socket->peerAddress().toString())
                  .arg(m_socket->peerPort()),
              kColorOk);

    /*
     * 连上先探一下: PING 回 pong, STATUS 回一行状态, 0xF4 把 4 路呼吸灯的勾选框
     * 刷成板子上的真实状态(板子上电是四路全灭, 而 LED0 会因为"网口在线"被自动点亮 ——
     * 不读一次的话界面显示的和眼睛看到的对不上)。回复都会走 handleLine →
     * parseLedReply, 所以勾选框自己就同步好了。
     *
     * ⚠ 前两条是文本命令(带 \r\n), 第三条是单字节 —— 走 sendBytes 发那一个字节, 不能
     *   走 sendCommand(那会把 0xF4 变成 "F4\r\n" 三个字符发出去)。
     */
    sendSequence(QStringList() << QStringLiteral("PING")
                               << QStringLiteral("STATUS"),
                 30);

    QTimer::singleShot(60, this, [this] {
        sendByte(kBinLedStatus, QStringLiteral("读 4 路呼吸灯状态"));
    });

    /*
     * **连上网口后默认"制动关闭"**(现场要求, 2026-10-30 改):
     *   以前是"一连上就低档 + 使能 + 启动时序", 现在反过来 —— 连上只把板子置成
     *   确定的关闭态, 制不制动完全由操作员点"启动制动"决定。
     *
     *   1) 先把界面上的占空比 / 死区同步给板子(此时桥是关的, 只是记住数值),
     *      这样等会儿点"启动制动"时用的就是滑块上那个值, 而不是板子上电时的 0%;
     *   2) 再发 0xA5: 五桥全关 + 关全局调制 + 清过流锁存 = 确定的关闭态。
     *
     * ⚠ 帧命令(0xDA../0xDC..)必须一条 write 发完; 0xA5 单独一条, 中间隔 40 ms,
     *   排障时日志一行一条清楚, 也不依赖 TCP 怎么分段。
     */
    /* 站在"占空比 0 ⇒ 死区 0"这条规则上, 再按顺序把两个值同步给板子 */
    applyDutyDeadCoupling(ui->sliderBrakeDuty->value());
    sendBrakeDutyFrame(ui->sliderBrakeDuty->value());
    sendBrakeDeadFrame(ui->spinBoxBrakeDeadNs->value());

    /*
     * 电流指令也同步一次: **明确发 0 mA**(不是把滑块上的旧值发下去)。
     *
     * 为什么要发 0: 下面 40 ms 后的 0xA5 会让下位机的电流闭环把目标清 0 并放手, 所以
     * 这时把滑块上的旧值发过去毫无意义, 只会在 40 ms 里让闭环短暂接管一下功率级。
     * 界面也一起归零(不发帧的 resetCurrentCmdUi), 免得滑块停在上次的 5 A 上, 操作员
     * 下次一动就"从 5 A 直接接管"。
     */
    resetCurrentCmdUi();
    sendCurrentFrame(0);

    QTimer::singleShot(40, this, [this] {
        sendByte(kBinBrakeStop, QStringLiteral("默认关闭制动(0xA5)"));

        if (isConnected()) {                    // 中途断开就不要再改按钮状态了
            m_brakeOn = false;
            updateBrakeButton();
        }
    });

    if (m_acquireAfterConnect) {
        m_acquireAfterConnect = false;
        m_acquiring = true;
        updateStartButton();

        // 连上就"开始采集"时同样要告诉板子开始采样(板子上电默认不采)
        sendByte(kBinSampStart, QStringLiteral("开始采样 (0xE0)"));
        appendLog(QStringLiteral("● 已开始采集(已发 0xE0)"), kColorOk);
    }
}

void Widget::onSocketDisconnected()
{
    m_acquiring = false;
    m_acquireAfterConnect = false;


    /*
     * 网口一断, 按钮文字必须弹回"启动制动"、两个圆点回到"未检测/正常":
     * 圆点说的是**当前这条链路**收到的上报, 链路没了就没有"当前状态"可言,
     * 留着上一轮的红色"0C"反而会让人以为此刻还在过流(日志里那条告警不会丢)。
     */
    resetBinaryState();

    setLinkState(false, QStringLiteral("未连接"));
    updateConnectButton(false);
    updateStartButton();
    ui->pushButtonConnect->setEnabled(true);

    appendLog(QStringLiteral("· 网口已断开"), kColorGray);
}

void Widget::onSocketErrorOccurred(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error)

    m_acquiring = false;
    m_acquireAfterConnect = false;

    resetBinaryState();

    setLinkState(false, QStringLiteral("连接失败"));
    updateConnectButton(false);
    updateStartButton();
    ui->pushButtonConnect->setEnabled(true);

    appendLog(QStringLiteral("× 网口错误: %1").arg(m_socket->errorString()), kColorErr);
}

/*!
 * 网口收到数据: **文本行**和**单字节二进制报文**混在同一条流里, 这里按字节分流。
 *
 * ⚠ 为什么不能再用"只找 \n 切行"的老写法: 二进制短报文里没有换行, 当成文本一路
 *   攒下去会越攒越多, 最后被 64 KB 那条保护当成"对端一直不发换行"整段丢掉 ——
 *   光幕/过流就再也不会更新了。0xB0 / 0xC0 在板子发的 ASCII 文本里根本不会出现,
 *   正好可以当帧头认。
 * ⚠ 为什么必须留着 m_rxBuffer 这个跨包缓冲: TCP 是字节流, 一次 readAll() 可能
 *   - 把一条文本行和一条二进制报文**粘在一起**发过来, 得先按帧头切开;
 *   - 或者只给了二进制报文的**第 1 个字节**(0xB0 到了, mask 还在下一包里) ——
 *     这时绝不能就地消费, 不然会把下一包的 mask 当成另一条报文的帧头。
 *   所以循环里只要"凑不齐一整条"就 break, 剩下的留在缓冲里等下一次 readyRead。
 * ⚠ 判帧头只看**缓冲区第 0 个字节**: 文本行一旦开头不是 0xB0/0xC0, 就整行按老逻辑
 *   走(indexOf('\n')), 行中间的字节根本不会被当帧头看, 不会误切。
 */
void Widget::onReadyRead()
{
    m_rxBuffer.append(m_socket->readAll());

    // 板子的回复都是 \r\n 结尾; 半行留在缓冲里等下一包
    int newline = -1;
    int handled = 0;

    while (!m_rxBuffer.isEmpty()) {
        const unsigned char head = static_cast<unsigned char>(m_rxBuffer.at(0));

        /* ---- 1. 二进制报文: 0xB0/0xC0/0xAA 打头, 定长 2 字节 ------------- */
        if (head == kBinLsReport || head == kBinOcReport || head == kBinKeyReport) {
            if (m_rxBuffer.size() < 2)
                break;                               // 跨包: 只到了帧头, 等下一包

            const unsigned char arg = static_cast<unsigned char>(m_rxBuffer.at(1));
            m_rxBuffer.remove(0, 2);
            handleBinaryFrame(head, arg);
            continue;
        }

        /* ---- 2. 其余按原来的文本行处理 ----------------------------------- */
        newline = m_rxBuffer.indexOf('\n');

        /*
         * ⚠⚠ **防御性重新同步**(2026-10-15 现场踩出来的坑, 查了一整轮):
         *
         *   下位机某条回复的**长度是手写的**, 数少了就把结尾的 "\r\n" 截掉 —— 于是网口上
         *   出现一条**没有换行的文本**。而上位机是按 '\n' 分行的: 这条半行会一直留在
         *   m_rxBuffer 里等换行, 后面紧接着发来的二进制上报([0xB0][..] / [0xC0][..])
         *   就**被当成这条文本的内容一起吞掉** —— 现场表现是"过流了界面死也不变红",
         *   而且**时好时坏**(中间只要再来一条完整行, 缓冲就被冲开, 立刻又正常了)。
         *   板子那边已经修了(brake_app.c 的 Svc_Net_Reply 一律传 0 用 strlen), 这里再兜一道,
         *   免得以后任何一次"忘了换行"又把整条上报通道吃掉。
         *
         *   判据: 下位机发的**文本全是 ASCII**, 所以缓冲区里出现 >= 0x80 的字节, 就只可能
         *   是二进制帧头(0xB0/0xC0)或者野字节。在"还没有换行"的这一段里遇到它, 就在那里
         *   断开: 前面的半行丢掉并提示, 缓冲区头重新落到帧头上, 下一轮循环就能正常识别。
         */
        int resync = -1;

        for (int i = 0; i < m_rxBuffer.size(); ++i) {
            const unsigned char ch = static_cast<unsigned char>(m_rxBuffer.at(i));

            if (ch == '\n')
                break;                       // 这一段是完整的文本行, 不用重新同步

            if (ch >= 0x80) {                // 非 ASCII: 不可能是文本
                resync = i;
                break;
            }
        }

        if (resync >= 0) {
            if (resync > 0) {
                const QByteArray half = m_rxBuffer.left(resync);
                m_rxBuffer.remove(0, resync);

                appendLog(QStringLiteral("⚠ 收到一条**没有换行**的文本, 已丢弃并从后面的二进制帧"
                                         "重新同步(下位机回复长度写错了?): %1")
                              .arg(QString::fromUtf8(half.left(60))),
                          kColorErr);
            } else {
                /* 缓冲区头就是个野字节: 丢掉它继续找帧头(至少推进 1 字节, 不会死循环) */
                m_rxBuffer.remove(0, 1);
            }

            continue;
        }

        if (newline < 0)
            break;                                   // 半行, 等下一包

        QByteArray raw = m_rxBuffer.left(newline);
        m_rxBuffer.remove(0, newline + 1);

        while (raw.endsWith('\r') || raw.endsWith('\n') || raw.endsWith(' '))
            raw.chop(1);

        if (raw.isEmpty())
            continue;

        ++m_rxLines;

        // 一次性刷太多行会卡界面: 超出的只计数不显示
        if (++handled <= 200)
            handleLine(QString::fromUtf8(raw));
    }

    if (handled > 200)
        appendLog(QStringLiteral("· 本次收到 %1 行, 只显示了前 200 行").arg(handled), kColorGray);

    // 缓冲区异常增长(对端一直不发换行)时保护一下
    if (m_rxBuffer.size() > 64 * 1024) {
        appendLog(QStringLiteral("· 接收缓冲超过 64 KB 且没有换行, 已丢弃"), kColorErr);
        m_rxBuffer.clear();
    }
}

/* ========================================================================== */
/*  单字节二进制协议(制动开关 / 光幕位置 / 过流告警)                             */
/* ========================================================================== */

/*!
 * 在"启动制动"按钮的**正下方**插两个圆形状态图标: 左=小车运行位置, 右=系统状态。
 *
 * ⚠ 没有去改 widget.ui, 而是在这里 insertWidget —— 右边这一列的按钮还在经常动,
 *   在 Designer 里打开/保存 .ui 时多出来的一坨布局很容易被挤走位; 代码里插进去
 *   只占一行的位置, 上面的按钮挪多少次都不影响。
 * ⚠ 位置必须是"制动按钮的下一格"(insertWidget(brakeIndex + 1)), 不能用 addWidget:
 *   那一列下面还有 曲线截图 / 清除缓存区 / 停止 三个按钮, 加到末尾会跑到窗口最下边。
 */
void Widget::setupStatusIcons()
{
    QWidget *iconBox = new QWidget(ui->frameCtrl);

    QHBoxLayout *row = new QHBoxLayout(iconBox);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(14);

    m_iconPosition = makeCircleIcon(iconBox);
    m_iconSystem   = makeCircleIcon(iconBox);

    row->addStretch(1);
    row->addLayout(makeIconColumn(m_iconPosition, QStringLiteral("小车位置")));
    /* 系统状态那一路的说明小字要能被改(过保时显示"过保: 1,3"), 所以留个指针出来 */
    row->addLayout(makeIconColumn(m_iconSystem,   QStringLiteral("系统状态"),
                                  &m_iconSystemCaption));
    row->addStretch(1);

    const int brakeIndex = ui->verticalLayoutCtrl->indexOf(ui->pushButtonBrake);
    if (brakeIndex >= 0)
        ui->verticalLayoutCtrl->insertWidget(brakeIndex + 1, iconBox);
    else
        ui->verticalLayoutCtrl->addWidget(iconBox);   // 兜底: 万一布局被改过, 至少别丢

    updatePositionIcon();
    updateSystemIcon();
}

/*!
 * 左侧圆点: 小车压在哪一路光幕(由 0xB0 的 mask 驱动)。
 *   mask 没收到过 / == 0 → "未检测"(灰)
 *   mask 里最高置位的是 bit i → "线圈 N"(N = i+1, 蓝)
 */
void Widget::updatePositionIcon()
{
    if (m_iconPosition == nullptr)
        return;

    const int coil = (m_lsSeen && m_lsMask != 0) ? (highestSetBit(m_lsMask) + 1) : 0;

    if (coil <= 0) {
        m_iconPosition->setText(QStringLiteral("未检测"));
        m_iconPosition->setStyleSheet(circleStyle("#9AA0A6", "#6B7075"));
        m_iconPosition->setToolTip(QStringLiteral(
            "小车运行位置(光幕 0xB0 上报)\n"
            "还没收到过 0xB0, 或者 mask=0x00(没有一路光幕被挡)。"));
        return;
    }

    m_iconPosition->setText(QStringLiteral("线圈 %1").arg(coil));
    m_iconPosition->setStyleSheet(circleStyle("#1F6FEB", "#12448F"));
    m_iconPosition->setToolTip(QStringLiteral(
        "小车运行位置(光幕 0xB0 上报)\n"
        "mask=%1: 最高被挡的是第 %2 路 → 线圈 %2")
                                   .arg(byteHex(m_lsMask))
                                   .arg(coil));
}

/*!
 * 右侧圆点: 系统状态(由 0xC0 过流上报驱动)。
 *
 *   过流行 src == 0 → "正常"(绿)
 *   过流行 src != 0 → "0C" + 底下一行**点名哪几路**(红), 例如 src=0x05 → "过保: 1,3"
 *
 * ⚠ **跟着引脚的实时状态走, 不锁存**(现场要求, 2026-10-15 改的):
 *   下位机现在**过流出现和消失都上报**(消失时发 src=0), 所以这里收到 src=0 就回绿色
 *   "正常" —— 之前那版是"收到过一次就一直红着", 引脚恢复正常了界面还写着告警, 反而不对。
 * ⚠ 下位机同时还会做保护动作(关全部桥), 那是另一回事: 界面只反映"现在还有没有过流"。
 */
void Widget::updateSystemIcon()
{
    if (m_iconSystem == nullptr)
        return;

    if (m_ocAlarm) {
        /* 圆点里放 "0C", 桥号放到下面那行小字里 —— 圆点只有 78 px, 放不下 "0C 1,3" */
        m_iconSystem->setText(QStringLiteral("0C"));
        m_iconSystem->setStyleSheet(circleStyle("#CF222E", "#8B1219"));

        if (m_iconSystemCaption != nullptr) {
            m_iconSystemCaption->setText(QStringLiteral("过保: %1").arg(m_ocSrc));
            m_iconSystemCaption->setStyleSheet(
                QStringLiteral("QLabel{color:#CF222E;font-size:9pt;font-weight:bold;}"));
        }

        m_iconSystem->setToolTip(QStringLiteral(
            "系统状态: **过流中**(0xC0 上报, 实时状态)。\n"
            "过保的桥/路: %1 号 —— 下位机已把全部桥关掉做保护。\n"
            "引脚恢复后下位机会再发一条 src=0, 这里会自动回到「正常」。").arg(m_ocSrc));
    } else {
        m_iconSystem->setText(QStringLiteral("正常"));
        m_iconSystem->setStyleSheet(circleStyle("#2DA44E", "#1B7A36"));

        if (m_iconSystemCaption != nullptr) {
            m_iconSystemCaption->setText(QStringLiteral("系统状态"));
            m_iconSystemCaption->setStyleSheet(
                QStringLiteral("QLabel{color:#5A5A5A;font-size:9pt;}"));
        }

        m_iconSystem->setToolTip(QStringLiteral(
            "系统状态: 当前没有过流(引脚的过保信号都是正常的)。"));
    }
}

/*!
 * 一条 2 字节上报落地: 刷界面 + 记一行日志(每一条都是板子**主动**推的, 不是回复)。
 */
void Widget::handleBinaryFrame(unsigned char cmd, unsigned char arg)
{
    if (cmd == kBinKeyReport) {
        /*
         * 按键上报(板子主动推): [0xAA][掩码], bit0 = KEY1 按着, bit1 = KEY2 按着。
         *
         * 这一条回答的就是"刚才是谁把连接弄断了":
         *   bit0 -> KEY1 被按下, **板子马上软复位**(连接会断, 起来后要重连);
         *   bit1 -> KEY2 被按下, **板子马上重启 TCP 服务端**(掉线后重连, 板子不重启);
         *   0x00 -> 两个键都松开(板子刚起来时第一次上报就是它)。
         *
         * ⚠ 板子是"按下/松开都报、变化才报", 所以这里也按状态处理, 不当时事件 ——
         *   同一状态重复到达就什么都不做(和光幕那条一个处理法)。
         * ⚠ 收到的上报可能**晚于**断开信号(复位很快): 日志照样写, 用户往回翻能看到
         *   "是按键按的", 这正是要的效果。
         */
        const int mask = int(arg);

        if (m_keySeen && m_keyMask == mask)
            return;

        m_keySeen = true;
        m_keyMask = mask;

        const bool key1 = (mask & 0x01) != 0;
        const bool key2 = (mask & 0x02) != 0;

        if (key1) {
            appendLog(QStringLiteral("● 按键 0xAA: KEY1(复位键) 按下 → 板子即将软复位, 连接会断"),
                      kColorInfo);
        } else if (key2) {
            appendLog(QStringLiteral("● 按键 0xAA: KEY2(网口重连键) 按下 → 板子即将重启 TCP 服务端, "
                                     "掉线后会自动重连(板子不重启)"),
                      kColorInfo);
        } else {
            appendLog(QStringLiteral("○ 按键 0xAA: 两个键都松开 (mask=0x00)"), kColorInfo);
        }

        return;
    }

    if (cmd == kBinLsReport) {
        const int mask = int(arg);

        /*
         * 下位机是"状态变化才推", 但重连之后第一次推的值可能和上次一样 ——
         * 这里再比一次, 没变化就既不重画也不写日志, 免得接收区被同一行刷屏。
         */
        if (m_lsSeen && m_lsMask == mask)
            return;

        m_lsSeen = true;
        m_lsMask = mask;
        updatePositionIcon();

        if (mask == 0) {
            appendLog(QStringLiteral("○ 光幕 0xB0: mask=0x00 全部无遮挡 → 小车位置未检测"),
                      kColorInfo);
        } else {
            appendLog(QStringLiteral("● 光幕 0xB0: mask=%1 → 第 %2 路被挡(线圈 %2)")
                          .arg(byteHex(mask))
                          .arg(highestSetBit(mask) + 1),
                      kColorInfo);
        }
        return;
    }

    if (cmd == kBinOcReport) {
        const int src = int(arg);

        /*
         * ⚠ 语义(2026-10-15 现场定): 下位机报的是**状态**, 不是事件 ——
         *   src != 0 = 此刻/刚刚有过保(位图点明哪几路), src == 0 = 过保已恢复。
         *   界面跟着这两条走, 收到 src=0 就变回"正常"(不需要人工确认)。
         *
         * ⚠⚠ 但**红点必须亮得够久**: 真实过流是"硬件一钳位、电流立刻没了、oc_n 只低
         *   几微秒", 下位机两条报文可能挨着上来(红 5 ms 立刻变绿 = 人眼觉得从来没红)。
         *   所以这里记一轮"告警轮次 + 起始时刻", 收到"恢复"时如果还没亮够
         *   kOcMinRedMs, 就**推迟**变绿; 推迟期间若又收到告警(轮次变了)就作废这次推迟。
         *   下位机侧也有保持窗口(brake_app.h 的 BRAKE_OC_HOLD_MS), 两边是双保险。
         */
        if (src == 0) {
            const qint64 shownMs = (m_ocRedStartMs == 0)
                                   ? kOcMinRedMs
                                   : (QDateTime::currentMSecsSinceEpoch() - m_ocRedStartMs);

            if (m_ocAlarm && shownMs < qint64(kOcMinRedMs)) {
                /* 还没亮够: 挂一个单次定时器, 到点再变绿(期间又来告警就不执行) */
                const quint64 seq = m_ocSeq;

                appendLog(QStringLiteral("· 过保信号已恢复, 但红点至少亮满 %1 ms 再变绿")
                              .arg(kOcMinRedMs),
                          kColorGray);

                QTimer::singleShot(int(kOcMinRedMs - shownMs), this, [this, seq]() {
                    if (seq != m_ocSeq)
                        return;                     // 推迟期间又来告警了, 这次变绿作废
                    m_ocAlarm      = false;
                    m_ocSrc        = QStringLiteral("-");
                    m_ocRedStartMs = 0;
                    updateSystemIcon();
                });
                return;
            }

            if (m_ocAlarm) {
                appendLog(QStringLiteral("● 过流已消失(0xC0 src=0)—— 过保引脚恢复正常"),
                          kColorOk);
            }
            m_ocAlarm      = false;
            m_ocSrc        = QStringLiteral("-");
            m_ocRedStartMs = 0;
            updateSystemIcon();
            return;
        }

        const bool    wasAlarm = m_ocAlarm;
        const QString oldSrc   = m_ocSrc;

        m_ocAlarm = true;
        m_ocSrc   = ocSourceText(src);    // "1,3" 这样的桥号列表, 图标小字/状态栏/日志共用
        if (!wasAlarm) {
            m_ocRedStartMs = QDateTime::currentMSecsSinceEpoch();
        }
        ++m_ocSeq;                        // 新的一轮: 让"推迟变绿"作废
        updateSystemIcon();

        /*
         * ⚠ 只有"值真的变了"才写日志: 下位机会对同一状态重复上报(保持窗口期间会连发),
         *   如果每次都写, 接收区会被"过流告警"刷屏, 真正有用的那几条反而看不见。
         */
        if (!wasAlarm) {
            appendLog(QStringLiteral("⚠ 过流(OC)告警 0xC0: src=%1 → **%2 号桥**过保, 已关桥保护")
                          .arg(byteHex(src))
                          .arg(m_ocSrc),
                      kColorErr);
        } else if (oldSrc != m_ocSrc) {
            /* 过流还在, 只是受影响的桥变了(例如又有一路跳) */
            appendLog(QStringLiteral("⚠ 过流范围变化 0xC0: src=%1 → 现在 %2 号桥过保")
                          .arg(byteHex(src))
                          .arg(m_ocSrc),
                      kColorErr);
        }
        return;
    }
}

/*!
 * 把和"链路状态"有关的几项一起复位: 制动按钮文字、两个圆点、跨包接收缓冲。
 * 连上 / 断开 / 出错三处都调它, 免得三份重复代码改漏一处(踩过: 只改了断开那份,
 * 结果重连之后按钮还写着"关闭制动")。
 */
void Widget::resetBinaryState()
{
    m_brakeOn = false;
    m_lsSeen  = false;
    m_lsMask  = 0;
    m_keySeen = false;
    m_keyMask = 0;
    m_ocAlarm = false;
    m_ocSrc   = QStringLiteral("0");

    /* 过流"红点计时"也要复位: 否则重连之后第一条 src=0 会被上一轮的计时影响 */
    m_ocRedStartMs = 0;
    ++m_ocSeq;

    m_rxBuffer.clear();      // 上一条链路留下的半行/半个报文全部作废

    updateBrakeButton();
    updatePositionIcon();
    updateSystemIcon();
}

/* ========================================================================== */
/*  数据解析                                                                   */
/* ========================================================================== */

void Widget::handleLine(const QString &line)
{
    /* ⚠ 这里原来第一步是 parseOcReply(line) —— 解析纹理输出里 PWMFAULT 的回复。
     *   下位机 PWM 命令表(含 PWMFAULT)已删, 过流改成它**主动上报** 0xC0 二进制报文,
     *   由 handleBinaryFrame() 处理, 所以这一段整块删掉了。
     */

    /* ---- 0b. LED 的回复: 只同步界面(勾选框/频率), **不吞掉这一行** ---------
     * 过流是特例(巡检每秒都回同样的内容, 全写出来会把警告顶走), 而 LED 回复都是
     * 用户自己点的命令换来的, 该照常显示在接收区里。
     * -------------------------------------------------------------------- */
    (void)parseLedReply(line);

    /* ---- 1. 先判断是不是遥测行 ------------------------------------------- */
    double                      x     = 0.0;
    bool                        hasX  = false;
    QVector<QPair<int, double>> values;

    const bool isTelemetry = parseTelemetry(line, &x, &values, &hasX) && !values.isEmpty();

    /*
     * 遥测行默认不进接收区: 板子 10 Hz 推数据的话, 命令和回复会被瞬间刷走,
     * 改了占空比在窗口里什么反馈都看不到。数据照样进曲线, 只是不刷屏。
     */
    if (!isTelemetry || ui->checkBoxShowTelemetry->isChecked()) {
        const QString upper = line.toUpper();
        QColor color = kColorRx;

        if (upper.contains(QLatin1String("ERR")) || upper.contains(QLatin1String("FAULT"))
            || upper.contains(QLatin1String("FAIL")) || upper.contains(QLatin1String("UNKNOWN"))
            || upper.contains(QLatin1String("STILL LOW"))) {
            color = kColorErr;
        } else if (upper.contains(QLatin1String("PASS")) || upper.contains(QLatin1String("PONG"))
                   || upper.contains(QLatin1String(" OK")) || upper.startsWith(QLatin1String("PWM ACK"))) {
            color = kColorOk;
        } else if (line.startsWith(QLatin1Char('['))) {
            color = kColorInfo;                  // 板子主动广播
        }

        appendLog(QStringLiteral("<< %1").arg(line), color);
    } else if (!m_telemetryNoticeShown) {
        m_telemetryNoticeShown = true;
        appendLog(QStringLiteral("· 遥测数据正在进曲线(默认不刷接收区; 想看原始行请在\"控制参数预设\"里"
                                 "勾选\"接收区显示遥测数据行\")"),
                  kColorGray);
    }

    /* ---- 2. 收进曲线 ----------------------------------------------------- */
    /*
     * 实时遥测要点过"开始采集"才画(不然没点采集就开始画, 用户会以为程序自己在跑)。
     * 但 **eMMC 回放(EMCD)不受这个限制** —— 用户是主动点了"拉取并回放"才拿到的数据,
     * 想看的正是它, 这时候还要求先点"开始采集"是说不通的。
     */
    if (!isTelemetry || !m_acquiring)
        return;

    if (!hasX)
        x = double(m_sampleIndex++);

    feedSample(x, values);
}

/*!
 * 解析一行遥测。认得的格式(关键字不分大小写, 行首 [tag] 会被去掉):
 *
 *   DATA <t> <i1> <i2> <i3> <i4> <i5> <speed> [vbus]
 *                                               第 1 个是横坐标(秒), 后面按 CurveId 的顺序
 *                                               进各通道。板子现在发 8 个数(带母线电压),
 *                                               只发 7 个(老固件)也照样认 —— 少的那条
 *                                               曲线留空, 不会整体错位。
 *                                               ⚠ 前 5 个是**线圈电流(A)**(下位机按
 *                                               I = (1.5V - 2.5)*10 换算过), 第 7 个是
 *                                               CH6 的引脚电压(V), 第 8 个是母线电压(V,
 *                                               下位机已按 11:1 还原)。
 *   CUR|CURRENTS <i1> <i2> <i3> <i4> <i5>       5 个线圈电流, 横坐标 = 采样序号
 *   COIL <n> <i>                                单个线圈, n = 1..5
 *   SPEED|VEL <v>                               只有速度
 *   i1=.. i2=.. ... i5=.. speed=/vel= t=/time=/x=   key=value 形式
 *   裸数字行(整行没有字母)                       5 个数 = 5 个线圈; 6 个数 = 5 线圈 + 速度;
 *                                               其他个数按通道顺序填, 多的丢掉
 *
 * 一个数值都解析不出来就返回 false —— 宁可什么都不画, 也不画错。
 */
bool Widget::parseTelemetry(const QString &line,
                            double                     *x,
                            QVector<QPair<int, double>> *values,
                            bool                       *hasExplicitX)
{
    if (x == nullptr || values == nullptr || hasExplicitX == nullptr)
        return false;

    values->clear();
    *hasExplicitX = false;
    *x = 0.0;

    QString s = stripLeadingTags(line);
    if (s.isEmpty())
        return false;

    // 分隔符: 空格 / 逗号 / 分号(板子的行里逗号当分隔符用)
    s.replace(QLatin1Char(','), QLatin1Char(' '));
    s.replace(QLatin1Char(';'), QLatin1Char(' '));

    const QStringList tokens = s.split(splitWhitespace(), Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        return false;

    /*! 把 tokens[first..] 里能当数值解析的 token 都收下来(带单位的 "12.3A" 也认)。 */
    const auto numbersFrom = [&tokens](int first) {
        QVector<double> nums;
        for (int i = first; i < tokens.size(); ++i) {
            double v = 0.0;
            if (tokenToDouble(tokens.at(i), &v))
                nums.append(v);
        }
        return nums;
    };

    const QString head = tokens.first().toUpper();

    /* ---- 1. DATA <t> <i1> <i2> <i3> <i4> <i5> <speed> [vbus] -------------------- */
    if (head == QLatin1String("DATA")) {
        /*
         * EMCD 是"从 eMMC 回放出来的记录": 板子把卡上的 CSV 行原样发回来, 前面加个
         * EMCD 头。字段顺序和 DATA 完全一样, 所以这里共用一套解析 —— 回放的数据和
         * 实时数据走同一条路径进曲线, 不会出现"两种数据画法不一样"的问题。
         */
        const QVector<double> nums = numbersFrom(1);
        if (nums.size() < 2)          // 只有头或者只有时间: 没有可画的值
            return false;

        *x            = nums.first();
        *hasExplicitX = true;

        for (int i = 1; i < nums.size() && (i - 1) < CurveCount; ++i)
            values->append(qMakePair(i - 1, nums.at(i)));

        return !values->isEmpty();
    }

    /* ---- 2. CUR / CURRENTS <i1> <i2> <i3> <i4> <i5> ---------------------- */
    if (head == QLatin1String("CUR") || head == QLatin1String("CURRENTS")) {
        const QVector<double> nums = numbersFrom(1);
        for (int i = 0; i < nums.size() && i < 5; ++i)
            values->append(qMakePair(CurveCoil1 + i, nums.at(i)));

        return !values->isEmpty();
    }

    /* ---- 3. COIL <n> <v>: 单个线圈, n = 1..5 -> 通道 CurveCoil1 + n - 1 --- */
    if (head == QLatin1String("COIL")) {
        const QVector<double> nums = numbersFrom(1);
        if (nums.size() < 2)
            return false;

        const double raw = nums.at(0);
        const int    n   = int(std::lround(raw));
        if (n < 1 || n > 5 || std::fabs(raw - n) > 1e-6)     // 编号必须是 1..5 的整数
            return false;

        values->append(qMakePair(CurveCoil1 + n - 1, nums.at(1)));
        return true;
    }

    /* ---- 4. SPEED / VEL <v>: 只有速度通道 -------------------------------- */
    if (head == QLatin1String("SPEED") || head == QLatin1String("VEL")) {
        const QVector<double> nums = numbersFrom(1);
        if (nums.isEmpty())
            return false;

        values->append(qMakePair(CurveSpeed, nums.first()));
        return true;
    }

    /* ---- 5. key=value 形式: t=1.23 i1=0.5 ... i5=0.4 speed=400 ----------- */
    bool                   sawPair = false;
    QHash<QString, double> pairs;

    for (const QString &tok : tokens) {
        const int eq = tok.indexOf(QLatin1Char('='));
        if (eq <= 0 || eq >= tok.size() - 1)
            continue;

        double v = 0.0;
        if (!tokenToDouble(tok.mid(eq + 1), &v))
            continue;

        pairs.insert(tok.left(eq).toLower(), v);
        sawPair = true;
    }

    if (sawPair) {
        for (auto it = pairs.constBegin(); it != pairs.constEnd(); ++it) {
            const QString key = it.key();

            if (key == QLatin1String("t") || key == QLatin1String("time") || key == QLatin1String("x")) {
                *x            = it.value();
                *hasExplicitX = true;
            } else if (key == QLatin1String("speed") || key == QLatin1String("vel")) {
                values->append(qMakePair(CurveSpeed, it.value()));
            } else if (key.size() == 2 && key.at(0) == QLatin1Char('i')
                       && key.at(1) >= QLatin1Char('1') && key.at(1) <= QLatin1Char('5')) {
                // i1..i5 -> 线圈 1..5
                const int n = key.at(1).digitValue();
                values->append(qMakePair(CurveCoil1 + n - 1, it.value()));
            }
        }

        if (!values->isEmpty())
            return true;
    }

    /* ---- 6. 裸数字行: "1.2 0.5 0.5 0.4 0.4 0.3 398.5" -------------------- */
    if (isPureNumericLine(s)) {
        QVector<double> nums;
        for (const QString &tok : tokens) {
            double v = 0.0;
            if (tokenToDouble(tok, &v))
                nums.append(v);
        }

        if (nums.isEmpty())
            return false;

        if (nums.size() == 5 || nums.size() == 6 || nums.size() == 7) {
            /* 5 个 = 5 个线圈; 6 个 = +速度; 7 个 = +速度+母线电压。
             * 这三种"刚好是前缀"的情况按名字填, 比"按通道顺序"更不容易错 —— 用户
             * 少贴一路电压时也希望速度还是画在速度那条线上, 而不是因为少一个数就
             * 整体前移错位。 */
            for (int i = 0; i < 5; ++i)
                values->append(qMakePair(CurveCoil1 + i, nums.at(i)));

            if (nums.size() >= 6)
                values->append(qMakePair(CurveSpeed, nums.at(5)));

            if (nums.size() >= 7)
                values->append(qMakePair(CurveVoltage, nums.at(6)));
        } else {
            // 其他个数: 按通道顺序填, 多出来的丢掉
            for (int i = 0; i < nums.size() && i < CurveCount; ++i)
                values->append(qMakePair(i, nums.at(i)));
        }

        return !values->isEmpty();
    }

    return false;
}

void Widget::feedSample(double x, const QVector<QPair<int, double>> &values)
{
    if (values.isEmpty())
        return;

    Sample sample;
    sample.x = x;

    for (const QPair<int, double> &pair : values) {
        const int channel = pair.first;
        if (channel < 0 || channel >= CurveCount)
            continue;

        sample.value[channel] = pair.second;
        sample.valid[channel] = true;
    }

    m_cache.append(sample);

    /*
     * ⚠ 缓存到顶时的裁剪必须**一次多删一点**: 原来写的是
     *     `if (size > kMaxCacheRows) m_cache.remove(0, size - kMaxCacheRows);`
     *   —— 到顶之后每来一条采样都要把 50 万行整体前移一次(几十 MB 的 memmove),
     *   100 Hz 遥测下就是每秒几个 GB 的内存搬移, 界面直接卡死。
     *   留 10% 余量后, 平均每 5 万条才搬一次(上限仍然是 kMaxCacheRows)。
     */
    if (m_cache.size() > kMaxCacheRows) {
        const int keep = kMaxCacheRows - kMaxCacheRows / 10;
        m_cache.remove(0, m_cache.size() - keep);
    }

    for (int i = 0; i < CurveCount; ++i) {
        if (sample.valid[i])
            plotOfCurve(i)->appendPoint(localOfCurve(i), x, sample.value[i]);
    }

    // 遥测行不刷接收区了, 所以在状态标签上给个"确实在采"的活反馈
    updateStatusLabel();
}

/* ========================================================================== */
/*  本地模拟                                                                   */
/* ========================================================================== */

void Widget::onDemoToggled(bool on)
{
    if (on) {
        m_demoT       = 0.0;
        m_sampleIndex = 0;
        m_demoTimer->start(ui->spinBoxDemoPeriod->value());
        appendLog(QStringLiteral("▲ 本地模拟已开启(不连板子, 仅用于演示曲线)"), kColorInfo);
    } else {
        m_demoTimer->stop();
        appendLog(QStringLiteral("▼ 本地模拟已关闭"), kColorGray);
    }
}

void Widget::onDemoTick()
{
    const double dt = ui->spinBoxDemoPeriod->value() / 1000.0;
    m_demoT += dt;

    /*
     * 每 8 s 重来一次, 这样窗口里总能看到一段完整的"加速 -> 制动减速"过程。
     *
     * ⚠ 回卷时必须把动子那两条曲线的数据清掉: 位移会从末尾(~144 mm)一下子跳回 480 mm,
     *   10 ms 差分算出来就是一个 500 mm/s 的**假尖峰**(真板子当然不会跳)。清数据之后
     *   曲线从新的一段重新画, 不会连出一条竖直的假线。线圈那几条不受影响, 照常滚。
     */
    if (m_demoT > 8.0) {
        m_demoT = 0.0;

        plot(PlotMover)->clearCurve(localOfCurve(CurveSpeed));
        plot(PlotMover)->clearCurve(localOfCurve(CurveLaser));
    }

    /*
     * ---- 动子: 一段完整的制动过程, 严格"先加速、后减速" --------------------
     *
     *   [0, kAccel] 匀加速到峰值速度(线圈电流还在爬升)
     *   (kAccel, 8] 制动力矩把它刹回 0(指数衰减)
     *
     * ⚠ 位移**就是这条速度的积分**(下面 travelled 是解析积分, 不是另编一条曲线),
     *   所以两条曲线一定互相印证: |位移的斜率| = 速度, 而且位移一路**减小**
     *   (动子相对传感器越来越近)。之前那版是"速度直接指数衰减 + 位移反向累加",
     *   既没有加速段、位移还在变大、两者也差了个 tau 倍 —— 看着就像算错了。
     */
    const double kAccel    = 1.0;      /* 加速段时长(s)                 */
    const double kVpeak    = 280.0;    /* 峰值速度(mm/s)                */
    const double kBrakeTau = 0.7;      /* 减速段(制动)时间常数(s)       */

    double speed;                      /* mm/s, 速率(向传感器靠近时取正) */
    double travelled;                  /* mm, 已走过的路程 = ∫speed·dt  */

    if (m_demoT <= kAccel) {
        speed     = kVpeak * m_demoT / kAccel;
        travelled = 0.5 * kVpeak * m_demoT * m_demoT / kAccel;
    } else {
        const double td = m_demoT - kAccel;

        speed     = kVpeak * std::exp(-td / kBrakeTau);
        travelled = 0.5 * kVpeak * kAccel
                  + kVpeak * kBrakeTau * (1.0 - std::exp(-td / kBrakeTau));
    }

    const double travel = 480.0 - travelled;   /* 位移一直减小(动子靠近传感器) */

    /*
     * 5 路线圈也给出来 —— 真板子那一行是
     *     DATA <t> <i1> <i2> <i3> <i4> <i5> <动子速度> <vbus> <动子位移>
     * 其中线圈那 5 个数是**线圈电流(A)**(下位机按 I = (1.5V - 2.5)*10 换算过),
     * 动子速度是 mm/s, vbus 是母线电压(V, 已按 11:1 还原), 动子位移是 mm。
     * 模拟里按制动的样子造: 电流随制动过程爬升 + 不同相位的纹波, 这样"勾选哪几条曲线"
     * "纵轴自动缩放" "CSV 导出"这些都能在没有板子的时候先试出来。
     */
    const double base = 12.0 * (1.0 - std::exp(-m_demoT / m_demoTau));   /* A */

    QVector<QPair<int, double>> values;
    values << qMakePair(CurveCoil1, base        + 0.15 * std::sin(m_demoT * 31.0))
           << qMakePair(CurveCoil2, base * 0.95 + 0.15 * std::sin(m_demoT * 29.0 + 1.0))
           << qMakePair(CurveCoil3, base * 1.05 + 0.15 * std::sin(m_demoT * 33.0 + 2.0))
           << qMakePair(CurveCoil4, base * 1.10 + 0.15 * std::sin(m_demoT * 27.0 + 3.0))
           << qMakePair(CurveCoil5, base * 0.90 + 0.15 * std::sin(m_demoT * 35.0 + 4.0))
           << qMakePair(CurveSpeed, speed)
           /* 母线电压: 制动时被线圈拉低一点, 松开的瞬间回弹 —— 光看数字看不出对错,
             * 但能确认第 7 条曲线在图例/自动缩放/CSV 里都接对了。 */
           << qMakePair(CurveVoltage, 24.0 - 1.8 * (base / 12.0) + 0.05 * std::sin(m_demoT * 7.0))
           /* 动子位移: 第 9 个数, 画在右图; 就是上面那条速度的积分 */
           << qMakePair(CurveLaser, travel);

    const double x = double(m_sampleIndex++);
    feedSample(x, values);

    if (m_sampleIndex % 10 == 0) {
        appendLog(QStringLiteral("[sim] t=%1 s   速度=%2 mm/s   位移=%3 mm   线圈1=%4 A")
                      .arg(m_demoT, 0, 'f', 2)
                      .arg(speed, 0, 'f', 1)
                      .arg(travel, 0, 'f', 1)
                      .arg(base, 0, 'f', 3),
                  kColorGray);
    }
}

/* ========================================================================== */
/*  显示设置 / 预设                                                            */
/* ========================================================================== */

void Widget::onApplyDisplaySettings()
{
    const double lo = ui->doubleSpinBoxYMin->value();
    const double hi = ui->doubleSpinBoxYMax->value();

    if (hi <= lo) {
        appendLog(QStringLiteral("× 纵轴上限必须大于下限"), kColorErr);
        return;
    }

    for (int p = 0; p < PlotCount; ++p) {
        PlotWidget *w = plot(PlotId(p));

        w->setAxisTitles(ui->lineEditXTitle->text());

        /*
         * 纵轴自动缩放打开时下面的 setYRange() 照样发, 但 PlotWidget 会忽略它
         * (那时候上下限是每帧按显示中的曲线算的) —— 这里发一下不会有副作用, 关掉
         * 自动缩放之后手动区间立刻就是用户填的这两个数。
         */
        w->setYRange(lo, hi);
        w->setXSpan(ui->spinBoxXSpan->value());
        w->resetView();
    }

    appendLog(QStringLiteral("√ 显示设置已应用: 纵轴 %1..%2, 横轴窗口 %3 点")
                  .arg(lo).arg(hi).arg(ui->spinBoxXSpan->value()),
              kColorOk);
}

/*!
 * 第一页 6 个勾选框 → PlotWidget 的显示集合。
 *
 * 没勾的曲线只是不画、不进图例, 数据还在缓存区和曲线里(再勾上就回来了)。
 * "想看几条"就是勾几个框: 只留一个框就是单条, 全勾就是全画, 任意组合都行;
 * 纵轴自动缩放会跟着"当前显示中的曲线"重新适配, 所以只显示某一路时它是撑满的。
 *
 * ⚠ 曲线分在左右两张图上, 所以每条要交回**它自己那张图**、用**局部序号**(见 widget.h)。
 */
void Widget::syncCurveVisibility()
{
    const QList<QCheckBox *> boxes = curveBoxes();

    for (int i = 0; i < boxes.size() && i < CurveCount; ++i)
        plotOfCurve(i)->setCurveVisible(localOfCurve(i), boxes.at(i)->isChecked());
}

/*!
 * 曲线显示勾选框, **按 CurveId 的顺序**。
 *
 * 集中在一个函数里返回: 之前 registerCurves / connectSignals / syncCurveVisibility /
 * loadSettings / saveSettings 五处各抄了一份同样的大括号数组, 加一条曲线要改五个
 * 地方 —— 少改一处就是"界面多了个勾选框, 但它存不下来 / 点了没反应"。
 * 加曲线时只改这里(和 CurveId、.ui、kCurves[])。
 *
 * ⚠ 返回顺序是 **CurveId 顺序**, 不是界面上从左到右的顺序 —— 界面上把"动子"那两个框
 *   挪到了右边一组(和左右两张图对应), 但 CurveId 必须跟板子 DATA 行的字段顺序一致,
 *   所有用到的地方(存/取 view/showN、syncCurveVisibility)走的都是这个返回值, 所以安全。
 */
QList<QCheckBox *> Widget::curveBoxes() const
{
    return { ui->checkBoxCoil1, ui->checkBoxCoil2, ui->checkBoxCoil3,
             ui->checkBoxCoil4, ui->checkBoxCoil5, ui->checkBoxSpeed,
             ui->checkBoxVoltage, ui->checkBoxLaser };
}

/*! 左/右那张图。 */
PlotWidget *Widget::plot(PlotId id) const
{
    return (id == PlotMover) ? ui->plotWidgetMover : ui->plotWidget;
}

/*! 这条曲线画在哪张图上(表见 widget.h 的 m_curvePlot[])。 */
PlotWidget *Widget::plotOfCurve(int curveId) const
{
    if (curveId < 0 || curveId >= CurveCount)
        return ui->plotWidget;

    return plot(m_curvePlot[curveId]);
}

/*! 这条曲线在它那张图里的局部序号 —— appendPoint / setCurveVisible 必须用这个。 */
int Widget::localOfCurve(int curveId) const
{
    if (curveId < 0 || curveId >= CurveCount)
        return 0;

    return m_curveLocal[curveId];
}

/*! 曲线名(CSV 表头): 问它所在的那张图要, 名字仍然只有 kCurves[] 一处定义。 */
QString Widget::curveName(int curveId) const
{
    PlotWidget *w = plotOfCurve(curveId);

    return w->curveName(localOfCurve(curveId));
}

/*!
 * 铺满某一张图 / 还原成左右双图。
 *
 * 做法就是**把另一张图所在的那一列整个隐藏**: 那条水平分隔条里只剩一列, Qt 会自动让它
 * 占满整个绘图区 —— 不用改任何几何计算, 也不用碰窗口本身。所以它是"铺满绘图区", 不是
 * 软件窗口全屏: 接收区和右侧控制栏都还在。
 *
 * 还原时把分隔条按 1:1 重新分配; 按钮文字/勾选状态一并同步(用 QSignalBlocker 免得
 * setChecked 又回调进来)。
 *
 * @param id : PlotElectrical / PlotMover; 其它值(含 -1) = 还原。
 */
void Widget::setMaximizedPlot(int id)
{
    const bool leftOn  = (id == int(PlotElectrical));
    const bool rightOn = (id == int(PlotMover));

    if (!leftOn && !rightOn)
        m_maxPlot = -1;
    else
        m_maxPlot = id;

    ui->widgetPlotLeft->setVisible(!rightOn);
    ui->widgetPlotRight->setVisible(!leftOn);

    {
        const QSignalBlocker blockLeft(ui->pushButtonMaxLeft);
        const QSignalBlocker blockRight(ui->pushButtonMaxRight);

        ui->pushButtonMaxLeft->setChecked(leftOn);
        ui->pushButtonMaxRight->setChecked(rightOn);
        ui->pushButtonMaxLeft->setText(leftOn ? QStringLiteral("还原") : QStringLiteral("铺满"));
        ui->pushButtonMaxRight->setText(rightOn ? QStringLiteral("还原") : QStringLiteral("铺满"));
    }

    if (m_maxPlot < 0) {
        ui->splitterPlots->setSizes(QList<int>() << 1 << 1);
    } else {
        /* 铺满那张图里的曲线重新适配一下窗口(画面尺寸刚变过) */
        plot(PlotId(m_maxPlot))->update();
    }
}

void Widget::onMaxLeftToggled(bool on)
{
    setMaximizedPlot(on ? int(PlotElectrical) : -1);
}

void Widget::onMaxRightToggled(bool on)
{
    setMaximizedPlot(on ? int(PlotMover) : -1);
}

/*! 某张图被 Ctrl+双击: 它已经铺满就还原, 否则铺满它。 */
void Widget::onMaximizeRequested()
{
    const PlotWidget *src = qobject_cast<PlotWidget *>(sender());
    int               id  = int(PlotElectrical);

    if (src == ui->plotWidgetMover)
        id = int(PlotMover);

    setMaximizedPlot((m_maxPlot == id) ? -1 : id);
}

/*! Esc 退出"铺满"(鼠标在别的控件上也能用)。 */
void Widget::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Escape) && (m_maxPlot >= 0)) {
        setMaximizedPlot(-1);
        event->accept();
        return;
    }

    QWidget::keyPressEvent(event);
}

/*!
 * "纵轴自动缩放(按当前显示的曲线)"。
 * 打开时纵轴上下限由 PlotWidget 每帧按显示中的曲线算, 手动的两个数字框就不起作用了,
 * 所以顺手禁用掉, 免得用户以为改了没反应。这里不写日志: loadSettings() 在欢迎语之前
 * 就会调用它, 启动时凭空多一行"已开启"看着像出错。
 */
void Widget::onAutoScaleYToggled(bool on)
{
    ui->doubleSpinBoxYMin->setEnabled(!on);
    ui->doubleSpinBoxYMax->setEnabled(!on);

    for (int p = 0; p < PlotCount; ++p)
        plot(PlotId(p))->setAutoScaleY(on);
}

/**
 * Shift+滚轮缩了纵轴: 那张 PlotWidget 已经把自动缩放关掉、把当时的区间转成手动区间。
 * 这里把勾选框同步取消, 并把新区间回填到两个数字框 —— 否则界面显示的上下限
 * 和实际画出来的对不上, 再点一次"应用显示设置"还会跳回旧范围。
 *
 * 两张图都会发这个信号: 用 sender() 认出是哪一张, 回填它自己的区间(左右量级本来就不同)。
 */
void Widget::onAutoScaleYTakenOver()
{
    const PlotWidget *src = qobject_cast<PlotWidget *>(sender());

    if (src == nullptr)
        src = ui->plotWidget;

    ui->checkBoxAutoScaleY->setChecked(false);          /* 会触发 onAutoScaleYToggled(false) */
    ui->doubleSpinBoxYMin->setValue(src->yMin());
    ui->doubleSpinBoxYMax->setValue(src->yMax());
}

/**
 * 制动强度下拉: 低/中/高 三档, 选中就往下位机发对应的**单字节自定义命令**。
 *
 * ⚠ 和改造前的区别(别改回去): 以前这个下拉只是"给占空比数字框填个数", 强度是靠文本命令
 *   PWMDUTY 下发的; 现在下位机 PWM 那套文本命令已经删干净, 制动强度改由 0xA2/0xA3/0xA4
 *   三个字节直接告诉下位机, 由它把"档位 → 占空比(30/50/70%)"定死在 brake_ctl_app 里。
 *   所以这里**只管发档位**, 不再碰任何占空比控件(那些控件已经随 PWM 模块一起删了)。
 */
void Widget::onBrakingLevelChanged(int index)
{
    if (index < 0 || index > 2) {
        return;                       // 只有三档, 越界当没发生
    }

    static const unsigned char kLevelCmd[3] = { kBinBrakeLevelLow, kBinBrakeLevelMid,
                                                kBinBrakeLevelHigh };
    static const char *const   kLevelName[3] = { "低 (30%)", "中 (50%)", "高 (70%)" };

    if (!isConnected()) {
        appendLog(QStringLiteral("· 制动强度选了「%1」, 但没连板子 —— 连上后再点一次才会下发")
                      .arg(QString::fromUtf8(kLevelName[index])),
                  kColorGray);
        return;
    }

    sendByte(kLevelCmd[index], QStringLiteral("制动强度 %1").arg(QString::fromUtf8(kLevelName[index])));
}

QString Widget::exportCsv()
{
    if (m_cache.isEmpty())
        return QString();

    const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/data");
    if (!QDir().mkpath(dir))
        return QString();

    const QString path = QStringLiteral("%1/brake_%2.csv")
                             .arg(dir, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out.setGenerateByteOrderMark(true);          // 让 Excel 也能认中文表头

    /* 表头直接问曲线要名字(取自它所在的那张图): 名字只有 kCurves[] 一处定义,
     * 不会和界面不一致。曲线分在两张图上, 所以要走 curveName(i) 而不是直接查某一张图。 */
    out << QStringLiteral("time");
    for (int i = 0; i < CurveCount; ++i)
        out << ',' << curveName(i);
    out << '\n';

    for (const Sample &s : m_cache) {
        out << QString::number(s.x, 'f', 3);
        for (int i = 0; i < CurveCount; ++i) {
            out << ',';
            if (s.valid[i])
                out << QString::number(s.value[i], 'f', 4);
        }
        out << '\n';
    }

    out.flush();
    file.close();

    return path;
}

/* ========================================================================== */
/*  配置持久化                                                                 */
/* ========================================================================== */

void Widget::loadSettings()
{
    QSettings settings = appSettings();

    ui->lineEditIp->setText(settings.value(QStringLiteral("net/ip"),
                                           ui->lineEditIp->text()).toString());
    ui->spinBoxPort->setValue(settings.value(QStringLiteral("net/port"),
                                             ui->spinBoxPort->value()).toInt());
    ui->comboBoxLevel->setCurrentIndex(settings.value(QStringLiteral("net/level"),
                                                      ui->comboBoxLevel->currentIndex()).toInt());

    /* ---- 直线电机(发射小车) ---------------------------------------------- */
    ui->comboBoxLaunchSpeed->setCurrentIndex(settings.value(QStringLiteral("motor/launchSpeedIndex"),
                                                            ui->comboBoxLaunchSpeed->currentIndex()).toInt());

    /* ---- 4 路呼吸灯 ------------------------------------------------------- */
    ui->spinBoxLedFreq->setValue(settings.value(QStringLiteral("led/freq"),
                                                ui->spinBoxLedFreq->value()).toInt());
    setLedMaskToUi(settings.value(QStringLiteral("led/mask"), ledMaskFromUi()).toInt());

    /* ---- 制动占空比 / 死区(预设页; 只是界面初值, 要用再点/拖动才会下发) --- */
    ui->sliderBrakeDuty->setValue(settings.value(QStringLiteral("brake/duty"),
                                                 ui->sliderBrakeDuty->value()).toInt());
    ui->labelBrakeDutyValue->setText(QStringLiteral("%1 %").arg(ui->sliderBrakeDuty->value()));
    ui->spinBoxBrakeDeadNs->setValue(settings.value(QStringLiteral("brake/deadNs"),
                                                    ui->spinBoxBrakeDeadNs->value()).toInt());

    /* 占空比 0 ⇒ 死区必须 0(加载时就按这条规则把界面摆对, 不往板子发) */
    m_deadNsRemembered = (ui->spinBoxBrakeDeadNs->value() > 0) ? ui->spinBoxBrakeDeadNs->value()
                                                               : 500;
    applyDutyDeadCoupling(ui->sliderBrakeDuty->value());

    /* ---- eMMC ------------------------------------------------------------- */

    /*
     * 纵轴范围换过一次键名: 线圈那 5 条从"ADC 电压 V"改成"电流 A"(下位机换算),
     * 旧 ini 里存的是按电压定的范围(-5 ~ 5), 拿它当电流轴用会把几十安培的信号压平,
     * 看着像"改动没生效"。换键 = 旧范围作废一次, 新的电流默认值(-5 ~ 60)立刻生效;
     * 之后用户自己调的范围照常记住。键名里的 A 就是"ampere 版"的意思。
     */
    ui->lineEditXTitle->setText(settings.value(QStringLiteral("view/xTitle"),
                                               ui->lineEditXTitle->text()).toString());
    ui->doubleSpinBoxYMin->setValue(settings.value(QStringLiteral("view/yMinA"),
                                                   ui->doubleSpinBoxYMin->value()).toDouble());
    ui->doubleSpinBoxYMax->setValue(settings.value(QStringLiteral("view/yMaxA"),
                                                   ui->doubleSpinBoxYMax->value()).toDouble());
    ui->spinBoxXSpan->setValue(settings.value(QStringLiteral("view/xSpan"),
                                              ui->spinBoxXSpan->value()).toInt());

    /* ---- 第一页: 曲线的显示勾选框 + 纵轴自动缩放 -------------------------- */
    const QList<QCheckBox *> boxes = curveBoxes();
    for (int i = 0; i < boxes.size() && i < CurveCount; ++i) {
        /*
         * 键名从 view/curve0..5 换成了 view/show0..5。
         *
         * 为什么换: 默认值从"6 条全显示"改成了"只显示速度曲线"。要是继续用旧键,
         * 以前跑过这个程序留下的 ini 里存着"6 个都勾上", 会把新默认值**整个盖掉** ——
         * 启动一看还是 6 条曲线, 像是改动没生效。换键 = 旧的那份选择作废一次,
         * 新默认立刻生效; 之后用户自己勾的选择照常记住。
         *
         * 注意 2026/10 加了第 7 条(电压): 它的键是 view/show6, ini 里没有, 所以直接
         * 用 .ui 的默认值(不勾)。不需要再换一次键 —— 换键是为了让**改了默认值的那些**
         * 生效, 新增的曲线没有旧值要作废。
         */
        const QString key = QStringLiteral("view/show%1").arg(i);
        boxes.at(i)->setChecked(settings.value(key, boxes.at(i)->isChecked()).toBool());
    }
    // 勾选状态和 .ui 里的默认值一样时 toggled 不会发, 这里再对齐一次
    syncCurveVisibility();

    // setChecked 会触发 toggled -> onAutoScaleYToggled(), 那里会把状态应用到控件上
    ui->checkBoxAutoScaleY->setChecked(settings.value(QStringLiteral("view/autoScaleY"),
                                                      ui->checkBoxAutoScaleY->isChecked()).toBool());

    /* ---- 接收区是否显示遥测行 ------------------------------------------- */
    /* ⚠ 原来这里还恢复"过流巡检开关/周期"和"实时下发占空比"—— 那三个控件随 PWM
     *   模块一起删了(过流现在由下位机 0xC0 主动上报, 不需要巡检; 占空比由档位决定)。 */
    ui->checkBoxShowTelemetry->setChecked(settings.value(QStringLiteral("view/showTelemetry"),
                                                         ui->checkBoxShowTelemetry->isChecked()).toBool());

    // 显示设置立刻生效(不再要求用户点一次"应用")
    for (int p = 0; p < PlotCount; ++p) {
        PlotWidget *w = plot(PlotId(p));

        w->setAxisTitles(ui->lineEditXTitle->text());
        w->setYRange(ui->doubleSpinBoxYMin->value(), ui->doubleSpinBoxYMax->value());
        w->setXSpan(ui->spinBoxXSpan->value());
    }

    setLinkState(false, QStringLiteral("未连接"));

    const int w = settings.value(QStringLiteral("window/w"), 0).toInt();
    const int h = settings.value(QStringLiteral("window/h"), 0).toInt();
    if (w > 400 && h > 300)
        resize(w, h);
}

void Widget::saveSettings()
{
    QSettings settings = appSettings();

    settings.setValue(QStringLiteral("net/ip"), ui->lineEditIp->text());
    settings.setValue(QStringLiteral("net/port"), ui->spinBoxPort->value());
    settings.setValue(QStringLiteral("net/level"), ui->comboBoxLevel->currentIndex());


    settings.setValue(QStringLiteral("motor/launchSpeedIndex"), ui->comboBoxLaunchSpeed->currentIndex());

    settings.setValue(QStringLiteral("led/freq"), ui->spinBoxLedFreq->value());
    settings.setValue(QStringLiteral("led/mask"), ledMaskFromUi());

    settings.setValue(QStringLiteral("brake/duty"), ui->sliderBrakeDuty->value());
    settings.setValue(QStringLiteral("brake/deadNs"), ui->spinBoxBrakeDeadNs->value());


    /* 键名带 A = 电流版, 和 loadSettings 里那段说明对应 */
    settings.setValue(QStringLiteral("view/xTitle"), ui->lineEditXTitle->text());
    settings.setValue(QStringLiteral("view/yMinA"), ui->doubleSpinBoxYMin->value());
    settings.setValue(QStringLiteral("view/yMaxA"), ui->doubleSpinBoxYMax->value());
    settings.setValue(QStringLiteral("view/xSpan"), ui->spinBoxXSpan->value());

    const QList<QCheckBox *> boxes = curveBoxes();
    for (int i = 0; i < boxes.size() && i < CurveCount; ++i)
        settings.setValue(QStringLiteral("view/show%1").arg(i), boxes.at(i)->isChecked());

    settings.setValue(QStringLiteral("view/autoScaleY"), ui->checkBoxAutoScaleY->isChecked());

    settings.setValue(QStringLiteral("view/showTelemetry"), ui->checkBoxShowTelemetry->isChecked());

    settings.setValue(QStringLiteral("window/w"), width());
    settings.setValue(QStringLiteral("window/h"), height());

    settings.sync();
}

void Widget::closeEvent(QCloseEvent *event)
{
    ui->checkBoxDemo->setChecked(false);
    m_demoTimer->stop();


    if (isConnected())
        m_socket->abort();

    saveSettings();
    event->accept();
}
