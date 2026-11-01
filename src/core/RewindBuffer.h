#pragma once
#include <QByteArray>
#include <QImage>
#include <deque>

// Worker-thread-only, compressed checkpoints. Never stores files or core pointers.
class RewindBuffer final {
public:
    explicit RewindBuffer(qsizetype byteLimit = 128 * 1024 * 1024) : byteLimit_(byteLimit) {}
    void configure(int seconds, double snapshotsPerSecond);
    void clear();
    bool capture(const QByteArray& state, const QImage& frame, double aspect);
    bool stepBack(QByteArray& state, QImage& frame, double& aspect);
    size_t size() const { return entries_.size(); }
    qsizetype bytesUsed() const { return bytesUsed_; }
private:
    struct Entry { QByteArray state, pixels; QSize size; double aspect; };
    std::deque<Entry> entries_;
    size_t entryLimit_ = 0;
    qsizetype byteLimit_, bytesUsed_ = 0;
};
