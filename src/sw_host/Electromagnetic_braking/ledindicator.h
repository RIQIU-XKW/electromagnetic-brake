#ifndef LEDINDICATOR_H
#define LEDINDICATOR_H

#include <QColor>
#include <QWidget>

/*!
 * \brief 圆形状态指示灯(圆点)。
 *
 * 点亮 = 连接正常(默认绿色), 熄灭 = 未连接(深灰)。参考图右上角那颗绿点。
 */
class LedIndicator : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(bool on READ isOn WRITE setOn)

public:
    explicit LedIndicator(QWidget *parent = nullptr);

    bool isOn() const;
    void setOn(bool on);

    /*! 点亮时的颜色, 默认绿色。 */
    void    setOnColor(const QColor &color);
    QColor  onColor() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    bool   m_on = false;
    QColor m_onColor = QColor(0x21, 0xC0, 0x3A);
};

#endif // LEDINDICATOR_H
