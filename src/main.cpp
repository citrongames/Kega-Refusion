#include "core/EmulationSession.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMessageBox>
#include <QTimer>
#include <QtGlobal>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Kega Refusion"));
    QApplication::setOrganizationName(QStringLiteral("Kega Refusion"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Kega Refusion — Mega Drive и Master System"));
    parser.addHelpOption();
    QCommandLineOption coreOption(
        QStringList{QStringLiteral("core")},
        QStringLiteral("Путь к ядру Genesis Plus GX Libretro"),
        QStringLiteral("path"));
    QCommandLineOption dataOption(
        QStringList{QStringLiteral("data")},
        QStringLiteral("Каталог настроек и сохранений"),
        QStringLiteral("path"));
    QCommandLineOption smokeOption(
        QStringList{QStringLiteral("smoke-test")},
        QStringLiteral("Показать окно на 3 секунды и завершить с кодом проверки"));
    QCommandLineOption screenshotOption(
        QStringList{QStringLiteral("screenshot")},
        QStringLiteral("Снимок окна на второй секунде проверки"),
        QStringLiteral("path"));
    QCommandLineOption fullscreenOption(
        QStringList{QStringLiteral("fullscreen")},
        QStringLiteral("Начать в полноэкранном режиме"));
    QCommandLineOption osdDemoOption(
        QStringList{QStringLiteral("osd-demo")},
        QStringLiteral("Показать образцы экранных сообщений при проверке"));
    QCommandLineOption keyTestOption(
        QStringList{QStringLiteral("smoke-fullscreen-keys")},
        QStringLiteral("При проверке нажать Esc, затем Alt+Enter"));
    parser.addOption(coreOption);
    parser.addOption(dataOption);
    parser.addOption(smokeOption);
    parser.addOption(screenshotOption);
    parser.addOption(fullscreenOption);
    parser.addOption(osdDemoOption);
    parser.addOption(keyTestOption);
    parser.addPositionalArgument(QStringLiteral("rom"),
                                 QStringLiteral("Необязательный путь к игре"));
    parser.process(app);

    const QString appDirectory = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    const QString defaultCore = QStringLiteral("genesis_plus_gx_libretro.dll");
#else
    const QString defaultCore = QStringLiteral("genesis_plus_gx_libretro.so");
#endif
    const QString corePath = QFileInfo(
        parser.isSet(coreOption)
            ? parser.value(coreOption)
            : QDir(appDirectory).filePath(QStringLiteral("cores/") + defaultCore))
                                 .absoluteFilePath();
    const QString dataPath = QFileInfo(
        parser.isSet(dataOption)
            ? parser.value(dataOption)
            : QDir(appDirectory).filePath(QStringLiteral("data")))
                                 .absoluteFilePath();
    if (!QDir().mkpath(dataPath)) {
        QMessageBox::critical(nullptr, QStringLiteral("Kega Refusion"),
                              QStringLiteral("Не удалось создать каталог данных: %1")
                                  .arg(dataPath));
        return 2;
    }

    const bool smokeTest = parser.isSet(smokeOption);
    if (parser.isSet(screenshotOption) && !smokeTest) {
        qCritical("--screenshot requires --smoke-test");
        return 2;
    }
    if (parser.isSet(keyTestOption) &&
        (!smokeTest || !parser.isSet(fullscreenOption))) {
        qCritical("--smoke-fullscreen-keys requires --smoke-test --fullscreen");
        return 2;
    }
    const QString screenshotPath = parser.isSet(screenshotOption)
        ? QFileInfo(parser.value(screenshotOption)).absoluteFilePath() : QString();
    const QStringList arguments = parser.positionalArguments();
    const QString romPath = arguments.isEmpty()
        ? QString() : QFileInfo(arguments.first()).absoluteFilePath();
    bool smokeError = false;
    bool frameSeen = false;
    bool screenshotError = false;

    EmulationSession session(corePath, dataPath);
    QObject::connect(&session, &EmulationSession::errorOccurred, &app,
                     [&](const QString&) { smokeError = true; });
    if (smokeTest && !romPath.isEmpty()) {
        QObject::connect(&session, &EmulationSession::videoFrame, &app,
                         [&](const QImage& frame, double) {
                             frameSeen = frameSeen || !frame.isNull();
                         });
    }

    MainWindow window(session, dataPath, smokeTest);
    window.show();
    if (parser.isSet(fullscreenOption))
        QTimer::singleShot(0, &window, [&window] { window.setFullscreenMode(true); });
    if (!romPath.isEmpty())
        QTimer::singleShot(0, &window, [&window, romPath] { window.openRom(romPath); });
    if (parser.isSet(osdDemoOption))
        QTimer::singleShot(900, &window, [&window] { window.showOsdDemo(); });
    if (parser.isSet(keyTestOption)) {
        QTimer::singleShot(1100, &window, [&] {
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(&window, &escape);
            if (!window.isFullScreen() || !window.menuBar()->isVisible())
                smokeError = true;
        });
        QTimer::singleShot(2600, &window, [&] {
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(&window, &escape);
            if (!window.isFullScreen() || window.menuBar()->isVisible())
                smokeError = true;
            QTimer::singleShot(100, &window, [&] {
                if (window.centralWidget()->geometry().top() != 0) {
                    qWarning("Fullscreen menu did not collapse: top=%d",
                             window.centralWidget()->geometry().top());
                    smokeError = true;
                }
            });
        });
        QTimer::singleShot(3200, &window, [&] {
            QKeyEvent altEnter(QEvent::KeyPress, Qt::Key_Return, Qt::AltModifier);
            QCoreApplication::sendEvent(&window, &altEnter);
            if (window.isFullScreen() || !window.menuBar()->isVisible())
                smokeError = true;
        });
    }

    if (smokeTest) {
        if (!screenshotPath.isEmpty()) {
            QTimer::singleShot(2000, &window, [&] {
                if (!QDir().mkpath(QFileInfo(screenshotPath).absolutePath()) ||
                    !static_cast<GameView*>(window.centralWidget())->captureImage().save(screenshotPath)) {
                    screenshotError = true;
                    qCritical("Could not save smoke-test screenshot");
                }
            });
        }
        QTimer::singleShot(parser.isSet(keyTestOption) ? 3900 : 3000, &app, [&] {
            app.exit(smokeError ? 2 : screenshotError ? 4 :
                     (!romPath.isEmpty() && !frameSeen ? 3 : 0));
        });
    }
    const int result = app.exec();
    session.shutdown();
    return result;
}
