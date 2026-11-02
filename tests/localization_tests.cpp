#undef NDEBUG
#include "i18n/Localization.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <cassert>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    assert(root.isValid());
    const QString appPath = QDir(root.path()).filePath(QStringLiteral("app"));
    const QString dataPath = QDir(root.path()).filePath(QStringLiteral("data"));
    assert(QDir().mkpath(appPath));
    assert(QDir().mkpath(QDir(dataPath).filePath(QStringLiteral("languages"))));
    QSettings settings(QDir(dataPath).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ui/language"), QStringLiteral("unavailable"));
    settings.sync();
    Localization::initialize(appPath, dataPath);
    assert(Localization::currentLanguage() == QLatin1String("en"));
    assert(Localization::text(QStringLiteral("Достижения выключены")) ==
           QLatin1String("Achievements disabled"));

    QJsonObject partial;
    partial.insert(QStringLiteral("code"), QStringLiteral("xx"));
    partial.insert(QStringLiteral("name"), QStringLiteral("Test language"));
    partial.insert(QStringLiteral("translations"),
        QJsonObject{{QStringLiteral("Пауза"), QStringLiteral("Custom pause")}});
    QFile catalog(QDir(dataPath).filePath(QStringLiteral("languages/xx.json")));
    assert(catalog.open(QIODevice::WriteOnly));
    assert(catalog.write(QJsonDocument(partial).toJson()) > 0);
    catalog.close();
    settings.setValue(QStringLiteral("ui/language"), QStringLiteral("xx"));
    settings.sync();
    Localization::initialize(appPath, dataPath);
    assert(Localization::currentLanguage() == QLatin1String("xx"));
    assert(Localization::text(QStringLiteral("Пауза")) == QLatin1String("Custom pause"));
    assert(Localization::text(QStringLiteral("Достижения выключены")) ==
           QLatin1String("Achievements disabled"));
    return 0;
}
