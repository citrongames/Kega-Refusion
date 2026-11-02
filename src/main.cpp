#include "i18n/Localization.h"
#include "core/EmulationSession.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLibraryInfo>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMessageBox>
#include <QTimer>
#include <QSslSocket>
#include <QtGlobal>
#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <atomic>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#include <vector>
#endif

namespace {
QString executableDirectory() {
#ifdef Q_OS_WIN
    // Resolve the module itself, not argv[0] or the process working directory.
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size())
            return QFileInfo(QString::fromWCharArray(buffer.data(), int(length))).absolutePath();
        if (buffer.size() >= 32768) return {};
        buffer.resize(qMin<std::size_t>(buffer.size() * 2, 32768));
    }
#elif defined(Q_OS_LINUX)
    const QString target = QFileInfo(QStringLiteral("/proc/self/exe")).symLinkTarget();
    return target.isEmpty() ? QString() : QFileInfo(target).absolutePath();
#else
    return {};
#endif
}

void configurePluginPaths() {
    const QString directory = executableDirectory();
    if (directory.isEmpty()) return;
    QStringList paths;
    const QString bundled = QDir(directory).filePath(QStringLiteral("plugins"));
    if (QDir(bundled).exists()) paths.append(bundled);
    const QString installed = QLibraryInfo::path(QLibraryInfo::PluginsPath);
    if (!installed.isEmpty() && !paths.contains(installed)) paths.append(installed);
    for (const QString& path : QCoreApplication::libraryPaths())
        if (!paths.contains(path)) paths.append(path);
    QCoreApplication::setLibraryPaths(paths);
}
}

int main(int argc, char* argv[]) {
    configurePluginPaths(); // The platform plugin loads in QApplication's constructor.
    QApplication app(argc, argv);
    // Select before achievements or any other SSL client initializes a backend.
    // Direct netplay uses PSK ciphers supplied by the bundled OpenSSL backend.
    if(QSslSocket::availableBackends().contains(QStringLiteral("openssl")))
        QSslSocket::setActiveBackend(QStringLiteral("openssl"));
    QApplication::setApplicationName(QStringLiteral("Kega Refusion"));
    QApplication::setOrganizationName(QStringLiteral("Kega Refusion"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    const QString appDirectory = QCoreApplication::applicationDirPath();
    Localization::initialize(appDirectory,
        QDir(appDirectory).filePath(QStringLiteral("data")));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        Localization::text(QStringLiteral("Kega Refusion — Mega Drive, Sega CD и Master System")));
    parser.addHelpOption();
    QCommandLineOption performanceOption(QStringList{QStringLiteral("performance-check")},
        Localization::text(QStringLiteral("Измерить работу окна в течение 10 секунд")));
    parser.addOption(performanceOption);
    QCommandLineOption coreOption(
        QStringList{QStringLiteral("core")},
        Localization::text(QStringLiteral("Путь к ядру Genesis Plus GX Libretro")),
        QStringLiteral("path"));
    QCommandLineOption dataOption(
        QStringList{QStringLiteral("data")},
        Localization::text(QStringLiteral("Каталог настроек и сохранений")),
        QStringLiteral("path"));
    QCommandLineOption smokeOption(
        QStringList{QStringLiteral("smoke-test")},
        Localization::text(QStringLiteral("Показать окно на 3 секунды и завершить с кодом проверки")));
    QCommandLineOption screenshotOption(
        QStringList{QStringLiteral("screenshot")},
        Localization::text(QStringLiteral("Снимок окна на второй секунде проверки")),
        QStringLiteral("path"));
    QCommandLineOption fullscreenOption(
        QStringList{QStringLiteral("fullscreen")},
        Localization::text(QStringLiteral("Начать в полноэкранном режиме")));
    QCommandLineOption osdDemoOption(
        QStringList{QStringLiteral("osd-demo")},
        Localization::text(QStringLiteral("Показать образцы экранных сообщений при проверке")));
    QCommandLineOption keyTestOption(
        QStringList{QStringLiteral("smoke-fullscreen-keys")},
        Localization::text(QStringLiteral("При проверке нажать Esc, затем Alt+Enter")));
    parser.addOption(coreOption);
    parser.addOption(dataOption);
    parser.addOption(smokeOption);
    parser.addOption(screenshotOption);
    parser.addOption(fullscreenOption);
    parser.addOption(osdDemoOption);
    parser.addOption(keyTestOption);
    parser.addPositionalArgument(QStringLiteral("rom"),
                                 Localization::text(QStringLiteral("Необязательный путь к игре")));
    parser.process(app);

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
    if (parser.isSet(dataOption)) Localization::initialize(appDirectory, dataPath);
    if (!QDir().mkpath(dataPath)) {
        QMessageBox::critical(nullptr, QStringLiteral("Kega Refusion"),
                              Localization::text(QStringLiteral("Не удалось создать каталог данных: %1"))
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
    std::atomic<int> produced{0};
    int delivered = 0, swapped = 0;
    QElapsedTimer performanceClock;
    performanceClock.start();
    if (parser.isSet(performanceOption)) {
        QObject::connect(&session, &EmulationSession::videoFrame, &app,
            [&](const QImage&, double) { ++produced; }, Qt::DirectConnection);
        QObject::connect(&session, &EmulationSession::videoFrameReady, &app,[&] { ++delivered; });
    }
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
    if (parser.isSet(performanceOption)) {
        QTimer::singleShot(500, &app,[&] {
            if (auto* gpu = window.findChild<QOpenGLWidget*>(QStringLiteral("gpuVideo")))
                QObject::connect(gpu, &QOpenGLWidget::frameSwapped, &app,[&] { ++swapped; });
        });
        auto* report = new QTimer(&app);
        QObject::connect(report, &QTimer::timeout, &app,[&] {
            std::printf("PERF ms=%lld core=%d gui=%d swaps=%d\n",
                static_cast<long long>(performanceClock.elapsed()), produced.load(), delivered, swapped);
            std::fflush(stdout);
        });
        report->start(1000);
        QTimer::singleShot(10000, &app,[&] { app.quit(); });
    }
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
