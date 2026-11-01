#include "RewindBuffer.h"
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr qsizetype maxBytes = 64 * 1024 * 1024;
void appendWord(QByteArray& out, quint32 value) {
    const auto word = qToLittleEndian(value);
    out.append(reinterpret_cast<const char*>(&word), 4);
}
// Read only complete words; memcpy avoids unaligned access and aliasing UB.
qsizetype sameBytes(const char* a, const char* b, qsizetype count) {
    qsizetype i = 0;
    while (i + 8 <= count) {
        quint64 x, y; std::memcpy(&x, a+i, 8); std::memcpy(&y, b+i, 8);
        if (x != y) break;
        i += 8;
    }
    while (i < count && a[i] == b[i]) ++i;
    return i;
}
}
RewindBuffer::Patch RewindBuffer::encode(const QByteArray& older, const QByteArray& newer) {
    Patch result;
    result.restoredSize = older.size();
    if (older.size() == newer.size()) {
        QByteArray changes;
        qsizetype pos = 0;
        while (pos < older.size()) {
            const qsizetype skip = sameBytes(older.constData()+pos, newer.constData()+pos, older.size()-pos);
            pos += skip;
            if (pos == older.size()) break;
            const qsizetype start = pos++;
            // Merge short equal gaps: an extra run header costs eight bytes.
            while (pos < older.size()) {
                if (older[pos] == newer[pos] &&
                    sameBytes(older.constData()+pos,newer.constData()+pos,older.size()-pos) >= 8) break;
                ++pos;
            }
            if (changes.size() + 8 + pos-start >= older.size()) {
                result.bytes = qCompress(older, 1);
                return result;
            }
            appendWord(changes, quint32(skip)); appendWord(changes, quint32(pos-start));
            changes.append(older.constData()+start, pos-start);
        }
        // Empty change stream is valid; avoid qCompress's empty-input ambiguity.
        result.delta = true;
        result.bytes = changes.isEmpty() ? QByteArray(1, '\0') : QByteArray(1, '\1') + qCompress(changes, 1);
    } else result.bytes = qCompress(older, 1);
    return result;
}
QByteArray RewindBuffer::decode(const Patch& patch, const QByteArray& newer) {
    if (patch.restoredSize <= 0 || patch.restoredSize > maxBytes || patch.bytes.isEmpty()) return {};
    if (patch.delta && (patch.restoredSize != newer.size())) return {};
    if (patch.delta && patch.bytes == QByteArray(1, '\0')) return newer;
    const qsizetype prefix = patch.delta ? 1 : 0;
    if (patch.bytes.size() < prefix+4 || (patch.delta && patch.bytes[0] != '\1')) return {};
    const auto rawSize = qFromBigEndian<quint32>(patch.bytes.constData()+prefix);
    if (!rawSize || rawSize > maxBytes || (!patch.delta && rawSize != patch.restoredSize)) return {};
    const auto raw = qUncompress(reinterpret_cast<const uchar*>(patch.bytes.constData()+prefix), patch.bytes.size()-prefix);
    if (raw.size() != rawSize) return {};
    if (!patch.delta) return raw;
    QByteArray result = newer;
    qsizetype cursor = 0, position = 0;
    while (cursor < raw.size()) {
        if (raw.size()-cursor < 8) return {};
        const quint32 skip = qFromLittleEndian<quint32>(raw.constData()+cursor);
        const quint32 count = qFromLittleEndian<quint32>(raw.constData()+cursor+4);
        cursor += 8;
        if (!count || quint64(position)+skip+count > quint64(result.size()) || count > quint64(raw.size()-cursor)) return {};
        position += skip;
        std::memcpy(result.data()+position, raw.constData()+cursor, count);
        position += count; cursor += count;
    }
    return result;
}
qsizetype RewindBuffer::entryBytes(const Entry& e) { return e.state.bytes.size()+e.pixels.bytes.size()+sizeof(Entry); }
void RewindBuffer::configure(int seconds, double snapshotsPerSecond) {
    clear();
    entryLimit_ = seconds > 0 && std::isfinite(snapshotsPerSecond)
        ? size_t(std::clamp(seconds,1,60)*std::clamp(snapshotsPerSecond,1.0,120.0))+1 : 0;
}
void RewindBuffer::clear() {
    entries_.clear(); currentState_.clear(); currentPixels_.clear(); currentSize_ = {};
    bytesUsed_ = 0;
}
bool RewindBuffer::capture(const QByteArray& state, const QImage& frame, double aspect) {
    if (!entryLimit_) return true;
    if (state.isEmpty() || state.size()>maxBytes || frame.isNull() || frame.sizeInBytes()>maxBytes ||
        !std::isfinite(aspect) || aspect<=0 || quint64(frame.width())*frame.height()*4>maxBytes) return false;
    const QImage rgb = frame.convertToFormat(QImage::Format_RGB32);
    if (rgb.isNull() || rgb.sizeInBytes()>maxBytes) return false;
    const QByteArray pixels(reinterpret_cast<const char*>(rgb.constBits()),rgb.sizeInBytes());
    const qsizetype anchorBytes = state.size()+pixels.size();
    if (anchorBytes>byteLimit_) { clear(); return false; }
    if (!currentState_.isEmpty()) {
        Entry previous{encode(currentState_,state),encode(currentPixels_,pixels),currentSize_,currentAspect_};
        if (previous.state.bytes.isEmpty() || previous.pixels.bytes.isEmpty()) { clear(); return false; }
        bytesUsed_ += entryBytes(previous);
        entries_.push_back(std::move(previous));
    }
    bytesUsed_ += anchorBytes-currentState_.size()-currentPixels_.size();
    currentState_ = state; currentPixels_ = pixels; currentSize_ = rgb.size(); currentAspect_ = aspect;
    while (!entries_.empty() && (size()>entryLimit_ || bytesUsed_>byteLimit_)) {
        bytesUsed_ -= entryBytes(entries_.front()); entries_.pop_front();
    }
    return true;
}
bool RewindBuffer::stepBack(QByteArray& state, QImage& frame, double& aspect) {
    if (entries_.empty()) return false;
    const auto& target = entries_.back();
    auto raw = decode(target.state,currentState_);
    auto pixels = decode(target.pixels,currentPixels_);
    if (raw.isEmpty() || pixels.isEmpty() || quint64(target.size.width())*target.size.height()*4 != quint64(pixels.size())) {
        clear(); return false;
    }
    QImage image(target.size,QImage::Format_RGB32);
    if (image.isNull() || image.sizeInBytes()!=pixels.size()) { clear(); return false; }
    std::memcpy(image.bits(),pixels.constData(),size_t(pixels.size()));
    bytesUsed_ += raw.size()+pixels.size()-currentState_.size()-currentPixels_.size()-entryBytes(target);
    currentState_ = std::move(raw); currentPixels_ = std::move(pixels);
    currentSize_ = target.size; currentAspect_ = target.aspect;
    entries_.pop_back();
    // A size-changing older anchor can be larger than the newer one.
    while (!entries_.empty() && bytesUsed_>byteLimit_) {
        bytesUsed_ -= entryBytes(entries_.front()); entries_.pop_front();
    }
    state = currentState_; frame = std::move(image); aspect = currentAspect_;
    return true;
}
