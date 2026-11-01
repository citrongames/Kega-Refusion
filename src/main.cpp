#include "core/EmulationSession.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPixmap>
#include <QTimer>
#include <QtGlobal>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Kega Refusion"));
    QApplication::setOrganizationName(QStringLiteral("Kega Refusion"));

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
    parser.addOption(coreOption);
    parser.addOption(dataOption);
    QCommandLineOption screenshotOption(
        QStringList{QStringLiteral("screenshot")},
        QStringLiteral("Снимок окна на второй секунде проверки"),
        QStringLiteral("path"));
    parser.addOption(smokeOption);
    parser.addOption(screenshotOption);
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
                         [&](const QImage& frame, double) { frameSeen = frameSeen || !frame.isNull(); });
    }

    MainWindow window(session, dataPath, smokeTest);
    window.show();
    if (!romPath.isEmpty())
        QTimer::singleShot(0, &window, [&window, romPath] { window.openRom(romPath); });

    if (smokeTest) {
        if (!screenshotPath.isEmpty()) {
            QTimer::singleShot(2000, &window, [&] {
                if (!QDir().mkpath(QFileInfo(screenshotPath).absolutePath()) ||
                    !window.grab().save(screenshotPath)) {
                    screenshotError = true;
                    qCritical("Could not save smoke-test screenshot");
                }
            });
        }
        QTimer::singleShot(3000, &app, [&] {
            app.exit(smokeError ? 2
                : screenshotError ? 4
                : (!romPath.isEmpty() && !frameSeen ? 3 : 0));
        });
    }
    const int result = app.exec();
    session.shutdown();
    return result;
}
