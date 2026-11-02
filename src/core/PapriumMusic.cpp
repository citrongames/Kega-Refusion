#include "PapriumMusic.h"

#include "Mp3Stream.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QtEndian>

#include <algorithm>
#include <utility>

namespace {
struct TrackName {
    unsigned id;
    const char* baseName;
};

// The command-to-filename mapping used by the Paprium cartridge.
constexpr TrackName kTrackNames[] = {
    {0x01, "02 90's Acid Dub Character Select"}, {0x02, "08 90's Dance"},
    {0x03, "42 1988 Commercial"}, {0x04, "05 Asian Chill"},
    {0x05, "31 Bad Dudes vs Paprium"}, {0x06, "43 Blade FM"},
    {0x07, "03 Bone Crusher"}, {0x0b, "26 Club Shuffle"},
    {0x0c, "23 Continue"}, {0x0e, "07 Cool Groove"},
    {0x0f, "36 Cyberpunk Ninja"}, {0x10, "35 Cyberpunk Funk"},
    {0x11, "30 Cyber Interlude"}, {0x12, "21 Cyborg Invasion"},
    {0x13, "44 Dark Alley"}, {0x14, "29 Dark & Power Mad"},
    {0x15, "24 Intro"}, {0x16, "27 Dark Rock"},
    {0x17, "04 Drumbass Boss"}, {0x18, "45 Dubstep Groove"},
    {0x19, "15 Electro Acid Funk"}, {0x1b, "28 Evolve"},
    {0x1c, "33 Funk Enhanced Mix"}, {0x1d, "41 Game Over"},
    {0x1e, "46 Gothic"}, {0x20, "13 Hard Rock"},
    {0x21, "22 Hardcore BP1"}, {0x22, "11 Hardcore BP2"},
    {0x23, "38 Hardcore BP3"}, {0x24, "40 Score"},
    {0x25, "47 House"}, {0x26, "17 Indie Shuffle"},
    {0x27, "25 Indie Break Beat"}, {0x28, "16 Jazzy Shuffle"},
    {0x2a, "19 Neo Metal"}, {0x2b, "14 Neon Rider"},
    {0x2e, "09 Retro Beat"}, {0x2f, "20 Sadness"},
    {0x31, "18 Slow Asian Beat"}, {0x32, "48 Slow Mood"},
    {0x33, "49 Smooth Coords"}, {0x34, "10 Spiral"},
    {0x35, "12 Stage Clear"}, {0x36, "32 Summer Breeze"},
    {0x37, "06 Techno Beats"}, {0x38, "50 Tension"},
    {0x39, "01 Theme of Paprium"}, {0x3a, "39 Ending"},
    {0x3b, "34 Transe"}, {0x3c, "37 Urban"},
    {0x3d, "51 Water"}, {0x3e, "52 Waterfront Beat"},
};

constexpr int kMaxDirectoryEntries = 128;

QString papriumDirectory(const QDir& romDirectory) {
    const QString exact = romDirectory.filePath(QStringLiteral("paprium"));
    if (QFileInfo(exact).isDir()) return exact;

    // Case-sensitive hosts can still find a differently capitalized folder.
    QDirIterator entries(romDirectory.absolutePath(),
                         QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (int examined = 0; examined < kMaxDirectoryEntries && entries.hasNext(); ++examined) {
        entries.next();
        const QFileInfo info = entries.fileInfo();
        if (info.isDir() &&
            info.fileName().compare(QStringLiteral("paprium"), Qt::CaseInsensitive) == 0)
            return info.absoluteFilePath();
    }
    return {};
}

void collectTracks(const QString& directory, std::array<QString, 0x3f>& paths) {
    if (directory.isEmpty()) return;
    QDirIterator entries(directory, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (int examined = 0; examined < kMaxDirectoryEntries && entries.hasNext(); ++examined) {
        entries.next();
        const QFileInfo info = entries.fileInfo();
        if (!info.isFile() ||
            info.suffix().compare(QStringLiteral("mp3"), Qt::CaseInsensitive) != 0)
            continue;
        const QString fileName = info.fileName();
        for (const auto& name : kTrackNames) {
            if (!paths[name.id].isEmpty()) continue;
            const QString expected = QString::fromLatin1(name.baseName) + QStringLiteral(".mp3");
            if (fileName.compare(expected, Qt::CaseInsensitive) == 0) {
                paths[name.id] = info.absoluteFilePath();
                break;
            }
        }
    }
}
} // namespace

PapriumMusic::PapriumMusic() = default;
PapriumMusic::~PapriumMusic() = default;

void PapriumMusic::configure(const QString& romPath) {
    clear();
    if (romPath.isEmpty()) return;
    const QDir romDirectory(QFileInfo(romPath).absolutePath());
    collectTracks(papriumDirectory(romDirectory), paths_);
    collectTracks(romDirectory.absolutePath(), paths_);
}

void PapriumMusic::clear() {
    stream_.reset();
    active_ = 0;
    paths_.fill({});
    lengths_.fill(0);
    rates_.fill(0);
    lengthKnown_.fill(false);
    failed_.fill(false);
    error_.clear();
}

bool PapriumMusic::available(unsigned track) const {
    return track < paths_.size() && !paths_[track].isEmpty() && !failed_[track];
}

bool PapriumMusic::activate(unsigned track) {
    if (!available(track)) return false;
    if (active_ == track && stream_) return true;

    // Drop the previous compressed-frame index before opening another track.
    stream_.reset();
    active_ = 0;
    auto next = std::make_unique<Mp3Stream>();
    QString detail;
    if (!next->open(paths_[track], detail, 0)) {
        failed_[track] = true;
        error_ = std::move(detail);
        return false;
    }

    lengths_[track] = std::uint64_t((next->size() - 44) / 4);
    rates_[track] = next->sampleRate();
    lengthKnown_[track] = true;
    stream_ = std::move(next);
    active_ = track;
    return true;
}

std::uint64_t PapriumMusic::frames(unsigned track) {
    if (!available(track)) return 0;
    if (!lengthKnown_[track] && !activate(track)) return 0;
    return lengths_[track];
}

int PapriumMusic::sampleRate(unsigned track) {
    if (!available(track)) return 0;
    if (!lengthKnown_[track] && !activate(track)) return 0;
    return rates_[track];
}

std::size_t PapriumMusic::read(unsigned track, std::uint64_t frame,
                               std::int16_t* interleaved, std::size_t requested) {
    if (!interleaved || !requested || !activate(track)) return 0;
    const std::uint64_t length = lengths_[track];
    if (frame >= length) return 0;
    const std::size_t count = std::min(requested, std::size_t(length - frame));
    const qint64 bytePosition = 44 + qint64(frame * 4);
    if (stream_->pos() != bytePosition && !stream_->seek(bytePosition)) return 0;

    const qint64 received = stream_->read(reinterpret_cast<char*>(interleaved),
                                          qint64(count * 4));
    if (received <= 0) {
        error_ = QStringLiteral("Failed to decode Paprium MP3 track: %1").arg(paths_[track]);
        failed_[track] = true;
        stream_.reset();
        active_ = 0;
        return 0;
    }
    const std::size_t decoded = std::size_t(received / 4);
#if Q_BYTE_ORDER == Q_BIG_ENDIAN
    const auto* littleEndian = reinterpret_cast<const uchar*>(interleaved);
    for (std::size_t sample = 0; sample < decoded * 2; ++sample)
        interleaved[sample] = qFromLittleEndian<qint16>(littleEndian + sample * 2);
#endif
    return decoded;
}
