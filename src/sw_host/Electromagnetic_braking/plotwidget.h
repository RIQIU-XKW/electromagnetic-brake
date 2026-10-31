#ifndef PLOTWIDGET_H
#define PLOTWIDGET_H

#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>

class QTimer;
class QContextMenuEvent;

/*!
 * \brief 轻量实时曲线控件(纯 QPainter 绘制, 不依赖 QtCharts)。
 *
 * 外观对齐参考图: 浅灰点状网格 + 黑色坐标框 + 底部横轴标题,
 * 横轴默认取 "最近 100 个采样点" 的滚动窗口。纵轴不留标题(刻度数字贴左边框)。
 *
 * 多曲线: 每条曲线自带名字/颜色/单位/数据/是否显示; 只有"显示中且存在数据"的曲线才会被
 *         绘制、写入图例和准星读数(勾选框在"实时采集数据"页)。
 * 交互: 滚轮 = 缩放横轴 / Shift+滚轮 = 缩放纵轴(自动缩放开着时由它接管, 见信号
 *       autoScaleYTakenOver); 左键拖动 = 平移(进入 Fixed 模式);
 *       双击 = 回到默认滚动窗口; 鼠标悬停 = 十字准星读数。
 */
class PlotWidget : public QWidget
{
    Q_OBJECT

public:
    /*! 横轴取窗口的方式。 */
    enum class XMode {
        Scroll,   ///< 窗口跟随最新数据(默认), 宽度 = XSpan
        Fixed     ///< 固定区间(鼠标拖动/缩放之后进入)
    };

    /*! 一条曲线。 */
    struct Curve {
        QString          name;    ///< 图例名
        QString          unit;    ///< 单位(准星读数框里跟在数值后面, 例如 "A" / "V")
        QColor           color;   ///< 画笔颜色
        QVector<QPointF> points;  ///< 按 x 递增保存的采样点
        bool             visible = true;   ///< 是否绘制/进图例(由页面上的勾选框控制)
    };

    explicit PlotWidget(QWidget *parent = nullptr);

    /* ---- 曲线管理 -------------------------------------------------------- */
    int  addCurve(const QString &name, const QColor &color, const QString &unit = QString());
    void clearAll();                                        ///< 清空所有曲线数据
    void clearCurve(int index);
    void appendPoint(int index, double x, double y);
    void appendPoints(int index, const QVector<QPointF> &points);

    int              curveCount() const;
    const Curve     *curve(int index) const;
    QString          curveName(int index) const;
    QString          curveUnit(int index) const;
    QVector<QPointF> curvePoints(int index) const;

    /*! 单条曲线的显示/隐藏。隐藏的曲线不画、不进图例、不参与纵轴自动缩放。 */
    void setCurveVisible(int index, bool visible);
    bool isCurveVisible(int index) const;

    /* ---- 显示设置 -------------------------------------------------------- */
    void   setAxisTitles(const QString &xTitle);
    void   setXMode(XMode mode);
    XMode  xMode() const;
    void   setXSpan(double span);                 ///< 滚动窗口宽度(数据单位)
    double xSpan() const;
    void   setYRange(double lo, double hi);       ///< 手动纵轴区间(自动缩放打开时被忽略)
    double yMin() const;                          ///< 手动纵轴下限
    double yMax() const;                          ///< 手动纵轴上限

    /*!
     * 纵轴自动缩放: 打开后纵轴区间在每次重绘时按"当前显示中的曲线"落在横轴窗口内的
     * 点重新算(上下各留 ~8% 余量)。通道单位不一样(线圈是 A, 速度是 mm/s)时很有用。
     * 没有任何数据时回落到手动区间。
     */
    void   setAutoScaleY(bool on);
    bool   autoScaleY() const;

    /*! 本次重绘实际使用的纵轴区间(手动 or 自动), 绘制/准星都用它, 保证全控件一致。 */
    double yLo() const;
    double yHi() const;

    void   setLegendVisible(bool visible);
    bool   legendVisible() const;
    void   resetView();                           ///< 回到默认的滚动窗口 + 纵轴区间

    /* ---- 游标(双击加点, 旁边显示该处的数值) ------------------------------ */
    /*!
     * 在数据坐标 X 处加一个游标; 附近已经有游标就把那一个删掉(双击同一个位置 = 取消)。
     * @return 新游标的序号(从 0 开始, 按 X 升序); 删掉了就返回 -1。
     */
    int    addCursor(double x);
    void   clearCursors();                        ///< 清掉所有游标
    int    cursorCount() const;
    double cursorX(int index) const;              ///< 越界返回 0
    QVector<double> cursors() const;

    void   setMaxPointsPerCurve(int maxPoints);   ///< 防止长时间运行吃光内存

    QSize  minimumSizeHint() const override;
    QSize  sizeHint() const override;

signals:
    /*!
     * Shift+滚轮缩了纵轴, 控件已把自动缩放关掉并接管为手动区间。
     * 页面上据此取消"纵轴自动缩放"勾选、并把当前区间回填到输入框。
     */
    void autoScaleYTakenOver();

    /*!
     * **Ctrl+双击**这张图: 页面上把这张图铺满绘图区(隐藏另一张), 再 Ctrl+双击还原。
     * 普通的双击没有修饰键, 现在是"在这里加一个游标", 两者不冲突。
     */
    void maximizeRequested();

    /*! 游标集合变了(加了/删了/清空) —— 页面上可以顺手刷新个提示。 */
    void cursorsChanged();

   protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRectF plotRect() const;
    double xLo() const;
    double xHi() const;
    bool   dataBounds(double *firstX, double *lastX) const;
    bool   visibleYBounds(double *lo, double *hi) const;   ///< 显示中的曲线在窗口内的 y 范围
    void   autoYRange(double *lo, double *hi) const;        ///< 现算一次自动缩放区间
    void   refreshAutoYRange() const;                       ///< 绘制过程中算一次, 结果进缓存

    /*!
     * **数据路径专用**的重绘节流(2026-10-16 加, 修"图像绘制过多导致卡顿")。
     *
     * 遥测是 100 Hz 来的, 但屏幕不需要 100 Hz: 一次重绘要画 8 条曲线 × 上万点(还带
     * 抗锯齿), 一次几十毫秒; 每条数据都 update() 的话, 重绘请求会越堆越多, 界面就像
     * 卡住了。这里改成"打脏标记 + 33 ms 的合并定时器"(约 30 Hz)。
     * ⚠ 只用在 appendPoint/appendPoints 这条热路径上; 用户操作(勾选/拖动/缩放)仍然
     *   立刻 update() —— 那些是一次性的, 不需要也不会被节流。
     */
    void requestRepaint();

    /*! 超出点数上限时按"留 10% 余量"的方式裁剪(见 .cpp 里的说明). */
    void trimCurve(Curve &c);

    void drawBackgroundAndGrid(QPainter &painter, const QRectF &rect);
    void drawCurves(QPainter &painter, const QRectF &rect);
    void drawLegend(QPainter &painter, const QRectF &rect);
    void drawCrosshair(QPainter &painter, const QRectF &rect);
    void drawCursors(QPainter &painter, const QRectF &rect);

    /*! 某条曲线在 x 处的取值(取最近的采样点); 没有数据返回 false。 */
    static bool valueAtX(const Curve &CurveRef, double X, double *Out);

    QVector<Curve> m_curves;

    /*! 游标(双击加点)的横坐标, 按 X 升序。只存 X —— 数据更新后读数是实时重算的。 */
    QVector<double> m_cursors;

    QString m_xTitle = QStringLiteral("时间");

    XMode  m_xMode  = XMode::Scroll;
    double m_xSpan  = 100.0;      ///< 默认窗口 = 100 个采样点(与参考图一致)
    double m_xFixLo = 0.0;
    double m_xFixHi = 100.0;
    double m_yLo    = -2500.0;    ///< 默认纵轴区间(与参考图一致, 可在"控制参数预设"里改)
    double m_yHi    = 500.0;

    /*!
     * 纵轴自动缩放: m_autoYLo/m_autoYHi 是本次 paintEvent 内算出来的区间(绘制期间用缓存,
     * 保证网格/曲线/准星拿到同一个区间); 绘制之外调用 yLo()/yHi() 一律现算, 不会读到
     * 数据或横轴窗口变化之前的旧值。
     */
    bool          m_autoScaleY  = false;
    bool          m_painting    = false;
    mutable bool   m_autoYValid = false;
    mutable double m_autoYLo    = 0.0;
    mutable double m_autoYHi    = 0.0;

    bool   m_legendVisible = true;
    int    m_maxPoints     = 200000;

    /*! 数据路径的重绘节流(见 requestRepaint 的说明): 脏标记 + 单次定时器。 */
    QTimer *m_repaintTimer = nullptr;
    bool    m_dirty        = false;

    /*!
     * 上一帧是不是"密集/毛数据"(每列一条竖线的那种)。
     *
     * ⚠ 这种帧的代价是**光栅化的像素数**, 不是点数: 满量程乱跳(ADC 通道悬空)时每列要画
     *   一条跨满高的竖线, 单帧就是几百万像素 × 曲线条数。这种画面帧率降下来肉眼几乎无感
     *   (看到的还是那条带), 而 CPU 按比例降 —— 所以密集时把重绘间隔从 33 ms 放到 100 ms。
     */
    bool m_lastFrameDense = false;

    double m_lastPaintMs = 0.0;     ///< 上一帧重绘耗时(ms) —— 决定下一次重绘间隔/笔画粗细

    bool   m_hovering  = false;
    bool   m_panning   = false;
    bool   m_pressed   = false;
    QPoint m_pressPos;
    QPoint m_lastPos;
    double m_pressXLo  = 0.0;
    double m_pressXHi  = 0.0;
};

#endif // PLOTWIDGET_H
