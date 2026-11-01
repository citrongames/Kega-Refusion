#include "RewindBuffer.h"
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr qsizetype maxBytes = 64 * 1024 * 1024;
QByteArray unpack(const QByteArray& bytes, qsizetype expected = -1) {
    if (bytes.size() < 4) return {};
    const quint32 size = qFromBigEndian<quint32>(bytes.constData());
    if (!size || size > maxBytes || (expected >= 0 && size != expected)) return {};
    const auto raw = qUncompress(bytes);
    return raw.size() == size ? raw : QByteArray();
}
}
void RewindBuffer::configure(int seconds, double snapshotsPerSecond) {
    clear();
    entryLimit_ = seconds > 0 && std::isfinite(snapshotsPerSecond)
        ? size_t(std::clamp(seconds, 1, 60) * std::clamp(snapshotsPerSecond, 1.0, 120.0)) + 1 : 0;
}
void RewindBuffer::clear() { entries_.clear(); bytesUsed_ = 0; }
bool RewindBuffer::capture(const QByteArray& state, const QImage& frame, double aspect) {
    if (!entryLimit_) return true;
    if (state.isEmpty() || state.size() > maxBytes || frame.isNull() ||
        frame.sizeInBytes() > maxBytes || !std::isfinite(aspect) || aspect <= 0) return false;
    if (quint64(frame.width()) * frame.height() * 4 > maxBytes) return false;
    const QImage rgb = frame.convertToFormat(QImage::Format_RGB32);
    if (rgb.isNull() || rgb.sizeInBytes() > maxBytes) return false;
    Entry next{qCompress(state, 1), qCompress(rgb.constBits(), rgb.sizeInBytes(), 1), rgb.size(), aspect};
    const qsizetype bytes = next.state.size() + next.pixels.size();
    if (next.state.isEmpty() || next.pixels.isEmpty() || bytes > byteLimit_) return false;
    while (!entries_.empty() && (entries_.size() >= entryLimit_ || bytesUsed_ + bytes > byteLimit_)) {
        bytesUsed_ -= entries_.front().state.size() + entries_.front().pixels.size();
        entries_.pop_front();
    }
    bytesUsed_ += bytes;
    entries_.push_back(std::move(next));
    return true;
}
bool RewindBuffer::stepBack(QByteArray& state, QImage& frame, double& aspect) {
    if (entries_.size() < 2) return false;
    const auto& target = entries_[entries_.size() - 2];
    auto raw = unpack(target.state);
    auto pixels = unpack(target.pixels, qsizetype(target.size.width()) * target.size.height() * 4);
    if (raw.isEmpty() || pixels.isEmpty()) { clear(); return false; }
    QImage image(target.size, QImage::Format_RGB32);
    if (image.isNull() || image.sizeInBytes() != pixels.size()) { clear(); return false; }
    std::memcpy(image.bits(), pixels.constData(), size_t(pixels.size()));
    state = std::move(raw); frame = std::move(image); aspect = target.aspect;
    bytesUsed_ -= entries_.back().state.size() + entries_.back().pixels.size();
    entries_.pop_back(); // Discard the future. Resuming starts a new branch.
    return true;
}
