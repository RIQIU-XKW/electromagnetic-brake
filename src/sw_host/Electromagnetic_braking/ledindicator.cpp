#include "ledindicator.h"

#include <QPainter>
#include <QRadialGradient>

LedIndicator::LedIndicator(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(28, 28);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setToolTip(QStringLiteral("绿灯 = 已连接; 灰灯 = 未连接"));
}

bool LedIndicator::isOn() const
{
    return m_on;
}

void LedIndicator::setOn(bool on)
{
    if (m_on == on)
        return;

    m_on = on;
    update();
}

void LedIndicator::setOnColor(const QColor &color)
{
    m_onColor = color;
    update();
}

QColor LedIndicator::onColor() const
{
    return m_onColor;
}

void LedIndicator::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), Qt::transparent);

    const qreal d = std::min(width(), height()) - 2.0;
    if (d <= 2.0)
        return;

    const QRectF outer((width() - d) / 2.0, (height() - d) / 2.0, d, d);
    const QRectF inner = outer.adjusted(d * 0.22, d * 0.22, -d * 0.22, -d * 0.22);

    // 外圈: 深色灯罩
    QRadialGradient shade(outer.center(), d / 2.0);
    shade.setColorAt(0.0, QColor(0x6E, 0x6E, 0x6E));
    shade.setColorAt(1.0, QColor(0x2A, 0x2A, 0x2A));
    painter.setPen(QPen(QColor(0x1A, 0x1A, 0x1A), 1));
    painter.setBrush(shade);
    painter.drawEllipse(outer);

    // 内圈: 亮/灭
    if (m_on) {
        QRadialGradient glow(inner.center() - QPointF(d * 0.06, d * 0.06), d * 0.45);
        glow.setColorAt(0.0, m_onColor.lighter(150));
        glow.setColorAt(1.0, m_onColor.darker(130));
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x4A, 0x4A, 0x4A));
    }
    painter.drawEllipse(inner);
}

QSize LedIndicator::sizeHint() const
{
    return QSize(30, 30);
}

QSize LedIndicator::minimumSizeHint() const
{
    return QSize(20, 20);
}
