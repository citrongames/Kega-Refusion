#include "DataFolders.h"
#include "i18n/Localization.h"

#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>

namespace {
using Folders = QMap<QString, QString>;

#ifdef Q_OS_WIN
const QString platformGroup = QStringLiteral("folders/windows/");
constexpr auto pathCase = Qt::CaseInsensitive;
#else
const QString platformGroup = QStringLiteral("folders/linux/");
constexpr auto pathCase = Qt::CaseSensitive;
#endif
const QString relativeGroup = QStringLiteral("folders/relative/");
QMutex cacheMutex;
QString cachedRoot;
Folders cachedFolders;

QString settingsFile(const QString& root) {
    return QDir(root).filePath(QStringLiteral("settings.ini"));
}

Folders readFolders(QSettings& settings) {
    Folders values;
    for (const auto& name : DataFolders::categories()) {
        const QString local = settings.value(platformGroup + name).toString();
        values.insert(name, local.isEmpty() ? settings.value(relativeGroup + name, name).toString() : local);
    }
    return values;
}

Folders configuredFolders(const QString& dataRoot) {
    const QString root = QDir(dataRoot).absolutePath();
    QMutexLocker lock(&cacheMutex);
    if (cachedRoot != root || cachedFolders.isEmpty()) {
        QSettings settings(settingsFile(root), QSettings::IniFormat);
        cachedFolders = readFolders(settings);
        cachedRoot = root;
    }
    return cachedFolders;
}

QString resolve(const QString& root, const QString& configured) {
    return QDir::cleanPath(QDir(root).absoluteFilePath(configured));
}

// Resolve symlinks in an existing parent even if the selected folder is new.
QString physicalPath(QString path) {
    QStringList missing;
    for (;;) {
        const QFileInfo info(path);
        const QString existing = info.canonicalFilePath();
        if (!existing.isEmpty())
            return missing.isEmpty() ? existing : QDir(existing).filePath(missing.join('/'));
        const QString parent = info.absolutePath();
        if (parent == path) return QDir(path).filePath(missing.join('/'));
        missing.prepend(info.fileName());
        path = parent;
    }
}

bool sessionTemporaryPath(const QString& root, const QString& path) {
    const QString physical = physicalPath(path);
    for (const auto& subdir : {"netplay/downloads", "netplay/sessions"}) {
        const QString reserved = physicalPath(QDir(root).filePath(QString::fromLatin1(subdir)));
        if (!physical.compare(reserved, pathCase) || physical.startsWith(reserved + '/', pathCase))
            return true;
    }
    return false;
}

bool validPath(const QString& value) {
    if (value.contains(QChar::Null) || value.startsWith(QLatin1String(":/"))) return false;
    const bool drive = value.size() >= 2 && value.at(0).isLetter() && value.at(1) == ':';
#ifdef Q_OS_WIN
    if (drive && (value.size() < 3 || value.at(2) != '/')) return false;
    if (value.startsWith('/') && !value.startsWith(QLatin1String("//"))) return false;
#else
    if (drive || value.startsWith(QLatin1String("\\\\"))) return false;
#endif
    return true;
}
}

QStringList DataFolders::categories() {
    return {QStringLiteral("bios"), QStringLiteral("cheats"), QStringLiteral("movies"),
            QStringLiteral("saves"), QStringLiteral("screenshots"), QStringLiteral("states"),
            QStringLiteral("videos")};
}

QString DataFolders::defaultPath(const QString& dataRoot, const QString& category) {
    return resolve(dataRoot, category);
}

QString DataFolders::configuredPath(const QString& dataRoot, const QString& category) {
    return configuredFolders(dataRoot).value(category, category);
}

QString DataFolders::path(const QString& dataRoot, const QString& category) {
    return resolve(dataRoot, configuredPath(dataRoot, category));
}

QMap<QString, QString> DataFolders::paths(const QString& dataRoot) {
    auto result = configuredFolders(dataRoot);
    for (auto it = result.begin(); it != result.end(); ++it)
        it.value() = resolve(dataRoot, it.value());
    return result;
}

bool DataFolders::save(const QString& dataRoot, const Folders& changes,
                       QString& error, QString* failedCategory) {
    error.clear();
    if (failedCategory) failedCategory->clear();
    if (changes.isEmpty()) return true;
    const QString root = QDir(dataRoot).absolutePath();
    Folders validated;
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        const QString& key = it.key();
        const QString input = QDir::fromNativeSeparators(it.value().trimmed());
        const QString value = input.isEmpty() ? key : QDir::cleanPath(input);
        const QString folder = resolve(root, value);
        const auto fail = [&](const QString& message, const QString& argument) {
            error = Localization::text(message).arg(QDir::toNativeSeparators(argument));
            if (failedCategory) *failedCategory = key;
            return false;
        };
        if (!categories().contains(key) || !validPath(input))
            return fail(QStringLiteral("Недопустимый путь к папке: %1"), input);
        if (sessionTemporaryPath(root, folder))
            return fail(QStringLiteral("Эта папка используется для временных файлов: %1"), folder);
        if (!QDir().mkpath(folder))
            return fail(QStringLiteral("Не удалось создать папку: %1"), folder);
        const QFileInfo info(folder);
        if (!info.isReadable() || (key != QLatin1String("bios") && !info.isWritable()))
            return fail(QStringLiteral("Нет доступа к папке: %1"), folder);
        validated.insert(key, value);
    }

    QMutexLocker lock(&cacheMutex);
    QSettings settings(settingsFile(root), QSettings::IniFormat);
    for (auto it = validated.cbegin(); it != validated.cend(); ++it) {
        const QString localKey = platformGroup + it.key();
        const QString sharedKey = relativeGroup + it.key();
        if (QDir::isAbsolutePath(it.value())) {
            settings.setValue(localKey, it.value());
        } else {
            settings.remove(localKey);
            if (it.value() == it.key()) settings.remove(sharedKey);
            else settings.setValue(sharedKey, it.value());
        }
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        error = Localization::text(QStringLiteral("Не удалось сохранить настройки папок: %1"))
            .arg(QDir::toNativeSeparators(settings.fileName()));
        return false;
    }
    cachedRoot = root;
    cachedFolders = readFolders(settings);
    return true;
}
