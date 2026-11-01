#pragma once
#include <QByteArray>
#include <QImage>
#include <deque>

// Worker-owned reverse deltas. The current raw checkpoint anchors the chain.
class RewindBuffer final {
public:
    explicit RewindBuffer(qsizetype byteLimit = 128 * 1024 * 1024) : byteLimit_(byteLimit) {}
    void configure(int seconds, double snapshotsPerSecond);
    void clear();
    bool capture(const QByteArray& state, const QImage& frame, double aspect);
    bool stepBack(QByteArray& state, QImage& frame, double& aspect);
    size_t size() const { return currentState_.isEmpty() ? 0 : entries_.size() + 1; }
    // Includes retained raw anchor and entry metadata, excludes transient work/allocator overhead.
    qsizetype bytesUsed() const { return bytesUsed_; }
private:
    struct Patch { QByteArray bytes; qsizetype restoredSize = 0; bool delta = false; };
    struct Entry { Patch state, pixels; QSize size, nativeSize; double aspect; };
    static Patch encode(const QByteArray& older, const QByteArray& newer);
    static QByteArray decode(const Patch& patch, const QByteArray& newer);
    static qsizetype entryBytes(const Entry& entry);
    std::deque<Entry> entries_;
    QByteArray currentState_, currentPixels_;
    QSize currentSize_, currentNativeSize_;
    double currentAspect_ = 4.0 / 3.0;
    size_t entryLimit_ = 0;
    qsizetype byteLimit_, bytesUsed_ = 0;
};
