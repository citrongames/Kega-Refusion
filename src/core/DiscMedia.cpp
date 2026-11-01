#include "DiscMedia.h"
#include "CoreFileSystem.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace {
bool validPath(const QString& path, QString& error) {
    // The pinned core copies image and CUE paths into 256-byte buffers.
    if (QDir::toNativeSeparators(path).toUtf8().size() >= 256) {
        error = QStringLiteral("Слишком длинный путь к образу или дорожке (предел ядра — 255 байт): %1").arg(path);
        return false;
    }
    if (!QFileInfo(path).isFile()) {
        error = QStringLiteral("Не найден образ или дорожка: %1").arg(path); return false;
    }
    return true;
}
QString resolved(const QString& directory, QString path) {
    path.replace('\\','/');
    return QDir::cleanPath(QFileInfo(QDir(directory).filePath(path)).absoluteFilePath());
}
bool hashFile(const QString& path, QCryptographicHash& hash, QString& error) {
    if (!validPath(path,error)) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 8LL*1024*1024*1024 || !hash.addData(&file)) {
        error = QStringLiteral("Не удалось прочитать образ или дорожку: %1").arg(path); return false;
    }
    return true;
}
int headerRegion(const QByteArray& header) {
    int start = header.startsWith("SEGADISCSYSTEM") ? 0 : header.mid(16,14) == "SEGADISCSYSTEM" ? 16 : -1;
    if (start < 0 || header.size() <= start+0x20b) return 0;
    const auto value = quint8(header[start+0x20b]);
    return value == 0x64 ? 3 : value == 0xa1 ? 1 : 2;
}
}
bool DiscMedia::isDisc(const QString& path) {
    return QStringList{"cue","iso","chd","m3u"}.contains(QFileInfo(path).suffix().toLower());
}
bool DiscMedia::inspect(const QString& path, DiscImage& image, QString& error) {
    error.clear();
    const QFileInfo info(path);
    const QString absolute = info.absoluteFilePath();
    const auto extension = info.suffix().toLower();
    if (!QStringList{"cue","iso","chd"}.contains(extension)) {
        error = QStringLiteral("Выберите образ Sega CD: CUE, ISO или CHD."); return false;
    }
    if (!validPath(absolute,error)) return false;
    if (extension == "iso") {
        const auto sidecar = info.absolutePath() + '/' + info.completeBaseName() + ".cue";
        // GPX also opens an ISO's CUE sidecar. Include its complete track set
        // in identity/validation so changed audio tracks cannot reuse a state.
        if (QFileInfo(sidecar).isFile()) return inspect(sidecar,image,error);
    }
    QFile source(absolute);
    if (!source.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Не удалось открыть образ: %1").arg(absolute); return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray("KegaRefusion-CD-v1:"));
    int region = 0;
    QVector<DiscImage::File> files{{absolute,info.size(),info.lastModified()}};
    QVector<DiscImage::Track> aliases;
    QByteArray virtualCue;
    bool hasMp3 = false;
    if (extension == "cue") {
        if (source.size() > 65536) { error = QStringLiteral("Файл CUE слишком велик."); return false; }
        const QString cue = QString::fromUtf8(source.readAll());
        static const QRegularExpression fileLine(QStringLiteral(R"cue(^\s*FILE\s+(?:"([^"]+)"|(\S+))\s+(\S+)\s*$)cue"), QRegularExpression::CaseInsensitiveOption);
        QStringList tracks;
        QByteArray layout;
        for (const auto& line : cue.split('\n')) {
            const auto match = fileLine.match(line.trimmed());
            if (match.hasMatch()) {
                const auto type = match.captured(3).toUpper();
                if (!QStringList{"BINARY","MOTOROLA","WAVE","OGG","MP3"}.contains(type)) {
                    error = QStringLiteral("Дорожки %1 в CUE не поддерживаются. Поддерживаются BIN, WAV, OGG и MP3.").arg(type);
                    return false;
                }
                if (line.toUtf8().size() >= 128) {
                    error = QStringLiteral("Слишком длинная строка FILE в CUE (предел ядра — 127 байт)."); return false;
                }
                const auto name = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
                if (QDir::isAbsolutePath(name) || name.contains("://")) {
                    error = QStringLiteral("В CUE нужны относительные пути к дорожкам: %1").arg(name); return false;
                }
                tracks.append(resolved(info.absolutePath(),name));
                const bool mp3 = type == "MP3";
                hasMp3 |= mp3;
                const QString alias = QStringLiteral("track%1.%2").arg(tracks.size(),2,10,QChar('0')).arg(mp3 ? "wav" : QFileInfo(name).suffix());
                aliases.append({alias,tracks.last(),mp3});
                virtualCue += "FILE \"" + alias.toUtf8() + "\" " + (mp3 ? QByteArray("WAVE") : type.toUtf8()) + '\n';
                layout += "FILE #" + QByteArray::number(tracks.size()) + " " + match.captured(3).toUpper().toUtf8() + '\n';
            } else {
                layout += line.trimmed().toUtf8() + '\n';
                virtualCue += line.trimmed().toUtf8() + '\n';
            }
        }
        if (tracks.isEmpty() || tracks.size() > 99 || !cue.contains(QRegularExpression(QStringLiteral("TRACK\\s+0?1\\s+MODE[12]/"),QRegularExpression::CaseInsensitiveOption))) {
            error = QStringLiteral("CUE не содержит поддерживаемую первую дорожку данных Sega CD."); return false;
        }
        hash.addData(layout);
        for (const auto& track : tracks) {
            QCryptographicHash trackHash(QCryptographicHash::Sha256);
            if (!hashFile(track,trackHash,error)) return false;
            hash.addData(trackHash.result());
            const QFileInfo trackInfo(track);
            files.append({track,trackInfo.size(),trackInfo.lastModified()});
        }
        QFile first(tracks.first());
        if (first.open(QIODevice::ReadOnly)) region = headerRegion(first.read(0x220));
    } else {
        const auto header = source.read(0x220);
        if (extension == "chd" ? !header.startsWith("MComprHD") : !(region = headerRegion(header))) {
            error = QStringLiteral("Файл не похож на образ Sega CD: %1").arg(info.fileName()); return false;
        }
        QCryptographicHash imageHash(QCryptographicHash::Sha256);
        if (!hashFile(absolute,imageHash,error)) return false;
        hash.addData(extension.toUtf8()); hash.addData(imageHash.result());
    }
    image = {};
    image.path = absolute; image.hash = hash.result(); image.region = region;
    image.files = files;
    if (hasMp3) { image.virtualCue = virtualCue; image.tracks = aliases; }
    return true;
}
bool DiscMedia::unchanged(const DiscImage& image, QString& error) {
    for (const auto& expected : image.files) {
        const QFileInfo current(expected.path);
        if (!current.isFile() || current.size() != expected.size || current.lastModified() != expected.modified) {
            error = QStringLiteral("Образ или дорожка изменились. Откройте игру заново: %1").arg(expected.path);
            return false;
        }
    }
    return true;
}
bool DiscMedia::prepare(DiscImage& image, const QString& dataPath, QString& error) {
    image.runtimePath = image.path;
    if (image.virtualCue.isEmpty()) return true;
    const QString root = QDir(dataPath).absoluteFilePath("cd/" + QString::fromLatin1(image.hash.toHex().left(16)));
    image.runtimePath = root + "/game.cue";
    if (QDir::toNativeSeparators(image.runtimePath).toUtf8().size() >= 240) {
        error = QStringLiteral("Слишком длинный путь к каталогу данных для образа с MP3."); return false;
    }
    // Only the tiny CUE and path aliases live in memory. No generated image,
    // WAV file, ISO copy or full decoded audio is stored anywhere.
    CoreFileSystem::addMemoryFile(image.runtimePath,image.virtualCue);
    for (const auto& track : image.tracks)
        CoreFileSystem::addAlias(root + '/' + track.alias,track.source,track.mp3);
    return true;
}
bool DiscMedia::read(const QString& path, QVector<DiscImage>& images, QString& error) {
    images.clear(); error.clear();
    if (QFileInfo(path).suffix().compare("m3u",Qt::CaseInsensitive) != 0) {
        DiscImage image;
        if (!inspect(path,image,error)) return false;
        images.append(image); return true;
    }
    QFile playlist(path);
    if (!playlist.open(QIODevice::ReadOnly) || playlist.size() > 65536) {
        error = QStringLiteral("Не удалось прочитать список дисков M3U."); return false;
    }
    QString text = QString::fromUtf8(playlist.readAll());
    if (text.startsWith(QChar(0xfeff))) text.remove(0,1);
    for (QString line : text.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('"') && line.endsWith('"')) line = line.mid(1,line.size()-2);
        if (images.size() >= 4) { error = QStringLiteral("Ядро поддерживает до четырёх дисков в M3U."); return false; }
        DiscImage image;
        if (!inspect(resolved(QFileInfo(path).absolutePath(),line),image,error)) return false;
        images.append(image);
    }
    if (images.isEmpty()) { error = QStringLiteral("Список дисков M3U пуст."); return false; }
    return true;
}
