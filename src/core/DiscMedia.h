#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <QDateTime>

struct DiscImage {
    QString path;
    QByteArray hash;
    struct File { QString path; qint64 size; QDateTime modified; };
    struct Track { QString alias, source; bool mp3; };
    QVector<File> files;
    QVector<Track> tracks;
    QByteArray virtualCue;
    QString runtimePath;
    int region = 0; // 1 Japan, 2 USA, 3 Europe; 0 requires core detection (CHD).
    bool needs32x = false; // Boot header says SEGA 32X; CHD remains core-detected.
};
class DiscMedia final {
public:
    static bool isDisc(const QString& path);
    static bool read(const QString& path, QVector<DiscImage>& images, QString& error);
    static bool unchanged(const DiscImage& image, QString& error);
    static bool prepare(DiscImage& image, const QString& dataPath, QString& error);
    static bool inspect(const QString& path, DiscImage& image, QString& error);
};
