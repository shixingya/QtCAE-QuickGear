#include "app/MainWindow.h"
#include "core/ISolverBackend.h"

#include <QApplication>
#include <QFont>
#include <QStyleFactory>

/**
 * 程序入口。
 *
 * 壳工程可以直接运行：未接入闭源库时使用内置示意后端，
 * 界面/流程/授权链路与正式版一致，方便演示与联调。
 */
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("QtCAE-QuickGear"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("shixingya"));

    // 自定义信号参数类型（跨线程排队需要）
    qRegisterMetaType<GearParams>("GearParams");
    qRegisterMetaType<SolveResult>("SolveResult");

    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFont appFont(QStringLiteral("Microsoft YaHei UI"), 9);
    appFont.setStyleStrategy(QFont::PreferAntialias);
    QApplication::setFont(appFont);

    // 与演示网页同一套色板：深青灰主色 + 低饱和灰底
    app.setStyleSheet(QStringLiteral(R"(
        QMainWindow, QDialog { background: #F8FAFC; }
        QWidget { color: #0F172A; }
        QMenuBar { background: #FFFFFF; border-bottom: 1px solid #E4E7EB; padding: 2px 4px; }
        QMenuBar::item { padding: 4px 10px; background: transparent; }
        QMenuBar::item:selected { background: #EEF2F6; border-radius: 3px; }
        QMenu { background: #FFFFFF; border: 1px solid #E4E7EB; }
        QMenu::item { padding: 6px 24px 6px 20px; }
        QMenu::item:selected { background: #EEF2F6; }

        QGroupBox {
            background: #FFFFFF; border: 1px solid #E4E7EB; border-radius: 5px;
            margin-top: 16px; padding: 12px 12px 8px 12px; font-weight: 600; color: #475569;
        }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }

        QSpinBox, QDoubleSpinBox, QComboBox, QLineEdit {
            background: #FFFFFF; border: 1px solid #D8DEE6; border-radius: 4px;
            padding: 4px 8px; min-height: 22px; selection-background-color: #1E3A5F;
        }
        QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QLineEdit:focus {
            border-color: #1E3A5F;
        }
        QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled, QLineEdit:disabled {
            background: #F1F3F5; color: #8A97A6;
        }

        QPushButton#runButton {
            background: #1E3A5F; color: #FFFFFF; border: none; border-radius: 4px;
            font-size: 13px; font-weight: 600;
        }
        QPushButton#runButton:hover { background: #274C78; }
        QPushButton#runButton:disabled { background: #A7B4C2; }

        QPushButton#primaryButton {
            background: #1E3A5F; color: #FFFFFF; border: none; border-radius: 4px;
            padding: 6px 14px; min-height: 26px;
        }
        QPushButton#primaryButton:hover { background: #274C78; }
        QPushButton#ghostButton {
            background: #FFFFFF; color: #0F172A; border: 1px solid #D8DEE6;
            border-radius: 4px; padding: 6px 14px; min-height: 26px;
        }
        QPushButton#ghostButton:hover { background: #F1F3F5; }

        QLabel#dimensionHint, QLabel#hintLabel { color: #8A97A6; font-weight: 400; }
        QLabel#sectionTitle { color: #475569; font-weight: 600; }
        QLabel#metricTitle { color: #8A97A6; font-size: 11px; }
        QLabel#metricValue { color: #0F172A; font-size: 19px; font-weight: 600; }
        QWidget#metricBox { background: #FFFFFF; border: 1px solid #E4E7EB; border-radius: 5px; }

        QStatusBar { background: #FFFFFF; border-top: 1px solid #E4E7EB; }
        QStatusBar QLabel { color: #475569; padding: 0 6px; }

        QProgressBar {
            border: 1px solid #E4E7EB; border-radius: 3px; background: #F1F3F5;
            height: 16px; text-align: center; color: #475569; font-size: 11px;
        }
        QProgressBar::chunk { background: #1E3A5F; border-radius: 2px; }
    )"));

    MainWindow window;
    window.show();

    return app.exec();
}
