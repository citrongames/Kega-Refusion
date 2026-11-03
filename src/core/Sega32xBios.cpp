#include "i18n/Localization.h"
#include "Sega32xBios.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <array>
#include <algorithm>
#include <utility>

namespace {
struct Image {
    Sega32xBios::Piece piece;
    qsizetype size;
    const char* name;
    const char* label;
    const char* sha256;
};
// Fingerprints of the user's read-only original Fusion images. These tiny boot
// ROMs have no common MD header/checksum. Accept known contents, never size or
// filename alone; an unrecognized revision remains unsupported, not overwritten.
constexpr std::array<Image,3> images{{
    {Sega32xBios::Piece::M68k,256,"32X_G_BIOS.BIN","68000",
     "a2d6b6e1544467e27d404634de6e6b0a54ed4b3babb6b71739a260e1be04ebfc"},
    {Sega32xBios::Piece::MasterSh2,2048,"32X_M_BIOS.BIN","Master SH2",
     "4c6128229bd24dae5f11d3f569ca592665f141bef26cef937e2ce4d9b548cc3b"},
    {Sega32xBios::Piece::SlaveSh2,1024,"32X_S_BIOS.BIN","Slave SH2",
     "7bde4245ce00c51a23a1ce73a881752e7b14dc12416e175352a3807338e04745"},
}};
const Image* imageFor(Sega32xBios::Piece piece) {
    for (const auto& image:images) if (image.piece==piece) return &image;
    return nullptr;
}
QString pathFor(const QString& dataPath,const Image& image) {
    return QDir(dataPath).filePath(QStringLiteral("bios/")+QString::fromLatin1(image.name));
}
bool readImage(const QString& path, qsizetype size, QByteArray& bytes, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error=Localization::text(QStringLiteral("Не удалось прочитать BIOS %1: %2")).arg(path,file.errorString());
        return false;
    }
    if (file.size()!=size) {
        error=Localization::text(QStringLiteral("Неверный размер BIOS %1: требуется %2 байт.")).arg(path).arg(size);
        return false;
    }
    bytes=file.read(size+1);
    if (bytes.size()!=size) {
        error=Localization::text(QStringLiteral("Не удалось полностью прочитать BIOS: %1")).arg(path);
        return false;
    }
    return true;
}
const Image* identify(const QByteArray& bytes) {
    const auto hash=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
    for (const auto& image:images)
        if (bytes.size()==image.size && hash==image.sha256) return &image;
    return nullptr;
}
void append(QString& error,const QString& detail) {
    if (!error.isEmpty()) error+=QLatin1Char('\n');
    error+=detail;
}
struct Recognition {
    qint64 size;
    QDateTime modified;
    QDateTime metadataChanged;
    QDateTime created;
    const Image* image;
};

// Metadata-bound cache; no BIOS bytes retained after recognition.
QMutex recognitionMutex;
QHash<QString, Recognition> recognitionCache;
constexpr int recognitionCacheLimit=256;

const Image* recognizeFile(const QFileInfo& info) {
    if (!info.isFile() || !info.isReadable() ||
        (info.size()!=256 && info.size()!=1024 && info.size()!=2048)) return nullptr;
    const QString path=info.absoluteFilePath();
    {
        QMutexLocker lock(&recognitionMutex);
        const auto cached=recognitionCache.constFind(path);
        if (cached!=recognitionCache.cend() && cached->size==info.size() &&
            cached->modified==info.lastModified() &&
            cached->metadataChanged==info.metadataChangeTime() &&
            cached->created==info.birthTime())
            return cached->image;
    }
    QByteArray bytes;
    QString error;
    if (!readImage(path,info.size(),bytes,error)) return nullptr;
    const Image* image=identify(bytes);
    {
        QMutexLocker lock(&recognitionMutex);
        if (recognitionCache.size()>=recognitionCacheLimit) recognitionCache.clear();
        recognitionCache.insert(path,{info.size(),info.lastModified(),info.metadataChangeTime(),
                                      info.birthTime(),image});
    }
    return image;
}
}
QString Sega32xBios::fileName(Piece piece) {
    const auto* image=imageFor(piece);
    return image ? QString::fromLatin1(image->name):QString();
}
bool Sega32xBios::validateFile(const QString& path,Piece piece,QString& error) {
    error.clear();
    const auto* image=imageFor(piece);
    if (!image) { error=Localization::text(QStringLiteral("Неизвестная часть BIOS 32X.")); return false; }
    QByteArray bytes;
    if (!readImage(path,image->size,bytes,error)) return false;
    if (identify(bytes)!=image) {
        error=Localization::text(QStringLiteral("BIOS %1 не распознан: контрольная сумма не соответствует поддерживаемому образу %2."))
                  .arg(path,QString::fromLatin1(image->label));
        return false;
    }
    return true;
}
QString Sega32xBios::resolve(const QString& dataPath,Piece piece,QString& error) {
    error.clear();
    const Image* expected=imageFor(piece);
    if (!expected) {
        error=Localization::text(QStringLiteral("Неизвестная часть BIOS 32X."));
        return {};
    }
    const QString canonical=pathFor(dataPath,*expected);
    const QFileInfo canonicalInfo(canonical);
    if (canonicalInfo.exists() && recognizeFile(canonicalInfo)==expected) return canonical;
    QDir directory(QDir(dataPath).filePath(QStringLiteral("bios")));
    auto entries=directory.entryInfoList(QDir::Files|QDir::Readable|QDir::NoDotAndDotDot,QDir::NoSort);
    std::sort(entries.begin(),entries.end(),[](const QFileInfo& a,const QFileInfo& b) {
        return a.fileName()<b.fileName();
    });
    for (const QFileInfo& info:std::as_const(entries)) {
        if (info.absoluteFilePath()==canonical || info.size()!=expected->size) continue;
        if (recognizeFile(info)==expected) return info.absoluteFilePath();
    }
    if (canonicalInfo.exists()) {
        error=Localization::text(QStringLiteral("BIOS %1 не распознан: контрольная сумма не соответствует поддерживаемому образу %2."))
                  .arg(canonical,QString::fromLatin1(expected->label));
    } else {
        error=Localization::text(QStringLiteral("Не найден BIOS 32X: %1.")).arg(canonical);
    }
    return {};
}
bool Sega32xBios::validate(const QString& dataPath,QString& error) {
    error.clear();
    for (const auto& image:images) {
        QString detail;
        if (resolve(dataPath,image.piece,detail).isEmpty()) append(error,detail);
    }
    return error.isEmpty();
}
QString Sega32xBios::status(const QString& dataPath) {
    QString error;
    if (!validate(dataPath,error)) return error;
    return Localization::text(QStringLiteral("BIOS 32X установлены: 68000, Master SH2, Slave SH2."));
}
QStringList Sega32xBios::importDirectory(const QString& sourceDirectory,const QString& dataPath,QString& error) {
    error.clear();
    QStringList imported;
    QDir source(sourceDirectory);
    if (!source.exists()) {
        error=Localization::text(QStringLiteral("Каталог BIOS не найден: %1")).arg(sourceDirectory);
        return imported;
    }
    std::array<QByteArray,3> candidates;
    for (const auto& entry:source.entryInfoList(QDir::Files|QDir::Readable|QDir::NoDotAndDotDot,QDir::Name)) {
        if (entry.size()!=256 && entry.size()!=1024 && entry.size()!=2048) continue;
        QByteArray bytes; QString detail;
        if (!readImage(entry.absoluteFilePath(),entry.size(),bytes,detail)) continue;
        if (const auto* image=identify(bytes)) {
            const auto index=static_cast<std::size_t>(image-images.data());
            if (candidates[index].isEmpty()) candidates[index]=bytes;
        }
    }
    bool found=false;
    for (std::size_t i=0;i<images.size();++i) {
        if (candidates[i].isEmpty()) continue;
        found=true;
        const auto& image=images[i];
        const QString destination=pathFor(dataPath,image);
        QString detail;
        if (!resolve(dataPath,image.piece,detail).isEmpty()) continue;
        const QFileInfo existing(destination);
        if (existing.exists() || existing.isSymLink()) {
            append(error,Localization::text(QStringLiteral("Существующий файл сохранён без замены. %1")).arg(detail));
            continue;
        }
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
            append(error,Localization::text(QStringLiteral("Не удалось создать каталог BIOS: %1")).arg(destination));
            continue;
        }
        // NewOnly closes the existence-check race: user files are never replaced.
        QFile output(destination);
        if (!output.open(QIODevice::WriteOnly|QIODevice::NewOnly)) {
            append(error,Localization::text(QStringLiteral("Не удалось создать BIOS %1: %2")).arg(destination,output.errorString()));
            continue;
        }
        if (output.write(candidates[i])!=candidates[i].size() || !output.flush()) {
            const QString detail=output.errorString();
            output.close(); output.remove();
            append(error,Localization::text(QStringLiteral("Не удалось записать BIOS %1: %2")).arg(destination,detail));
            continue;
        }
        output.close();
        imported<<QString::fromLatin1(image.label);
    }
    if (!found) append(error,Localization::text(QStringLiteral("В каталоге %1 не найдены поддерживаемые оригинальные BIOS 32X (256, 2048 и 1024 байта)."))
                              .arg(source.absolutePath()));
    return imported;
}
