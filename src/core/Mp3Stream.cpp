#include "i18n/Localization.h"
#include "Mp3Stream.h"
#include <QFile>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#define MINIMP3_SCAN_LIMIT (256*1024)
#include "minimp3/minimp3_ex.h"

struct Mp3Stream::State {
    QFile file;
    mp3dec_ex_t decoder{};
    mp3dec_io_t io{};
    std::array<char,44> header{};
    std::array<mp3d_sample_t,4096> pcm{};
    qint64 position = 0, totalBytes = 0, cacheStart = -1, cacheBytes = 0;
    bool opened = false;
    ~State() { mp3dec_ex_close(&decoder); }
};
Mp3Stream::Mp3Stream() : state_(std::make_unique<State>()) {}
Mp3Stream::~Mp3Stream() = default;
bool Mp3Stream::open(const QString& path, QString& error, int expectedRate) {
    error.clear(); state_ = std::make_unique<State>();
    auto& s = *state_;
    s.file.setFileName(path);
    if (!s.file.open(QIODevice::ReadOnly) || s.file.size() <= 0 || s.file.size() > 256LL*1024*1024) {
        error = Localization::text(QStringLiteral("Не удалось открыть MP3-дорожку или она слишком велика: %1")).arg(path); return false;
    }
    s.io.read = [](void* out, size_t count, void* user) -> size_t {
        auto* file = static_cast<QFile*>(user);
        const auto result = file->read(static_cast<char*>(out),qint64(count));
        return result > 0 ? size_t(result) : 0;
    };
    s.io.seek = [](uint64_t offset, void* user) -> int {
        return offset <= uint64_t(std::numeric_limits<qint64>::max()) &&
            static_cast<QFile*>(user)->seek(qint64(offset)) ? 0 : -1;
    };
    s.io.read_data = s.io.seek_data = &s.file;
    const bool decoded = mp3dec_ex_open_cb(&s.decoder,&s.io,MP3D_SEEK_TO_SAMPLE) == 0;
    const int rate = s.decoder.info.hz;
    if (!decoded || s.decoder.info.channels != 2 ||
        (expectedRate ? rate != expectedRate : (rate != 44100 && rate != 48000)) ||
        !s.decoder.samples || s.decoder.samples > 100ULL*60*uint64_t(rate)*2) {
        error = expectedRate == 0
            ? Localization::text(QStringLiteral("MP3-дорожка должна быть корректной записью 44,1 или 48 кГц, стерео, не длиннее 100 минут: %1")).arg(path)
            : Localization::text(QStringLiteral("MP3-дорожка должна быть корректной записью 44,1 кГц, стерео, не длиннее 100 минут: %1")).arg(path);
        return false;
    }
    s.totalBytes = qint64(s.decoder.samples)*2 + 44;
    auto* h = s.header.data();
    std::memcpy(h,"RIFF",4); qToLittleEndian<quint32>(quint32(s.totalBytes-8),h+4);
    std::memcpy(h+8,"WAVEfmt ",8); qToLittleEndian<quint32>(16,h+16);
    qToLittleEndian<quint16>(1,h+20); qToLittleEndian<quint16>(2,h+22);
    qToLittleEndian<quint32>(quint32(rate),h+24); qToLittleEndian<quint32>(quint32(rate*4),h+28);
    qToLittleEndian<quint16>(4,h+32); qToLittleEndian<quint16>(16,h+34);
    std::memcpy(h+36,"data",4); qToLittleEndian<quint32>(quint32(s.totalBytes-44),h+40);
    s.opened = true;
    return true;
}
qint64 Mp3Stream::size() const { return state_->totalBytes; }
int Mp3Stream::sampleRate() const { return state_->opened ? state_->decoder.info.hz : 0; }
qint64 Mp3Stream::pos() const { return state_->position; }
bool Mp3Stream::seek(qint64 position) {
    if (!state_->opened || position < 0 || position > state_->totalBytes) return false;
    // Defer decoder seek until actual PCM is requested. Header probes and
    // seek-to-end during CD mounting do not decode or reposition audio.
    state_->position = position;
    return true;
}
qint64 Mp3Stream::read(char* destination, qint64 bytes) {
    auto& s = *state_;
    if (!s.opened || !destination || bytes < 0) return -1;
    bytes = std::min(bytes,s.totalBytes-s.position);
    qint64 done = 0;
    while (done < bytes) {
        if (s.position < 44) {
            const auto count = std::min(bytes-done,44-s.position);
            std::memcpy(destination+done,s.header.data()+s.position,size_t(count));
            s.position += count; done += count; continue;
        }
        const auto offset = s.position-44;
        if (offset < s.cacheStart || offset >= s.cacheStart+s.cacheBytes) {
            const auto sample = uint64_t(offset/2);
            if (s.decoder.cur_sample != sample && mp3dec_ex_seek(&s.decoder,sample)) return done ? done : -1;
            const auto received = mp3dec_ex_read(&s.decoder,s.pcm.data(),s.pcm.size());
            if (!received) return done ? done : (s.decoder.last_error ? -1 : 0);
#if Q_BYTE_ORDER == Q_BIG_ENDIAN
            for (size_t i=0;i<received;++i) s.pcm[i] = qToLittleEndian(s.pcm[i]);
#endif
            s.cacheStart = qint64(sample)*2; s.cacheBytes = qint64(received)*2;
        }
        const auto count = std::min(bytes-done,s.cacheStart+s.cacheBytes-offset);
        std::memcpy(destination+done,reinterpret_cast<const char*>(s.pcm.data())+(offset-s.cacheStart),size_t(count));
        s.position += count; done += count;
    }
    return done;
}
