#include "plotwidget.h"

#include <QAction>         /* 右键菜单项(游标/复位) */
#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QElapsedTimer>   /* 量单帧耗时, 供自适应帧率/笔画粗细用(见 paintEvent/requestRepaint) */
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QTimer>       /* 数据路径的重绘合并定时器(见构造函数的说明) */
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

/*! 取一个 "好看" 的刻度步长, 让刻度个数不超过 maxLabels。 */
double niceStep(double range, int maxLabels)
{
    if (!(range > 0.0) || maxLabels < 1)
        return 1.0;

    const double raw  = range / double(maxLabels);
    const double mag  = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;

    double mult = 10.0;
    if (norm <= 1.0)
        mult = 1.0;
    else if (norm <= 2.0)
        mult = 2.0;
    else if (norm <= 5.0)
        mult = 5.0;

    return mult * mag;
}

/*! 刻度文本: 步长是整数时不留小数(参考图里就是 500 / -2500 这种)。 */
QString formatTick(double value, double step)
{
    int decimals = 0;
    if (step < 0.01)
        decimals = 3;
    else if (step < 1.0)
        decimals = 2;
    else if (step < 10.0 && std::fabs(step - std::floor(step)) > 1e-9)
        decimals = 1;

    if (std::fabs(value) < step * 1e-6)
        value = 0.0;                      // 消掉 -0

    return QString::number(value, 'f', decimals);
}

/*! 刻度起点: 第一个 >= lo 的步长整数倍。 */
double firstTick(double lo, double step)
{
    return std::ceil(lo / step - 1e-9) * step;
}

bool isFinite(double v)
{
    return !std::isnan(v) && !std::isinf(v);
}

} // namespace

PlotWidget::PlotWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    /*
     * 数据路径的重绘合并定时器(约 30 Hz)。
     * ⚠ singleShot=true 是关键: 只有"真的来了新数据"(requestRepaint 里打脏标记)才会
     *   再启动它 —— 没数据时一次多余重绘都不会有, 不会变成常驻 30 fps 空转。
     */
    m_repaintTimer = new QTimer(this);
    m_repaintTimer->setSingleShot(true);
    m_repaintTimer->setInterval(33);
    connect(m_repaintTimer, &QTimer::timeout, this, [this]() {
        m_dirty = false;
        update();
    });
}

void PlotWidget::requestRepaint()
{
    if (m_dirty)
        return;                     /* 已经排了一次重绘, 这一拍的数据跟着那次一起画 */

    m_dirty = true;

    if (!m_repaintTimer->isActive()) {
        /*
         * ⚠⚠ 间隔**按上一帧实际花了多少毫秒**来定, 不是拍脑袋定的常数(2026-10-16)。
         *
         *   插桩量出来的真相(真板子 100 Hz × 8 通道):
         *     解析 0.13%, 进曲线 0.05%, **重绘 95%** —— 卡顿全在画图上, 而且是"数据毛"
         *     决定的: ADC 通道悬空时每条曲线都是一条**满高宽带**, 一帧要光栅化
         *     8 条 × 1446 列 × 600 像素 ≈ 670 万像素(约 27 MB), 单帧 20~100 ms。
         *     这种帧率下就算限到 10 fps 也照样 100% CPU —— 所以必须"帧慢就继续降帧率",
         *     把**重绘占空比**压下来(帧耗时 × 帧率), 界面才不会被画图拖住。
         *
         *   分级(按上一帧耗时):
         *     < 20 ms  → 33 ms  (约 30 fps, 平滑数据的正常观感)
         *     < 60 ms  → 100 ms (数据密/毛, 但还能画得动)
         *     ≥ 60 ms  → 500 ms (一帧就要几十毫秒的重货: 那是一条带, 2 fps 完全够看,
         *                        同时保证重绘占空比 ≤ 约 20%)
         */
        int interval = 33;

        if (m_lastFrameDense)
            interval = 100;

        if (m_lastPaintMs >= 60.0)
            interval = 500;
        else if (m_lastPaintMs >= 20.0)
            interval = 200;

        m_repaintTimer->setInterval(interval);
        m_repaintTimer->start();
    }
}

/*!
 * 超出点数上限时**一次多删一点**(留 10% 余量), 而不是每来一个点就搬一次整个数组。
 *
 * ⚠ 踩过的坑: 原来写的是 `if (size > max) remove(0, size - max)` —— 到顶之后**每一个
 *   新点都要把 20 万个点整体前移一次**, 100 Hz 遥测下就是每秒两千万次元素搬移,
 *   长时间跑起来界面明显发顿。留 10% 余量后, 平均每 2 万个点才搬一次。
 * ⚠ 上限仍然是 m_maxPoints(峰值不会超), 只是删的时候会删到 90% 那根线。
 */
void PlotWidget::trimCurve(Curve &c)
{
    if (m_maxPoints <= 0 || c.points.size() <= m_maxPoints)
        return;

    const int keep = m_maxPoints - m_maxPoints / 10;
    c.points.remove(0, c.points.size() - keep);
}

/* ========================================================================== */
/*  曲线管理                                                                   */
/* ========================================================================== */

int PlotWidget::addCurve(const QString &name, const QColor &color, const QString &unit)
{
    Curve c;
    c.name  = name;
    c.color = color;
    c.unit  = unit;
    m_curves.append(c);
    return m_curves.size() - 1;
}

QString PlotWidget::curveUnit(int index) const
{
    if (index < 0 || index >= m_curves.size())
        return QString();

    return m_curves.at(index).unit;
}

void PlotWidget::clearAll()
{
    for (Curve &c : m_curves)
        c.points.clear();

    /* 数据全没了, 游标也就没有意义了(读数会变成 --), 一起清掉 */
    m_cursors.clear();

    m_xMode = XMode::Scroll;
    update();
    emit cursorsChanged();
}

void PlotWidget::clearCurve(int index)
{
    if (index < 0 || index >= m_curves.size())
        return;

    m_curves[index].points.clear();
    update();
}

void PlotWidget::appendPoint(int index, double x, double y)
{
    if (index < 0 || index >= m_curves.size())
        return;

    if (!isFinite(x) || !isFinite(y))
        return;

    Curve &c = m_curves[index];
    c.points.append(QPointF(x, y));

    trimCurve(c);

    requestRepaint();   // ⚠ 不是 update(): 见 requestRepaint 的说明(100 Hz 数据 → 30 Hz 重绘)
}

void PlotWidget::appendPoints(int index, const QVector<QPointF> &points)
{
    if (index < 0 || index >= m_curves.size())
        return;

    Curve &c = m_curves[index];
    c.points.reserve(c.points.size() + points.size());

    for (const QPointF &p : points) {
        if (!isFinite(p.x()) || !isFinite(p.y()))
            continue;
        c.points.append(p);
    }

    trimCurve(c);

    requestRepaint();
}

int PlotWidget::curveCount() const
{
    return m_curves.size();
}

const PlotWidget::Curve *PlotWidget::curve(int index) const
{
    if (index < 0 || index >= m_curves.size())
        return nullptr;

    return &m_curves.at(index);
}

QString PlotWidget::curveName(int index) const
{
    const Curve *c = curve(index);
    return c ? c->name : QString();
}

QVector<QPointF> PlotWidget::curvePoints(int index) const
{
    const Curve *c = curve(index);
    return c ? c->points : QVector<QPointF>();
}

void PlotWidget::setCurveVisible(int index, bool visible)
{
    if (index < 0 || index >= m_curves.size())
        return;

    if (m_curves[index].visible == visible)
        return;

    m_curves[index].visible = visible;

    /*
     * 显示集合变了, 纵轴自动缩放的缓存立刻作废: 取消勾选一条大信号(比如速度),
     * 剩下的电流曲线应该马上按自己的量程铺满, 而不是等下一次重绘。
     */
    m_autoYValid = false;
    update();
}

bool PlotWidget::isCurveVisible(int index) const
{
    const Curve *c = curve(index);
    return c ? c->visible : false;
}

/* ========================================================================== */
/*  显示设置                                                                   */
/* ========================================================================== */

void PlotWidget::setAxisTitles(const QString &xTitle)
{
    m_xTitle = xTitle;
    update();
}

void PlotWidget::setXMode(XMode mode)
{
    m_xMode = mode;
    update();
}

PlotWidget::XMode PlotWidget::xMode() const
{
    return m_xMode;
}

void PlotWidget::setXSpan(double span)
{
    if (span > 0.0)
        m_xSpan = span;
    update();
}

double PlotWidget::xSpan() const
{
    return m_xSpan;
}

void PlotWidget::setYRange(double lo, double hi)
{
    if (!isFinite(lo) || !isFinite(hi) || hi <= lo)
        return;

    m_yLo = lo;
    m_yHi = hi;
    update();
}

double PlotWidget::yMin() const
{
    return m_yLo;
}

double PlotWidget::yMax() const
{
    return m_yHi;
}

void PlotWidget::setAutoScaleY(bool on)
{
    if (m_autoScaleY == on)
        return;

    m_autoScaleY = on;
    m_autoYValid = false;
    update();
}

bool PlotWidget::autoScaleY() const
{
    return m_autoScaleY;
}

double PlotWidget::yLo() const
{
    if (!m_autoScaleY)
        return m_yLo;

    if (!m_painting) {
        // 绘制之外: 现算现用, 不留缓存(数据/勾选/窗口刚变过也能拿到新值)
        double lo = 0.0;
        double hi = 0.0;
        autoYRange(&lo, &hi);
        return lo;
    }

    if (!m_autoYValid)
        refreshAutoYRange();

    return m_autoYLo;
}

double PlotWidget::yHi() const
{
    if (!m_autoScaleY)
        return m_yHi;

    if (!m_painting) {
        double lo = 0.0;
        double hi = 0.0;
        autoYRange(&lo, &hi);
        return hi;
    }

    if (!m_autoYValid)
        refreshAutoYRange();

    return m_autoYHi;
}

void PlotWidget::setLegendVisible(bool visible)
{
    m_legendVisible = visible;
    update();
}

bool PlotWidget::legendVisible() const
{
    return m_legendVisible;
}

void PlotWidget::setMaxPointsPerCurve(int maxPoints)
{
    m_maxPoints = maxPoints;
}

void PlotWidget::resetView()
{
    m_xMode = XMode::Scroll;
    update();
}

/* ========================================================================== */
/*  游标(双击加点)                                                             */
/* ========================================================================== */

/*!
 * 在数据坐标 X 处加一个游标; 附近已经有游标就把它删掉(双击同一处 = 取消)。
 *
 * "附近"的判据用**像素**折算成数据单位: 同一个位置再双击一次应当命中, 但隔开几十个
 * 像素的两个游标不能互相吃掉。游标只存 X, 每次重绘都按当前数据重算读数, 所以数据
 * 一直在滚动也不会读到旧值。
 *
 * @return 新游标的序号(按 X 升序, 从 0 开始); 删掉了返回 -1。
 */
int PlotWidget::addCursor(double x)
{
    const QRectF  r      = plotRect();
    const double  x0     = xLo();
    const double  x1     = xHi();
    const double  tolPx  = 6.0;                       /* 6 px 以内算"同一处" */

    if (!(x1 > x0) || (r.width() < 1.0))
        return -1;

    const double tol = tolPx / r.width() * (x1 - x0);

    for (int i = 0; i < m_cursors.size(); ++i) {
        if (std::fabs(m_cursors.at(i) - x) <= tol) {
            m_cursors.remove(i);
            update();
            emit cursorsChanged();
            return -1;
        }
    }

    m_cursors.append(x);
    std::sort(m_cursors.begin(), m_cursors.end());
    update();
    emit cursorsChanged();

    return int(std::lower_bound(m_cursors.begin(), m_cursors.end(), x) - m_cursors.begin());
}

void PlotWidget::clearCursors()
{
    if (m_cursors.isEmpty())
        return;

    m_cursors.clear();
    update();
    emit cursorsChanged();
}

int PlotWidget::cursorCount() const
{
    return m_cursors.size();
}

double PlotWidget::cursorX(int index) const
{
    if (index < 0 || index >= m_cursors.size())
        return 0.0;

    return m_cursors.at(index);
}

QVector<double> PlotWidget::cursors() const
{
    return m_cursors;
}

/*! 某条曲线在 X 处的取值: 取横坐标最近的采样点(和十字准星同一套). */
bool PlotWidget::valueAtX(const Curve &CurveRef, double X, double *Out)
{
    if (CurveRef.points.isEmpty() || (Out == nullptr))
        return false;

    const auto it = std::lower_bound(CurveRef.points.begin(), CurveRef.points.end(),
                                     QPointF(X, 0.0),
                                     [](const QPointF &p, const QPointF &v) { return p.x() < v.x(); });
    int idx = int(it - CurveRef.points.begin());
    if (idx >= CurveRef.points.size())
        idx = CurveRef.points.size() - 1;
    if (idx > 0 && std::fabs(CurveRef.points.at(idx - 1).x() - X)
                       < std::fabs(CurveRef.points.at(idx).x() - X)) {
        idx--;
    }

    *Out = CurveRef.points.at(idx).y();
    return true;
}

/* ========================================================================== */
/*  坐标换算                                                                   */
/* ========================================================================== */

bool PlotWidget::dataBounds(double *firstX, double *lastX) const
{
    bool   found = false;
    double lo = 0.0;
    double hi = 0.0;

    for (const Curve &c : m_curves) {
        if (c.points.isEmpty())
            continue;

        const double a = c.points.first().x();
        const double b = c.points.last().x();

        if (!found) {
            lo = a;
            hi = b;
            found = true;
        } else {
            lo = std::min(lo, a);
            hi = std::max(hi, b);
        }
    }

    if (found) {
        if (firstX)
            *firstX = lo;
        if (lastX)
            *lastX = hi;
    }

    return found;
}

/*!
 * 显示中的曲线在当前横轴窗口内的 y 范围(横轴窗口两边各多取一个点, 和绘制时一致)。
 * 没有任何点落在窗口里 → 返回 false, 让调用方回落到手动区间。
 */
bool PlotWidget::visibleYBounds(double *lo, double *hi) const
{
    const double x0 = xLo();
    const double x1 = xHi();

    bool   found = false;
    double y0 = 0.0;
    double y1 = 0.0;

    for (const Curve &c : m_curves) {
        if (!c.visible || c.points.size() < 1)
            continue;

        auto from = std::lower_bound(c.points.begin(), c.points.end(), QPointF(x0, 0.0),
                                     [](const QPointF &p, const QPointF &v) { return p.x() < v.x(); });
        if (from != c.points.begin())
            --from;

        auto to = std::upper_bound(c.points.begin(), c.points.end(), QPointF(x1, 0.0),
                                   [](const QPointF &v, const QPointF &p) { return v.x() < p.x(); });
        if (to != c.points.end())
            ++to;

        for (auto it = from; it != to; ++it) {
            const double v = it->y();
            if (!isFinite(v))
                continue;

            if (!found) {
                y0 = v;
                y1 = v;
                found = true;
            } else {
                y0 = std::min(y0, v);
                y1 = std::max(y1, v);
            }
        }
    }

    if (found) {
        if (lo)
            *lo = y0;
        if (hi)
            *hi = y1;
    }

    return found;
}

/*!
 * 现算纵轴自动缩放区间(不碰缓存)。
 * - 有数据: 取显示中的曲线在窗口内的最小/最大值, 上下各留 8% 余量;
 * - min == max(只有一条直线): 上下各撑开 1, 免得区间退化成一条线除以 0;
 * - 没数据: 直接用手动区间。
 */
void PlotWidget::autoYRange(double *lo, double *hi) const
{
    double a = m_yLo;
    double b = m_yHi;

    double d0 = 0.0;
    double d1 = 0.0;

    if (visibleYBounds(&d0, &d1)) {
        if (!(d1 - d0 > 1e-9)) {
            d0 -= 1.0;
            d1 += 1.0;
        }

        const double margin = (d1 - d0) * 0.08;
        a = d0 - margin;
        b = d1 + margin;
    }

    if (lo)
        *lo = a;
    if (hi)
        *hi = b;
}

void PlotWidget::refreshAutoYRange() const
{
    autoYRange(&m_autoYLo, &m_autoYHi);
    m_autoYValid = true;
}

double PlotWidget::xHi() const
{
    if (m_xMode == XMode::Fixed)
        return m_xFixHi;

    double last = 0.0;
    dataBounds(nullptr, &last);

    return std::max(last, m_xSpan);
}

double PlotWidget::xLo() const
{
    if (m_xMode == XMode::Fixed)
        return m_xFixLo;

    return xHi() - m_xSpan;
}

QRectF PlotWidget::plotRect() const
{
    /* 四个边距取对称值: 坐标框居中。左边留够放纵轴刻度数字, 下边留够放横轴刻度和标题。 */
    const qreal padX = 60.0;
    const qreal padY = 44.0;

    return QRectF(padX,
                  padY,
                  std::max<qreal>(1.0, width() - padX * 2.0),
                  std::max<qreal>(1.0, height() - padY * 2.0));
}

/* ========================================================================== */
/*  绘制                                                                       */
/* ========================================================================== */

void PlotWidget::paintEvent(QPaintEvent *)
{
    QElapsedTimer prof;      /* 只为量"这一帧花了多少毫秒"——下一帧的间隔/笔画粗细按它自适应 */
    prof.start();

    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);

    /*
     * 纵轴自动缩放的区间在一次重绘里只算一次: 网格/曲线/准星必须用同一个区间, 否则
     * 网格和曲线会错位。m_painting 让 yLo()/yHi() 只在绘制期间读缓存, 绘制之外一律现算。
     */
    m_autoYValid = false;
    m_painting   = true;

    const QRectF r = plotRect();
    if (r.width() < 24.0 || r.height() < 24.0) {
        m_painting = false;
        return;
    }

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    drawBackgroundAndGrid(painter, r);
    drawCurves(painter, r);

    if (m_legendVisible)
        drawLegend(painter, r);

    if (m_hovering)
        drawCrosshair(painter, r);

    /* 游标画在最后: 它是"读数"层, 压住曲线和准星都不影响判读 */
    drawCursors(painter, r);

    m_painting   = false;
    m_autoYValid = false;

    /* 上一帧的实际耗时 —— 决定下一次重绘的间隔与笔画粗细(见 requestRepaint) */
    m_lastPaintMs = double(prof.nsecsElapsed()) / 1e6;
}

void PlotWidget::drawBackgroundAndGrid(QPainter &painter, const QRectF &r)
{
    const double x0 = xLo();
    const double x1 = xHi();
    const double y0 = yLo();
    const double y1 = yHi();

    if (!(x1 > x0) || !(y1 > y0))
        return;

    QFont tickFont = painter.font();
    tickFont.setPointSize(8);
    tickFont.setBold(false);
    painter.setFont(tickFont);

    const QFontMetricsF fm(tickFont);

    /* ---- 网格: 与参考图一致的点状浅灰线 ---------------------------------- */
    const int    maxXLabels = std::max(2, int(r.width() / 28.0));
    const int    maxYLabels = std::max(2, int(r.height() / 44.0));
    const double xStep = niceStep(x1 - x0, maxXLabels);
    const double yStep = niceStep(y1 - y0, maxYLabels);

    QPen gridPen(QColor(0xB8, 0xB8, 0xB8));
    gridPen.setStyle(Qt::DotLine);
    gridPen.setWidth(1);

    for (double t = firstTick(y0, yStep); t <= y1 + yStep * 1e-6; t += yStep) {
        const double py = r.bottom() - (t - y0) / (y1 - y0) * r.height();
        painter.setPen(gridPen);
        painter.drawLine(QPointF(r.left(), py), QPointF(r.right(), py));
    }

    for (double t = firstTick(x0, xStep); t <= x1 + xStep * 1e-6; t += xStep) {
        const double px = r.left() + (t - x0) / (x1 - x0) * r.width();
        painter.setPen(gridPen);
        painter.drawLine(QPointF(px, r.top()), QPointF(px, r.bottom()));
    }

    /* ---- 坐标框 ---------------------------------------------------------- */
    painter.setPen(QPen(QColor(0x20, 0x20, 0x20), 1));
    painter.drawRect(r);

    /* ---- 刻度文字 -------------------------------------------------------- */
    painter.setPen(QColor(0x20, 0x20, 0x20));

    for (double t = firstTick(y0, yStep); t <= y1 + yStep * 1e-6; t += yStep) {
        const double py = r.bottom() - (t - y0) / (y1 - y0) * r.height();
        const QString text = formatTick(t, yStep);
        painter.drawText(QRectF(0.0, py - fm.height() / 2.0, r.left() - 6.0, fm.height()),
                         Qt::AlignRight | Qt::AlignVCenter, text);
    }

    for (double t = firstTick(x0, xStep); t <= x1 + xStep * 1e-6; t += xStep) {
        const double px = r.left() + (t - x0) / (x1 - x0) * r.width();
        const QString text = formatTick(t, xStep);
        const double  w    = fm.horizontalAdvance(text) + 8.0;
        painter.drawText(QRectF(px - w / 2.0, r.bottom() + 3.0, w, fm.height()),
                         Qt::AlignHCenter | Qt::AlignTop, text);
    }

    /* ---- 轴标题 ---------------------------------------------------------- */
    QFont titleFont = painter.font();
    titleFont.setPointSize(9);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(0x10, 0x10, 0x10));

    // 横轴标题: 居中放在刻度文字下面
    painter.drawText(QRectF(r.left(), height() - 22.0, r.width(), 20.0),
                     Qt::AlignHCenter | Qt::AlignTop, m_xTitle);

    // 纵轴不留标题(左边缘只放刻度数字), 数值和单位由鼠标处那根准星给出

}

void PlotWidget::drawCurves(QPainter &painter, const QRectF &r)
{
    const double x0 = xLo();
    const double x1 = xHi();
    const double y0 = yLo();
    const double y1 = yHi();

    /* 这一帧到底"密不密"由下面的绘制过程决定(任一曲线进入降采样就算密) */
    m_lastFrameDense = false;

    if (!(x1 > x0) || !(y1 > y0))
        return;

    painter.save();
    painter.setClipRect(r);

    for (const Curve &c : m_curves) {
        if (!c.visible)              // 勾选框没勾的曲线不画(数据还留着)
            continue;

        if (c.points.size() < 2)
            continue;

        // 只画落在窗口里的点(外扩一个采样点, 保证曲线从边界外进来)
        int from = int(std::lower_bound(c.points.begin(), c.points.end(), QPointF(x0, 0.0),
                                        [](const QPointF &p, const QPointF &v) { return p.x() < v.x(); })
                       - c.points.begin());
        from = std::max(0, from - 1);

        int to = int(std::upper_bound(c.points.begin(), c.points.end(), QPointF(x1, 0.0),
                                      [](const QPointF &v, const QPointF &p) { return v.x() < p.x(); })
                     - c.points.begin());
        to = int(std::min<qsizetype>(c.points.size(), qsizetype(to) + 1));

        if (to - from < 2)
            continue;

        const int    nVisible = to - from;
        const double sx       = double(r.width())  / (x1 - x0);   /* 数据单位 -> 像素 */
        const double sy       = double(r.height()) / (y1 - y0);

        QPolygonF poly;

        /* 这一帧里有没有"跨得开"的列(= 真毛数据/快沿)。只有它才配得上关抗锯齿 + 降帧率。 */
        bool wideSpan = false;

        /*
         * ⚠⚠ **点比像素还密时按"每像素列取最高/最低两点"降采样**(2026-10-16, 修卡顿)。
         *
         *   原来是把窗口里的点**一个不落地**做坐标变换 + 描成一条抗锯齿折线: 8 条曲线
         *   各上万点, 一帧就是几万个顶点, 一次重绘几十毫秒 —— 遥测 100 Hz 追着重绘,
         *   重绘请求越堆越多, 界面就卡住了(现场反馈"图像绘制过多导致卡顿")。
         *
         *   屏幕一个像素列本来就画不出更多细节: 每一列只保留**这一列里最高和最低的两个
         *   点**, 顶点数从"几万"降到"2 × 宽度"(约 2800), 形状(包络)完全一致,
         *   单帧耗时降一个数量级。点不密时(nVisible <= 2 × 宽度)仍然逐点画, 保持原样。
         */
        if (nVisible <= int(r.width()) * 2) {
            poly.reserve(nVisible);

            for (int i = from; i < to; ++i) {
                const QPointF &p = c.points.at(i);
                poly.append(QPointF(r.left() + (p.x() - x0) * sx,
                                    r.bottom() - (p.y() - y0) * sy));
            }
        } else {
            poly.reserve(int(r.width()) + 8);

            /*
             * ⚠ 密到这个程度时**几列并一列画**: 画面上就是一条带, 一步 2 像素与一步 1 像素
             *   肉眼无差, 但光栅化的墨水直接少一半。
             *
             * ⚠⚠ 再按**上一帧的实际耗时**继续加粗(自适应分辨率, 见 requestRepaint 里的说明):
             *   悬空那种"满高宽带"的代价 ∝ 笔画数 × 高度 —— 8 条曲线各 700 笔画 × 600 像素
             *   就是 340 万像素/帧, 单帧上百毫秒, 再多定时器也救不回来。把笔画数压到
             *   ≤ 约 360/曲线(必要时进一步减半), 一帧就回到十几毫秒量级, 而画面仍是那条带。
             */
            int colStep = std::max(2, int(r.width()) / 360);

            if (m_lastPaintMs > 60.0)
                colStep *= 2;               /* 还慢? 再粗一档(最多 8 像素一笔) */

            colStep = std::min(colStep, 8);

            int    curCol = -1;
            double colX   = 0.0;
            double colFirstY = 0.0;   /* 这一列**第一个**点的 y(平滑时用它当代表) */
            double minY   = 0.0;
            double maxY   = 0.0;

            /*
             * ⚠ 每一列收尾时**看这一列到底"跨得开不开"**再决定怎么画(2026-10-16 修正):
             *
             *   只是"点比像素密"并不代表这一列里有内容 —— 平滑信号(例如低频三角波)的斜边
             *   同样会让一个像素列落进好几个采样点, 但这几个点的 y 只差几个像素。如果一律
             *   画"最高+最低"两点, 斜边就变成一列一列的短竖线, 看上去像梳齿/变乱(现场反馈:
             *   "三角波画出来是凌乱的")。
             *
             *   所以: 列内跨度 ≤ kFlatSpanPx 时**只画一个点**(第一个点), 斜边就是一条平滑
             *   折线; 只有跨度真的大(毛数据/快沿)才画"最高+最低"两点, 保留包络。
             *   这样平滑信号的外观与降采样前一致, 而毛数据仍然只花 2 个点/列的代价。
             */
            constexpr double kFlatSpanPx = 3.0;   /* 列内跨度 ≤ 这么多像素 = "这一列是平的" */

            const auto flushCol = [&poly, &wideSpan](int col, double x, double firstY,
                                                     double lo, double hi) {
                if (col < 0) {
                    return;
                }

                if ((hi - lo) <= kFlatSpanPx) {
                    poly.append(QPointF(x, firstY));        /* 平滑: 一个点就够 */
                } else {
                    poly.append(QPointF(x, lo));            /* 毛: 保留包络 */
                    poly.append(QPointF(x, hi));
                    wideSpan = true;
                }
            };

            for (int i = from; i < to; ++i) {
                const QPointF &p  = c.points.at(i);
                const int      col = int((p.x() - x0) * sx) / colStep;   /* colStep 像素一笔 */
                const double   py  = r.bottom() - (p.y() - y0) * sy;

                if (col != curCol) {
                    flushCol(curCol, colX, colFirstY, minY, maxY);

                    curCol     = col;
                    colX       = r.left() + double(col * colStep);
                    colFirstY  = py;
                    minY       = py;
                    maxY       = py;
                } else {
                    minY = std::min(minY, py);
                    maxY = std::max(maxY, py);
                }
            }

            flushCol(curCol, colX, colFirstY, minY, maxY);
        }

        if (poly.size() < 2)
            continue;

        /*
         * ⚠ 什么时候才"关抗锯齿 + 把帧率放到 10 fps": **看这一帧里有没有真的跨得开的列**
         *   (现场场景: ADC 通道线拔掉悬空, 采样值在满量程之间乱跳), 而不是"点比像素密"。
         *   平滑信号(低频三角波)点同样很密, 但它的列跨度只有几像素 —— 那种帧照旧开抗锯齿、
         *   照旧 30 fps, 外观与降采样前一致, 只是顶点数少了。
         *   而真毛的那种帧, 每列要画一条跨满高的竖线, 代价按**像素数**算: 几百万像素 × 曲线
         *   条数/帧 —— 帧率降下来肉眼几乎无感(看到的还是那条带), CPU 却按比例降。
         */
        const bool dense = wideSpan;

        if (dense) {
            m_lastFrameDense = true;    /* 让下一次重绘把间隔放宽到 100 ms(见 requestRepaint) */
            painter.setRenderHint(QPainter::Antialiasing, false);
        }

        QPen pen(c.color);
        pen.setWidthF(1.6);

        /*
         * ⚠⚠ 顶点连接方式**必须跟着数据走**(2026-10-16, 卡顿的真凶就是这里):
         *
         *   圆角连接(RoundJoin/RoundCap)在**每个顶点**都要构造一段圆弧/圆帽。平滑曲线
         *   顶点疏、转角小, 看不出来; 而"悬空"那种噪声带是"一列一根竖线、顶点处近乎
         *   180° 折返", 顶点又多又尖 —— Qt 为每一个顶点生成圆角几何, 单帧直接飙到几百
         *   毫秒(实测重绘占 97% CPU 就是这么来的, 跟像素数关系不大)。
         *
         *   噪声帧改成平头 + 斜接(不生成任何圆弧), 视觉上就是一条带, 完全看不出区别,
         *   但单帧耗时降一个数量级以上。平滑帧照旧用圆角(观感更细腻)。
         */
        pen.setJoinStyle(dense ? Qt::BevelJoin : Qt::RoundJoin);
        pen.setCapStyle(dense ? Qt::FlatCap   : Qt::RoundCap);

        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(poly);

        if (dense)
            painter.setRenderHint(QPainter::Antialiasing, true);

        // 点不多的时候补上采样点, 避免看起来像"没数据"
        if (poly.size() <= 200) {
            painter.setBrush(c.color);
            painter.setPen(Qt::NoPen);
            for (const QPointF &p : poly)
                painter.drawEllipse(p, 2.0, 2.0);
        }
    }

    painter.restore();
}

void PlotWidget::drawLegend(QPainter &painter, const QRectF &r)
{
    /* 只统计"显示中且已经有数据"的曲线: 隐藏的曲线不该在图例里占位置。 */
    int shown = 0;
    for (const Curve &c : m_curves) {
        if (c.visible && !c.points.isEmpty())
            shown++;
    }

    if (shown < 2)
        return;

    QFont f = painter.font();
    f.setPointSize(8);
    f.setBold(false);
    painter.setFont(f);

    const QFontMetricsF fm(f);
    double w = 0.0;
    for (const Curve &c : m_curves) {
        if (c.visible && !c.points.isEmpty())
            w = std::max(w, fm.horizontalAdvance(c.name));
    }
    w += 34.0;

    const double lineH = fm.height() + 4.0;
    const QRectF box(r.right() - w - 8.0, r.top() + 8.0, w, lineH * shown + 8.0);

    painter.setPen(QPen(QColor(0x99, 0x99, 0x99), 1));
    painter.setBrush(QColor(255, 255, 255, 220));
    painter.drawRect(box);

    double y = box.top() + 4.0;
    for (const Curve &c : m_curves) {
        if (!c.visible || c.points.isEmpty())
            continue;

        painter.setPen(QPen(c.color, 2.0));
        painter.drawLine(QPointF(box.left() + 6.0, y + lineH / 2.0),
                         QPointF(box.left() + 24.0, y + lineH / 2.0));

        painter.setPen(QColor(0x20, 0x20, 0x20));
        painter.drawText(QRectF(box.left() + 28.0, y, box.width() - 32.0, lineH),
                         Qt::AlignLeft | Qt::AlignVCenter, c.name);
        y += lineH;
    }
}

void PlotWidget::drawCrosshair(QPainter &painter, const QRectF &r)
{
    const double x0 = xLo();
    const double x1 = xHi();
    const double y0 = yLo();
    const double y1 = yHi();

    if (!(x1 > x0) || !(y1 > y0))
        return;

    const double px = qBound(r.left(), qreal(m_lastPos.x()), r.right());
    const double py = qBound(r.top(), qreal(m_lastPos.y()), r.bottom());

    QPen pen(QColor(0x80, 0x80, 0x80));
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    painter.drawLine(QPointF(px, r.top()), QPointF(px, r.bottom()));
    painter.drawLine(QPointF(r.left(), py), QPointF(r.right(), py));

    const double cx = x0 + (px - r.left()) / r.width() * (x1 - x0);
    const double cy = y0 + (r.bottom() - py) / r.height() * (y1 - y0);

    QStringList lines;
    lines << QStringLiteral("t = %1 s").arg(cx, 0, 'f', 3);

    for (const Curve &c : m_curves) {
        if (!c.visible || c.points.isEmpty())   // 隐藏的曲线不在读数框里露出来
            continue;

        const auto it = std::lower_bound(c.points.begin(), c.points.end(), QPointF(cx, 0.0),
                                         [](const QPointF &p, const QPointF &v) { return p.x() < v.x(); });
        int idx = int(it - c.points.begin());
        if (idx >= c.points.size())
            idx = c.points.size() - 1;
        if (idx > 0 && std::fabs(c.points.at(idx - 1).x() - cx) < std::fabs(c.points.at(idx).x() - cx))
            idx--;

        /* 读数带单位、保留 3 位小数(线圈 A, 速度/母线 V) */
        lines << (c.unit.isEmpty()
                      ? QStringLiteral("%1 = %2").arg(c.name).arg(c.points.at(idx).y(), 0, 'f', 3)
                      : QStringLiteral("%1 = %2 %3").arg(c.name).arg(c.points.at(idx).y(), 0, 'f', 3)
                            .arg(c.unit));
    }

    QFont f = painter.font();
    f.setPointSize(8);
    f.setBold(false);
    painter.setFont(f);

    const QFontMetricsF fm(f);
    double w = 0.0;
    for (const QString &s : lines)
        w = std::max(w, fm.horizontalAdvance(s));
    w += 12.0;

    const QRectF box(r.left() + 8.0, r.top() + 8.0, w, lines.size() * (fm.height() + 2.0) + 8.0);

    painter.setPen(QPen(QColor(0x99, 0x99, 0x99), 1));
    painter.setBrush(QColor(255, 255, 255, 235));
    painter.drawRect(box);

    painter.setPen(QColor(0x20, 0x20, 0x20));
    double y = box.top() + 4.0;
    for (const QString &s : lines) {
        painter.drawText(QRectF(box.left() + 6.0, y, box.width() - 12.0, fm.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, s);
        y += fm.height() + 2.0;
    }

    // 纵轴读数(光标所在高度), 同样 3 位小数
    painter.setPen(QColor(0x20, 0x20, 0x20));
    painter.drawText(QRectF(0.0, py - fm.height() / 2.0, r.left() - 6.0, fm.height()),
                     Qt::AlignRight | Qt::AlignVCenter, QString::number(cy, 'f', 3));
}

/*!
 * 画所有游标: 每个游标一条竖向虚线 + 右边一个读数框(横轴值 + 每条显示中曲线的值)。
 *
 * 第二个及以后的游标, 读数框里额外给一行 "Δ 相对第一个游标" —— 有了两个游标就能直接
 * 读出"这段时间里位移变了多少、速度差多少", 不用再手算(这正是试验台最常问的数)。
 * 框子按游标序号自上而下排开, 免得几个游标的框叠在一起。
 */
void PlotWidget::drawCursors(QPainter &painter, const QRectF &r)
{
    if (m_cursors.isEmpty())
        return;

    const double x0 = xLo();
    const double x1 = xHi();

    if (!(x1 > x0) || (r.width() < 1.0))
        return;

    QFont f = painter.font();
    f.setPointSize(8);
    f.setBold(false);
    painter.setFont(f);
    const QFontMetricsF fm(f);

    painter.save();
    painter.setClipRect(r);

    double boxTop = r.top() + 8.0;

    for (int i = 0; i < m_cursors.size(); ++i) {
        const double cx = m_cursors.at(i);
        const double px = r.left() + (cx - x0) / (x1 - x0) * r.width();

        if ((px < r.left() - 1.0) || (px > r.right() + 1.0))
            continue;                       /* 滚出窗口的游标不画(但也不删) */

        /* 竖线: 和准星的灰色区分开, 用橙色 */
        QPen pen(QColor(0xC0, 0x5A, 0x00), 1, Qt::DashDotLine);
        painter.setPen(pen);
        painter.drawLine(QPointF(px, r.top()), QPointF(px, r.bottom()));

        /* 顶部小三角, 让人看得出这是"钉住的"游标而不是鼠标准星 */
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0xC0, 0x5A, 0x00));
        painter.drawPolygon(QPolygonF() << QPointF(px - 4.0, r.top())
                                        << QPointF(px + 4.0, r.top())
                                        << QPointF(px, r.top() + 7.0));
        painter.setBrush(Qt::NoBrush);

        /* ---- 读数框 ---- */
        QStringList lines;
        lines << QStringLiteral("游标%1  %2 = %3")
                     .arg(i + 1)
                     .arg(m_xTitle.isEmpty() ? QStringLiteral("x") : m_xTitle)
                     .arg(cx, 0, 'f', 3);

        for (const Curve &c : m_curves) {
            double v = 0.0;

            if (!c.visible || !valueAtX(c, cx, &v))
                continue;

            lines << (c.unit.isEmpty()
                          ? QStringLiteral("%1 = %2").arg(c.name).arg(v, 0, 'f', 3)
                          : QStringLiteral("%1 = %2 %3").arg(c.name).arg(v, 0, 'f', 3).arg(c.unit));
        }

        /* 第二个起: 和第一个游标作差 */
        if (i > 0) {
            lines << QStringLiteral("Δ%1 = %2")
                         .arg(m_xTitle.isEmpty() ? QStringLiteral("x") : m_xTitle)
                         .arg(cx - m_cursors.first(), 0, 'f', 3);

            for (const Curve &c : m_curves) {
                double a = 0.0;
                double b = 0.0;

                if (!c.visible || !valueAtX(c, m_cursors.first(), &a) || !valueAtX(c, cx, &b))
                    continue;

                lines << QStringLiteral("Δ%1 = %2").arg(c.name).arg(b - a, 0, 'f', 3);
            }
        }

        double w = 0.0;
        for (const QString &s : lines)
            w = std::max(w, fm.horizontalAdvance(s));
        w += 14.0;

        const double h = lines.size() * (fm.height() + 2.0) + 8.0;

        /* 框子优先放线的右边; 右边放不下(贴近边框)就翻到左边 */
        double bx = px + 6.0;
        if (bx + w > r.right() - 2.0)
            bx = px - 6.0 - w;
        bx = qBound(r.left() + 2.0, bx, std::max(r.left() + 2.0, r.right() - 2.0 - w));

        const QRectF box(bx, boxTop, w, h);
        painter.setPen(QPen(QColor(0xC0, 0x5A, 0x00), 1));
        painter.setBrush(QColor(255, 252, 245, 240));
        painter.drawRect(box);
        painter.setBrush(Qt::NoBrush);

        double ty = box.top() + 4.0;
        for (int k = 0; k < lines.size(); ++k) {
            painter.setPen((k == 0) ? QColor(0xC0, 0x5A, 0x00) : QColor(0x20, 0x20, 0x20));
            painter.drawText(QRectF(box.left() + 6.0, ty, box.width() - 12.0, fm.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, lines.at(k));
            ty += fm.height() + 2.0;
        }

        boxTop = box.bottom() + 4.0;
        if (boxTop > r.bottom() - 20.0)
            boxTop = r.top() + 8.0;         /* 到底了就回到顶上, 别跑到框外 */
    }

    painter.restore();
}

/* ========================================================================== */
/*  交互                                                                       */
/* ========================================================================== */
void PlotWidget::wheelEvent(QWheelEvent *event)
{
    const QRectF r = plotRect();
    if (r.width() < 24.0)
        return;

    const int steps = event->angleDelta().y();
    if (steps == 0)
        return;

    const double factor = std::pow(1.15, -steps / 120.0);
    const QPointF pos    = event->position();

    if (event->modifiers() & Qt::ShiftModifier) {
        /*
         * 纵轴缩放: 以光标处的 y 值为锚点。
         * 自动缩放开着时先由本次滚轮接管: 拿"当前显示中的区间"当起点转成手动区间,
         * 否则改 m_yLo/m_yHi 也会被每帧重算的自动区间盖掉 —— 手感上就是"滚不动"。
         */
        if (m_autoScaleY) {
            m_yLo        = yLo();
            m_yHi        = yHi();
            m_autoScaleY = false;
            m_autoYValid = false;
            emit autoScaleYTakenOver();
        }

        const double y0 = m_yLo;
        const double y1 = m_yHi;
        const double anchor = y0 + (r.bottom() - qBound(r.top(), pos.y(), r.bottom())) / r.height() * (y1 - y0);

        double lo = anchor + (y0 - anchor) * factor;
        double hi = anchor + (y1 - anchor) * factor;
        if (hi - lo > 1e-9) {
            m_yLo = lo;
            m_yHi = hi;
        }
    } else {
        // 横轴缩放: 以光标处的 x 值为锚点
        const double x0 = xLo();
        const double x1 = xHi();
        const double anchor = x0 + (qBound(r.left(), pos.x(), r.right()) - r.left()) / r.width() * (x1 - x0);

        double lo = anchor + (x0 - anchor) * factor;
        double hi = anchor + (x1 - anchor) * factor;
        if (hi - lo > 1e-9) {
            m_xFixLo = lo;
            m_xFixHi = hi;
            m_xMode  = XMode::Fixed;
        }
    }

    event->accept();
    update();
}

void PlotWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_pressed  = true;
    m_panning  = false;
    m_pressPos = event->pos();
    m_lastPos  = event->pos();
    m_pressXLo = xLo();
    m_pressXHi = xHi();
    event->accept();
}

void PlotWidget::mouseMoveEvent(QMouseEvent *event)
{
    m_lastPos  = event->pos();
    m_hovering = true;

    if (m_pressed) {
        const QRectF r = plotRect();
        const int    dx = event->pos().x() - m_pressPos.x();

        if (std::abs(dx) > 2 && r.width() > 0.0) {
            m_panning = true;
            const double unitsPerPx = (m_pressXHi - m_pressXLo) / r.width();
            const double shift      = -dx * unitsPerPx;

            m_xFixLo = m_pressXLo + shift;
            m_xFixHi = m_pressXHi + shift;
            m_xMode  = XMode::Fixed;
        }
    }

    update();
    QWidget::mouseMoveEvent(event);
}

void PlotWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressed = false;
        m_panning = false;
        event->accept();
    }
    QWidget::mouseReleaseEvent(event);
}

void PlotWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    /*
     * 鼠标三个热键分工(2026-11 改):
     *   双击            = 在这里加一个游标(旁边显示该处的读数); 双击已有游标 = 删掉它
     *   Ctrl+双击       = 把这张图铺满绘图区(页面接 maximizeRequested 去隐藏另一张)
     *   右键菜单        = 清除游标 / 回到默认视图(原来的"双击复位"挪到这儿)
     */
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    if ((event->modifiers() & Qt::ControlModifier) != 0) {
        emit maximizeRequested();
        event->accept();
        return;
    }

    const QRectF r  = plotRect();
    const double x0 = xLo();
    const double x1 = xHi();

    if ((r.width() >= 24.0) && (x1 > x0)) {
        const double px = qBound(r.left(), qreal(event->position().x()), r.right());
        const double cx = x0 + (px - r.left()) / r.width() * (x1 - x0);

        (void)addCursor(cx);                /* 里面已经 update() + emit cursorsChanged() */
        event->accept();
        return;
    }

    QWidget::mouseDoubleClickEvent(event);
}

/*!
 * 右键菜单: 双击改成"加游标"之后, "回到默认视图"和"清游标"就放这儿。
 * 没游标时那一条显示成灰的, 免得点了没反应。
 */
void PlotWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);

    QAction *clearAct = menu.addAction(cursorCount() > 0
                                           ? QStringLiteral("清除游标 (%1 个)").arg(cursorCount())
                                           : QStringLiteral("清除游标"));
    clearAct->setEnabled(cursorCount() > 0);

    menu.addSeparator();

    QAction *resetAct = menu.addAction(QStringLiteral("回到默认视图(滚动窗口 + 纵轴)"));
    QAction *legendAct = menu.addAction(legendVisible() ? QStringLiteral("隐藏图例")
                                                        : QStringLiteral("显示图例"));

    QAction *chosen = menu.exec(event->globalPos());
    if (chosen == nullptr) {
        event->accept();
        return;
    }

    if (chosen == clearAct) {
        clearCursors();
    } else if (chosen == resetAct) {
        resetView();
    } else if (chosen == legendAct) {
        setLegendVisible(!legendVisible());
    }

    event->accept();
}

void PlotWidget::leaveEvent(QEvent *event)
{
    m_hovering = false;
    m_pressed  = false;
    m_panning  = false;
    update();
    QWidget::leaveEvent(event);
}

QSize PlotWidget::minimumSizeHint() const
{
    return QSize(320, 220);
}

QSize PlotWidget::sizeHint() const
{
    return QSize(640, 380);
}
