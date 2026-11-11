#include "RewindBuffer.h"
#include "VideoFrameGeometry.h"
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
void trimPatch(QByteArray& bytes) {
    const qsizetype slack = bytes.capacity() - bytes.size();
    if (slack >= 256 && slack >= bytes.size() / 8) bytes.squeeze();
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
                trimPatch(result.bytes);
                return result;
            }
            appendWord(changes, quint32(skip)); appendWord(changes, quint32(pos-start));
            changes.append(older.constData()+start, pos-start);
        }
        // Empty change stream is valid; avoid qCompress's empty-input ambiguity.
        result.delta = true;
        result.bytes = changes.isEmpty() ? QByteArray(1, '\0') : QByteArray(1, '\1') + qCompress(changes, 1);
    } else result.bytes = qCompress(older, 1);
    // qCompress may retain far more capacity than its compressed size. The
    // rewind limit counts capacity, so discard that slack before storing a patch.
    trimPatch(result.bytes);
    return result;
}
QByteArray RewindBuffer::decode(const Patch& patch, QByteArray newer) {
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
    qsizetype cursor = 0, position = 0;
    while (cursor < raw.size()) {
        if (raw.size()-cursor < 8) return {};
        const quint32 skip = qFromLittleEndian<quint32>(raw.constData()+cursor);
        const quint32 count = qFromLittleEndian<quint32>(raw.constData()+cursor+4);
        cursor += 8;
        if (!count || quint64(position)+skip+count > quint64(newer.size()) || count > quint64(raw.size()-cursor)) return {};
        position += skip;
        std::memcpy(newer.data()+position, raw.constData()+cursor, count);
        position += count; cursor += count;
    }
    return newer;
}
qsizetype RewindBuffer::entryBytes(const Entry& e) { return e.state.bytes.capacity()+e.pixels.bytes.capacity()+sizeof(Entry); }
void RewindBuffer::appendInput(const InputDisplayState& input) {
    if (!inputRuns_.empty() && inputRuns_.back().state == input) {
        ++inputRuns_.back().count;
    } else {
        inputRuns_.push_back({input,1});
        bytesUsed_ += sizeof(InputRun);
    }
}
void RewindBuffer::dropOldestInput() {
    if (inputRuns_.empty()) return;
    if (--inputRuns_.front().count == 0) {
        inputRuns_.pop_front();
        bytesUsed_ -= sizeof(InputRun);
    }
}
void RewindBuffer::dropNewestInput() {
    if (inputRuns_.empty()) return;
    if (--inputRuns_.back().count == 0) {
        inputRuns_.pop_back();
        bytesUsed_ -= sizeof(InputRun);
    }
}
size_t RewindBuffer::size() const {
    if (currentState_.isEmpty()) return 0;
    // After eviction, a replay-only oldest entry is a rendering prerequisite,
    // not another frame the user can land on.
    if (!entries_.empty() && entries_.front().controls) return entries_.size();
    return entries_.size() + 1;
}
void RewindBuffer::configure(bool enabled) {
    if (enabled_ == enabled) return;
    clear();
    enabled_ = enabled;
}
void RewindBuffer::setByteLimit(qsizetype byteLimit) {
    byteLimit = std::max<qsizetype>(0,byteLimit);
    if (byteLimit_ == byteLimit) return;
    byteLimit_ = byteLimit;
    trimToBudget();
}
void RewindBuffer::trimToBudget() {
    while (!entries_.empty() && bytesUsed_ > byteLimit_) {
        bytesUsed_ -= entryBytes(entries_.front());
        entries_.pop_front();
        dropOldestInput();
    }
    if (bytesUsed_ > byteLimit_) clear();
}
void RewindBuffer::clear() {
    entries_.clear(); inputRuns_.clear(); currentState_.clear(); currentPixels_.clear(); currentSize_ = {}; currentNativeSize_ = {};
    currentControls_.reset();
    bytesUsed_ = 0;
}
bool RewindBuffer::capture(const QByteArray& state, const QImage& frame, double aspect,
                           const InputDisplayState* input) {
    if (!enabled_) return true;
    if (state.isEmpty() || state.size()>maxBytes || frame.isNull() || frame.sizeInBytes()>maxBytes ||
        !std::isfinite(aspect) || aspect<=0 || quint64(frame.width())*frame.height()*4>maxBytes) return false;
    const QImage rgb = frame.convertToFormat(QImage::Format_RGB32);
    if (rgb.isNull() || rgb.sizeInBytes()>maxBytes) return false;
    const QByteArray pixels(reinterpret_cast<const char*>(rgb.constBits()),rgb.sizeInBytes());
    return append(state, pixels, rgb.size(), VideoFrameGeometry::nativeSize(frame), aspect, std::nullopt, input);
}
bool RewindBuffer::captureForReplay(const QByteArray& state, double aspect, const MovieInput& controls,
                                    const InputDisplayState* input) {
    if (!enabled_) return true;
    if (state.isEmpty() || state.size() > maxBytes || !std::isfinite(aspect) || aspect <= 0) return false;
    return append(state, {}, {}, {}, aspect, controls, input);
}
bool RewindBuffer::append(const QByteArray& state, const QByteArray& pixels, QSize size, QSize nativeSize,
                          double aspect, std::optional<MovieInput> controls, const InputDisplayState* input) {
    const qsizetype anchorBytes = state.capacity()+pixels.capacity();
    if (anchorBytes>byteLimit_) { clear(); return false; }
    const size_t previousCount = currentState_.isEmpty() ? 0 : entries_.size() + 1;
    if (!currentState_.isEmpty()) {
        Entry previous{encode(currentState_,state), {}, currentSize_, currentNativeSize_, currentAspect_, currentControls_};
        if (!currentControls_) previous.pixels = encode(currentPixels_,pixels);
        if (previous.state.bytes.isEmpty() || (!currentControls_ && previous.pixels.bytes.isEmpty())) { clear(); return false; }
        bytesUsed_ += entryBytes(previous);
        entries_.push_back(std::move(previous));
    }
    bytesUsed_ += anchorBytes-currentState_.capacity()-currentPixels_.capacity();
    currentState_ = state; currentPixels_ = pixels; currentSize_ = size; currentNativeSize_ = nativeSize;
    currentAspect_ = aspect; currentControls_ = controls;
    // A history captured before input tracking began has neutral input. Runs
    // keep long holds to one snapshot rather than copying it every frame.
    if (input && inputRuns_.empty() && previousCount) {
        inputRuns_.push_back({{}, previousCount});
        bytesUsed_ += sizeof(InputRun);
    }
    if (input || !inputRuns_.empty()) appendInput(input ? *input : InputDisplayState{});
    trimToBudget();
    return !currentState_.isEmpty();
}
bool RewindBuffer::stepBack(QByteArray& state, QImage& frame, double& aspect, int steps,
                            InputDisplayState* input, ReplayFrame* replay) {
    if (input) *input = {};
    if (replay) *replay = {};
    if (steps < 1 || size() < 2) return false;
    const size_t count = std::min(size_t(steps), size() - 1);
    if (!replay && entries_[entries_.size() - count].controls) return false;
    // Apply the reverse deltas in order. Only the landing frame needs an image
    // and a core restore, even when one display tick spans several checkpoints.
    for (size_t i = 0; i < count; ++i) {
        const auto& target = entries_.back();
        const auto oldBytes = currentState_.capacity() + currentPixels_.capacity();
        currentState_ = decode(target.state,std::move(currentState_));
        currentPixels_ = target.controls ? QByteArray() : decode(target.pixels,std::move(currentPixels_));
        const auto newBytes = currentState_.capacity() + currentPixels_.capacity();
        if (currentState_.isEmpty() || newBytes > byteLimit_ ||
            (!target.controls && (currentPixels_.isEmpty() ||
             quint64(target.size.width()) * target.size.height() * 4 != quint64(currentPixels_.size())))) {
            clear(); return false;
        }
        bytesUsed_ += newBytes - oldBytes - entryBytes(target);
        currentSize_ = target.size; currentNativeSize_ = target.nativeSize; currentAspect_ = target.aspect;
        currentControls_ = target.controls;
        entries_.pop_back();
        dropNewestInput();
    }
    if (currentControls_) {
        // Read the preceding state without moving the cursor a second time.
        replay->previousState = decode(entries_.back().state,currentState_);
        if (replay->previousState.isEmpty()) { clear(); return false; }
        replay->controls = *currentControls_;
        frame = {};
    } else {
        QImage image(currentSize_,QImage::Format_RGB32);
        if (image.isNull() || image.sizeInBytes()!=currentPixels_.size()) { clear(); return false; }
        std::memcpy(image.bits(),currentPixels_.constData(),size_t(currentPixels_.size()));
        if (currentNativeSize_ != image.size()) VideoFrameGeometry::setNativeSize(image,currentNativeSize_);
        frame = std::move(image);
    }
    // A size-changing older anchor can be larger than the newer one.
    trimToBudget();
    if (currentState_.isEmpty()) return false;
    state = currentState_; aspect = currentAspect_;
    if (input && !inputRuns_.empty()) *input = inputRuns_.back().state;
    return true;
}
