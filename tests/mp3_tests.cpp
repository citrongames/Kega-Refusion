#include "core/Mp3Stream.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDebug>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace {
struct Reference {
    qint64 offset;
    QByteArray bytes;
    qint64 filled = 0;
};

quint16 le16(const char* bytes) {
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(bytes));
}
quint32 le32(const char* bytes) {
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(bytes));
}

bool fail(const QString& message) {
    qCritical().noquote() << message;
    return false;
}

int failMain(const QString& message) {
    fail(message);
    return 1;
}

bool checkAt(Mp3Stream& stream, const Reference& reference, std::array<char, 4096>& scratch) {
    if (!stream.seek(reference.offset) || stream.pos() != reference.offset)
        return fail(QStringLiteral("Seek failed at byte %1").arg(reference.offset));
    const qint64 length = reference.bytes.size();
    if (length > static_cast<qint64>(scratch.size()))
        return fail(QStringLiteral("Test reference exceeds scratch buffer"));
    const qint64 received = stream.read(scratch.data(), length);
    if (received != length || stream.pos() != reference.offset + length)
        return fail(QStringLiteral("Short random read at byte %1: %2 of %3")
                        .arg(reference.offset).arg(received).arg(length));
    if (std::memcmp(scratch.data(), reference.bytes.constData(), size_t(length)) != 0)
        return fail(QStringLiteral("PCM differs after seek at byte %1").arg(reference.offset));
    return true;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        qCritical().noquote() << "Usage: mp3_tests <track.mp3>";
        return 2;
    }

    Mp3Stream stream;
    QString error;
    QElapsedTimer timer;
    timer.start();
    if (!stream.open(QString::fromLocal8Bit(argv[1]), error))
        return failMain(QStringLiteral("Open failed: %1").arg(error));
    const qint64 openMs = timer.elapsed();
    const qint64 totalBytes = stream.size();
    if (totalBytes <= 44 || totalBytes - 44 > 0xffffffffLL || (totalBytes - 44) % 4)
        return failMain(QStringLiteral("Invalid virtual WAV size: %1").arg(totalBytes));

    std::array<char, 44> header{};
    if (stream.read(header.data(), header.size()) != static_cast<qint64>(header.size()))
        return failMain(QStringLiteral("Could not read WAV header"));
    if (std::memcmp(header.data(), "RIFF", 4) ||
        std::memcmp(header.data() + 8, "WAVEfmt ", 8) ||
        std::memcmp(header.data() + 36, "data", 4) ||
        le32(header.data() + 4) != totalBytes - 8 ||
        le32(header.data() + 16) != 16 ||
        le16(header.data() + 20) != 1 || le16(header.data() + 22) != 2 ||
        le32(header.data() + 24) != 44100 || le32(header.data() + 28) != 176400 ||
        le16(header.data() + 32) != 4 || le16(header.data() + 34) != 16 ||
        le32(header.data() + 40) != totalBytes - 44)
        return failMain(QStringLiteral("Invalid 44.1 kHz stereo 16-bit PCM WAV header"));

    const qint64 audioBytes = totalBytes - 44;
    std::vector<Reference> references;
    auto addReference = [&](qint64 offset) {
        offset = std::clamp<qint64>(offset, 0, totalBytes - 1);
        references.push_back({offset, QByteArray(int(std::min<qint64>(4096, totalBytes - offset)), '\0')});
    };
    addReference(0);          // Header and first PCM bytes.
    addReference(40);         // A read crossing the 44-byte header boundary.
    addReference(45);         // Odd byte within an interleaved 16-bit sample.
    addReference(65535);      // Another unaligned byte after decoder block changes.
    addReference(44 + audioBytes / 5 + 1);
    addReference(44 + audioBytes / 2 + 3);
    addReference(44 + audioBytes * 4 / 5 + 7);
    addReference(totalBytes - 4096);

    if (!stream.seek(0)) return failMain(QStringLiteral("Could not rewind to WAV start"));
    std::array<char, 2352> block{};
    qint64 cursor = 0;
    timer.restart();
    while (cursor < totalBytes) {
        const qint64 received = stream.read(block.data(), block.size());
        if (received <= 0 || received > static_cast<qint64>(block.size()) ||
            stream.pos() != cursor + received)
            return failMain(QStringLiteral("Sequential read stopped at %1/%2 bytes (read %3)")
                                .arg(cursor).arg(totalBytes).arg(received));
        for (Reference& reference : references) {
            const qint64 begin = std::max(cursor, reference.offset);
            const qint64 end = std::min(cursor + received,
                                        reference.offset + reference.bytes.size());
            if (begin < end) {
                std::memcpy(reference.bytes.data() + (begin - reference.offset),
                            block.data() + (begin - cursor), size_t(end - begin));
                reference.filled += end - begin;
            }
        }
        cursor += received;
    }
    const qint64 sequentialMs = timer.elapsed();
    if (stream.read(block.data(), block.size()) != 0 || stream.pos() != totalBytes)
        return failMain(QStringLiteral("Read past virtual WAV end did not return EOF"));
    for (const Reference& reference : references)
        if (reference.filled != reference.bytes.size())
            return failMain(QStringLiteral("Reference capture incomplete at %1").arg(reference.offset));

    std::array<char, 4096> scratch{};
    for (int repeat = 0; repeat < 2; ++repeat) {
        for (auto it = references.rbegin(); it != references.rend(); ++it)
            if (!checkAt(stream, *it, scratch)) return 1;
    }
    if (!stream.seek(totalBytes) || stream.read(scratch.data(), scratch.size()) != 0 ||
        stream.pos() != totalBytes || stream.seek(-1) || stream.seek(totalBytes + 1))
        return failMain(QStringLiteral("EOF or out-of-range seek behavior is invalid"));
    const Reference& last = references.back();
    if (!stream.seek(totalBytes - 1) || stream.read(scratch.data(), 1) != 1 ||
        scratch[0] != last.bytes.back() || stream.read(scratch.data(), 1) != 0)
        return failMain(QStringLiteral("Last byte or repeated EOF read differs"));

    qint64 referenceBytes = 0;
    for (const Reference& reference : references) referenceBytes += reference.bytes.size();
    const qint64 testBufferBytes = referenceBytes + block.size() + scratch.size() + header.size();
    if (testBufferBytes > 256 * 1024)
        return failMain(QStringLiteral("Test retained too many audio bytes: %1").arg(testBufferBytes));
    const double audioSeconds = double(audioBytes) / 176400.0;
    const double realtimeMultiple = sequentialMs > 0 ? audioSeconds * 1000.0 / sequentialMs : 0.0;
    qInfo().noquote() << "MP3 virtual WAV OK:"
                      << "audio bytes" << audioBytes
                      << "duration s" << audioSeconds
                      << "open ms" << openMs
                      << "sequential decode ms" << sequentialMs
                      << "realtime multiple" << realtimeMultiple
                      << "reference bytes" << referenceBytes
                      << "peak test buffer bytes" << testBufferBytes;
    return 0;
}
