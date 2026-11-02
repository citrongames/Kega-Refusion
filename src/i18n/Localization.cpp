#include "Localization.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>
#include <algorithm>
#include <memory>

static void initializeLocalizationResources() { Q_INIT_RESOURCE(i18n); }

namespace {
struct Catalog {
    QString code;
    QString name;
    QHash<QString, QString> strings;
};

QHash<QString, QString> english;
QHash<QString, QString> active;
QList<Localization::LanguageInfo> languages;
QString currentCode = QStringLiteral("en");
QString selectedCode = QStringLiteral("system");
std::unique_ptr<QTranslator> qtTranslator;

QString normalizeCode(QString code) {
    return code.trimmed().replace(QLatin1Char('-'), QLatin1Char('_')).toLower();
}

Catalog readCatalog(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024) return {};
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return {};
    const QJsonObject root = document.object();
    Catalog catalog;
    catalog.code = normalizeCode(root.value(QStringLiteral("code")).toString());
    catalog.name = root.value(QStringLiteral("name")).toString().trimmed();
    if (catalog.code.isEmpty() || catalog.name.isEmpty() ||
        catalog.code != normalizeCode(QFileInfo(path).completeBaseName()) ||
        !root.value(QStringLiteral("translations")).isObject()) return {};
    const QJsonObject entries = root.value(QStringLiteral("translations")).toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it)
        if (it.value().isString() && !it.value().toString().isEmpty())
            catalog.strings.insert(it.key(), it.value().toString());
    return catalog;
}

void merge(QHash<QString, QString>& target, const QHash<QString, QString>& incoming) {
    for (auto it = incoming.cbegin(); it != incoming.cend(); ++it)
        target.insert(it.key(), it.value());
}
}

namespace Localization {
void initialize(const QString& applicationDirectory, const QString& dataDirectory) {
    initializeLocalizationResources();
    if (qtTranslator) QCoreApplication::removeTranslator(qtTranslator.get());
    qtTranslator.reset();
    english.clear();
    active.clear();
    languages.clear();
    const QStringList directories{
        QDir(applicationDirectory).filePath(QStringLiteral("languages")),
        QDir(dataDirectory).filePath(QStringLiteral("languages"))};
    QHash<QString, Catalog> catalogs;
    Catalog embeddedEnglish = readCatalog(QStringLiteral(":/languages/en.json"));
    if (!embeddedEnglish.code.isEmpty()) catalogs.insert(embeddedEnglish.code, embeddedEnglish);
    for (const QString& directory : directories) {
        const QFileInfoList files = QDir(directory).entryInfoList(
            QStringList{QStringLiteral("*.json")}, QDir::Files | QDir::Readable, QDir::Name);
        for (const QFileInfo& file : files) {
            const Catalog catalog = readCatalog(file.absoluteFilePath());
            if (catalog.code.isEmpty()) continue;
            Catalog& existing = catalogs[catalog.code];
            existing.code = catalog.code;
            existing.name = catalog.name;
            merge(existing.strings, catalog.strings);
        }
    }
    english = catalogs.value(QStringLiteral("en")).strings;
    QSettings settings(QDir(dataDirectory).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    selectedCode = normalizeCode(settings.value(QStringLiteral("ui/language"),
                                               QStringLiteral("system")).toString());
    QString requested = selectedCode == QLatin1String("system")
        ? normalizeCode(QLocale::system().name()) : selectedCode;
    if (!catalogs.contains(requested)) requested = requested.section(QLatin1Char('_'), 0, 0);
    if (!catalogs.contains(requested)) requested = QStringLiteral("en");
    currentCode = requested;
    active = catalogs.value(currentCode).strings;
    if (currentCode != QLatin1String("en")) {
        const QString fileName = QStringLiteral("qtbase_%1.qm").arg(currentCode);
        const QStringList qtDirectories{
            QDir(dataDirectory).filePath(QStringLiteral("languages")),
            QDir(applicationDirectory).filePath(QStringLiteral("languages")),
            QLibraryInfo::path(QLibraryInfo::TranslationsPath)};
        for (const QString& directory : qtDirectories) {
            auto candidate = std::make_unique<QTranslator>();
            if (candidate->load(fileName, directory)) {
                QCoreApplication::installTranslator(candidate.get());
                qtTranslator = std::move(candidate);
                break;
            }
        }
    }
    for (auto it = catalogs.cbegin(); it != catalogs.cend(); ++it)
        languages.append({it.key(), it.value().name});
    std::sort(languages.begin(), languages.end(), [](const LanguageInfo& a, const LanguageInfo& b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
}

QString text(const QString& source) {
    const auto translated = active.constFind(source);
    if (translated != active.cend()) return translated.value();
    const auto fallback = english.constFind(source);
    return fallback == english.cend() ? source : fallback.value();
}

QList<LanguageInfo> availableLanguages() { return languages; }
QString currentLanguage() { return currentCode; }
QString selectedLanguage() { return selectedCode; }
}
