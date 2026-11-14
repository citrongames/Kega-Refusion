#include "SegaLdBios.h"
#include "DataFolders.h"
#include "i18n/Localization.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <array>

namespace {
constexpr qint64 biosSize = 131072;
struct Image { int region; const char* filename; const char* label; const char* sha256; };
constexpr std::array<Image,2> images{{
    {1,"bios_LD_J.bin","PAC-S1 Japan v1.02","dca942d977217f703d8d1c6eb1aeb6b32c78ecc421486bbb46c459d385161c94"},
    {2,"bios_LD_U.bin","PAC-S10 USA v1.04","e89b5a319f66406611ec82fe5c4aa6827c175a05135bd7bd177366cba0465021"},
}};
const Image* imageFor(int region) {
    for (const auto& image : images) if (image.region == region) return &image;
    return nullptr;
}
const Image* identify(const QByteArray& bytes) {
    if (bytes.size() != biosSize) return nullptr;
    const auto hash = QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
    for (const auto& image : images) if (hash == image.sha256) return &image;
    return nullptr;
}
bool read(const QString& path,QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() != biosSize) return false;
    bytes = file.read(biosSize+1);
    return bytes.size() == biosSize && file.size() == biosSize;
}
struct Recognition { qint64 size; QDateTime modified,changed,created; const Image* image; };
QMutex mutex;
QHash<QString,Recognition> cache;
const Image* recognize(const QFileInfo& info) {
    if (!info.isFile() || !info.isReadable() || info.size() != biosSize) return nullptr;
    const auto path=info.absoluteFilePath();
    {
        QMutexLocker lock(&mutex);
        const auto found=cache.constFind(path);
        if (found!=cache.cend() && found->size==info.size() && found->modified==info.lastModified() &&
            found->changed==info.metadataChangeTime() && found->created==info.birthTime()) return found->image;
    }
    QByteArray bytes;
    const auto image=read(path,bytes)?identify(bytes):nullptr;
    {
        QMutexLocker lock(&mutex);
        if (cache.size()>=256) cache.clear();
        cache.insert(path,{info.size(),info.lastModified(),info.metadataChangeTime(),info.birthTime(),image});
    }
    return image;
}
void append(QString& error,const QString& detail) {
    if (!error.isEmpty()) error += QLatin1Char('\n');
    error += detail;
}
}

bool SegaLdBios::isPacImage(const QByteArray& bytes) { return identify(bytes) != nullptr; }
QString SegaLdBios::resolve(const QString& dataPath,int region,QString& error) {
    return resolveInDirectory(DataFolders::path(dataPath,QStringLiteral("bios")),region,error);
}
QString SegaLdBios::resolveInDirectory(const QString& directory,int region,QString& error) {
    error.clear();
    const auto expected=imageFor(region);
    if (!expected) { error=Localization::text(QStringLiteral("Неподдерживаемый регион MegaLD.")); return {}; }
    const auto canonical=QDir(directory).filePath(QString::fromLatin1(expected->filename));
    if (recognize(QFileInfo(canonical))==expected) return canonical;
    for (const auto& info : QDir(directory).entryInfoList(QDir::Files|QDir::Readable|QDir::NoDotAndDotDot,QDir::Name)) {
        if (info.absoluteFilePath()!=canonical && recognize(info)==expected) return info.absoluteFilePath();
    }
    error=Localization::text(QStringLiteral("Не найден проверенный BIOS MegaLD %1. Импортируйте BIOS через Настройки → Настройки эмулятора… → BIOS → MegaLD."))
        .arg(QString::fromLatin1(expected->label));
    return {};
}
QString SegaLdBios::status(const QString& dataPath,int region) {
    QString error;
    const auto path=resolve(dataPath,region,error);
    return path.isEmpty()?error:Localization::text(QStringLiteral("BIOS MegaLD установлен: %1")).arg(path);
}
QStringList SegaLdBios::importDirectory(const QString& sourceDirectory,const QString& dataPath,QString& error) {
    error.clear(); QStringList imported;
    const QDir source(sourceDirectory);
    if (!source.exists()) { error=Localization::text(QStringLiteral("Каталог BIOS не найден: %1")).arg(sourceDirectory); return imported; }
    const auto destinationDirectory=DataFolders::path(dataPath,QStringLiteral("bios"));
    bool found=false;
    for (const auto& info : source.entryInfoList(QDir::Files|QDir::Readable|QDir::NoDotAndDotDot,QDir::Name)) {
        if (info.size()!=biosSize) continue;
        QByteArray bytes;
        if (!read(info.absoluteFilePath(),bytes)) continue;
        const auto image=identify(bytes);
        if (!image) continue;
        found=true;
        QString ignored;
        if (!resolveInDirectory(destinationDirectory,image->region,ignored).isEmpty()) continue;
        const auto destination=QDir(destinationDirectory).filePath(QString::fromLatin1(image->filename));
        if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink()) {
            append(error,Localization::text(QStringLiteral("Существующий файл сохранён без замены: %1")).arg(destination)); continue;
        }
        if (!QDir().mkpath(destinationDirectory)) {
            append(error,Localization::text(QStringLiteral("Не удалось создать каталог BIOS: %1")).arg(destinationDirectory)); continue;
        }
        QSaveFile output(destination);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes)!=bytes.size() || !output.commit()) {
            append(error,Localization::text(QStringLiteral("Не удалось импортировать %1: %2")).arg(info.absoluteFilePath(),output.errorString())); continue;
        }
        imported.append(QString::fromLatin1(image->label));
    }
    if (!found) append(error,Localization::text(QStringLiteral("В папке не найдены поддерживаемые BIOS MegaLD: PAC-S1 Japan v1.02 или PAC-S10 USA v1.04.")));
    return imported;
}
