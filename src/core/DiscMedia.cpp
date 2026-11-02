#include "i18n/Localization.h"
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
        error = Localization::text(QStringLiteral("Слишком длинный путь к образу или дорожке (предел ядра — 255 байт): %1")).arg(path);
        return false;
    }
    if (!QFileInfo(path).isFile()) {
        error = Localization::text(QStringLiteral("Не найден образ или дорожка: %1")).arg(path); return false;
    }
    return true;
}
QString resolved(const QString& directory, QString path) {
    path.replace('\\','/');
    return QDir::cleanPath(QFileInfo(QDir(directory).filePath(path)).absoluteFilePath());
}
bool readableFile(const QString& path, QString& error) {
    if (!validPath(path,error)) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 8LL*1024*1024*1024) {
        error = Localization::text(QStringLiteral("Не удалось прочитать образ или дорожку: %1")).arg(path); return false;
    }
    return true;
}
int headerStart(const QByteArray& header) {
    return header.startsWith("SEGADISCSYSTEM") ? 0 : header.mid(16,14) == "SEGADISCSYSTEM" ? 16 : -1;
}
bool headerNeeds32x(const QByteArray& header) {
    const int start = headerStart(header);
    return start >= 0 && header.mid(start + 0x100, 8) == "SEGA 32X";
}
int headerRegion(const QByteArray& header) {
    const int start = headerStart(header);
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
        error = Localization::text(QStringLiteral("Выберите образ Sega CD: CUE, ISO или CHD.")); return false;
    }
    if (!validPath(absolute,error)) return false;
    if (extension == "iso") {
        const auto sidecar = info.absolutePath() + '/' + info.completeBaseName() + ".cue";
        // GPX also opens an ISO's CUE sidecar. Validate its complete track set
        // without scanning the contents of the disc.
        if (QFileInfo(sidecar).isFile()) return inspect(sidecar,image,error);
    }
    QFile source(absolute);
    if (!source.open(QIODevice::ReadOnly)) {
        error = Localization::text(QStringLiteral("Не удалось открыть образ: %1")).arg(absolute); return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // Private in-memory MP3 alias namespace only, not save-file identity.
    // Hashing the path is constant work; disc contents are streamed by GPX.
    hash.addData(QByteArray("KegaRefusion-CD-path-v1:"));
    hash.addData(absolute.toUtf8());
    int region = 0;
    bool needs32x = false;
    QVector<DiscImage::File> files{{absolute,info.size(),info.lastModified()}};
    QVector<DiscImage::Track> aliases;
    QByteArray virtualCue;
    bool hasMp3 = false;
    if (extension == "cue") {
        if (source.size() > 65536) { error = Localization::text(QStringLiteral("Файл CUE слишком велик.")); return false; }
        const QString cue = QString::fromUtf8(source.readAll());
        static const QRegularExpression fileLine(QStringLiteral(R"cue(^\s*FILE\s+(?:"([^"]+)"|(\S+))\s+(\S+)\s*$)cue"), QRegularExpression::CaseInsensitiveOption);
        QStringList tracks;
        for (const auto& line : cue.split('\n')) {
            const auto match = fileLine.match(line.trimmed());
            if (match.hasMatch()) {
                const auto type = match.captured(3).toUpper();
                if (!QStringList{"BINARY","MOTOROLA","WAVE","OGG","MP3"}.contains(type)) {
                    error = Localization::text(QStringLiteral("Дорожки %1 в CUE не поддерживаются. Поддерживаются BIN, WAV, OGG и MP3.")).arg(type);
                    return false;
                }
                if (line.toUtf8().size() >= 128) {
                    error = Localization::text(QStringLiteral("Слишком длинная строка FILE в CUE (предел ядра — 127 байт).")); return false;
                }
                const auto name = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
                if (QDir::isAbsolutePath(name) || name.contains("://")) {
                    error = Localization::text(QStringLiteral("В CUE нужны относительные пути к дорожкам: %1")).arg(name); return false;
                }
                tracks.append(resolved(info.absolutePath(),name));
                const bool mp3 = type == "MP3";
                hasMp3 |= mp3;
                const QString alias = QStringLiteral("track%1.%2").arg(tracks.size(),2,10,QChar('0')).arg(mp3 ? "wav" : QFileInfo(name).suffix());
                aliases.append({alias,tracks.last(),mp3});
                virtualCue += "FILE \"" + alias.toUtf8() + "\" " + (mp3 ? QByteArray("WAVE") : type.toUtf8()) + '\n';
            } else {
                virtualCue += line.trimmed().toUtf8() + '\n';
            }
        }
        if (tracks.isEmpty() || tracks.size() > 99 || !cue.contains(QRegularExpression(QStringLiteral("TRACK\\s+0?1\\s+MODE[12]/"),QRegularExpression::CaseInsensitiveOption))) {
            error = Localization::text(QStringLiteral("CUE не содержит поддерживаемую первую дорожку данных Sega CD.")); return false;
        }
        for (const auto& track : tracks) {
            if (!readableFile(track,error)) return false;
            const QFileInfo trackInfo(track);
            files.append({track,trackInfo.size(),trackInfo.lastModified()});
        }
        QFile first(tracks.first());
        if (first.open(QIODevice::ReadOnly)) {
            const auto header = first.read(0x220);
            region = headerRegion(header);
            needs32x = headerNeeds32x(header);
        }
    } else {
        const auto header = source.read(0x220);
        if (extension == "chd" ? !header.startsWith("MComprHD") : !(region = headerRegion(header))) {
            error = Localization::text(QStringLiteral("Файл не похож на образ Sega CD: %1")).arg(info.fileName()); return false;
        }
        if (extension != "chd") needs32x = headerNeeds32x(header);
        if (source.size() <= 0 || source.size() > 8LL*1024*1024*1024) {
            error = Localization::text(QStringLiteral("Некорректный размер образа Sega CD.")); return false;
        }
    }
    image = {};
    image.path = absolute; image.hash = hash.result(); image.region = region; image.needs32x = needs32x;
    image.files = files;
    if (hasMp3) { image.virtualCue = virtualCue; image.tracks = aliases; }
    return true;
}
bool DiscMedia::unchanged(const DiscImage& image, QString& error) {
    for (const auto& expected : image.files) {
        const QFileInfo current(expected.path);
        if (!current.isFile() || current.size() != expected.size || current.lastModified() != expected.modified) {
            error = Localization::text(QStringLiteral("Образ или дорожка изменились. Откройте игру заново: %1")).arg(expected.path);
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
        error = Localization::text(QStringLiteral("Слишком длинный путь к каталогу данных для образа с MP3.")); return false;
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
        error = Localization::text(QStringLiteral("Не удалось прочитать список дисков M3U.")); return false;
    }
    QString text = QString::fromUtf8(playlist.readAll());
    if (text.startsWith(QChar(0xfeff))) text.remove(0,1);
    for (QString line : text.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('"') && line.endsWith('"')) line = line.mid(1,line.size()-2);
        if (images.size() >= 4) { error = Localization::text(QStringLiteral("Ядро поддерживает до четырёх дисков в M3U.")); return false; }
        DiscImage image;
        if (!inspect(resolved(QFileInfo(path).absolutePath(),line),image,error)) return false;
        images.append(image);
    }
    if (images.isEmpty()) { error = Localization::text(QStringLiteral("Список дисков M3U пуст.")); return false; }
    return true;
}
