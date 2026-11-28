#include "BundledLuaFiles.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>

namespace {
QString digest(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
bool atomicWrite(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
}

bool installBundledLuaFiles(const QString& app, const QString& scripts,
                            const QString& data, const QString& source) {
    QMap<QString, QByteArray> files;
    QDirIterator it(source, QDir::Files, QDirIterator::Subdirectories);
    const QString prefix = source + QLatin1Char('/');
    while (it.hasNext()) {
        const QString path = it.next();
        const QString name = path.mid(prefix.size());
        if (!name.startsWith(QLatin1String("scripts/")) && !name.startsWith(QLatin1String("docs/")))
            continue;
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly)) return false;
        files.insert(name, input.readAll());
    }
    if (files.isEmpty()) return false;
    // Hash embedded memory only. On an unchanged launch, read one tiny receipt
    // and do no filesystem scans or writes to the scripts/docs directories.
    QCryptographicHash revisionHash(QCryptographicHash::Sha256);
    for (auto i = files.cbegin(); i != files.cend(); ++i) {
        revisionHash.addData(i.key().toUtf8());
        revisionHash.addData(QByteArray(1, '\0'));
        revisionHash.addData(i.value());
    }
    const QString revision = QString::fromLatin1(revisionHash.result().toHex());
    const QString receipt = QDir(data).filePath(QStringLiteral("lua-bundle.json"));
    QFile priorFile(receipt);
    QJsonObject prior;
    if (priorFile.open(QIODevice::ReadOnly) && priorFile.size() <= 64 * 1024)
        prior = QJsonDocument::fromJson(priorFile.readAll()).object();
    priorFile.close();
    if (prior.value(QStringLiteral("revision")).toString() == revision &&
        prior.value(QStringLiteral("application")).toString() == app &&
        prior.value(QStringLiteral("scripts")).toString() == scripts) return true;
    const auto owned = prior.value(QStringLiteral("files")).toObject();
    QJsonObject installed;
    bool complete = true;
    for (auto i = files.cbegin(); i != files.cend(); ++i) {
        const bool isScript = i.key().startsWith(QLatin1String("scripts/"));
        const QString target = isScript ? QDir(scripts).filePath(i.key().mid(8))
                                        : QDir(app).filePath(i.key());
        const QString expected = digest(i.value());
        const QFileInfo info(target);
        if (info.isSymLink()) continue;
        if (info.exists()) {
            QFile existing(target);
            if (!existing.open(QIODevice::ReadOnly) || existing.size() > 4 * 1024 * 1024) {
                continue;
            }
            const QString current = digest(existing.readAll());
            if (current == expected) {
                installed.insert(i.key(), expected);
                continue;
            }
            // Existing files without a matching receipt belong to the user.
            if (owned.value(i.key()).toString() != current) continue;
        }
        if (atomicWrite(target, i.value())) installed.insert(i.key(), expected);
        else complete = false;
    }
    // Failed installation remains retryable. Modified user files are preserved
    // and intentionally removed from our ownership receipt.
    if (!complete) return false;
    const QJsonObject result{{QStringLiteral("revision"), revision},
                             {QStringLiteral("application"), app},
                             {QStringLiteral("scripts"), scripts},
                             {QStringLiteral("files"), installed}};
    return atomicWrite(receipt, QJsonDocument(result).toJson(QJsonDocument::Compact));
}
