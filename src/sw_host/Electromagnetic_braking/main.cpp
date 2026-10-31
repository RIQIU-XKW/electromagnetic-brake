#include "widget.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // QSettings 用这两项定位 ini 文件(网口地址 / PWM 参数 / 预设方案都存在里面)
    QApplication::setOrganizationName(QStringLiteral("Zynq_Base_Platform"));
    QApplication::setApplicationName(QStringLiteral("Electromagnetic_braking"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));

    /*
     * 程序图标(**窗口标题栏 + 任务栏**都靠它)。
     *
     * ⚠ 图片是从 Qt 资源里来的(:/branding/app.png, 见 resources.qrc): 编进 exe,
     *   不依赖旁边有没有图片文件; PNG 又是 QtGui 内置格式, 不需要 imageformats 插件。
     * ⚠ 这里只能管"运行起来以后窗口/任务栏上显示什么"; **资源管理器里那个 exe 文件图标**
     *   是 Windows 资源(见 app.rc / app.ico), 由链接器写进 exe 头部, 跟这行无关。
     * ⚠ setWindowIcon 必须在建窗口**之前**调用, 否则第一个窗口会先按默认图标建出来。
     */
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/branding/app.png")));

    Widget w;
    w.show();
    return QApplication::exec();
}
