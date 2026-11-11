#pragma once
#include "InputDisplay.h"
#include "MovieTape.h"
#include <optional>
#include <QByteArray>
#include <QImage>
#include <deque>

// Worker-owned reverse deltas. The current raw checkpoint anchors the chain.
class RewindBuffer final {
public:
    explicit RewindBuffer(qsizetype byteLimit = 128 * 1024 * 1024) : byteLimit_(byteLimit) {}
    void configure(bool enabled);
    // Keep the newest checkpoints that fit the new budget.
    void setByteLimit(qsizetype byteLimit);
    void clear();
    bool capture(const QByteArray& state, const QImage& frame, double aspect,
                 const InputDisplayState* input = nullptr);
    // The caller can regenerate this picture from the preceding checkpoint and exact frame input.
    struct ReplayFrame { QByteArray previousState; MovieInput controls; };
    bool captureForReplay(const QByteArray& state, double aspect, const MovieInput& controls,
                          const InputDisplayState* input = nullptr);
    bool stepBack(QByteArray& state, QImage& frame, double& aspect, int steps = 1,
                  InputDisplayState* input = nullptr, ReplayFrame* replay = nullptr);
    size_t size() const;
    // Counts retained byte-array capacities, raw anchors, live Entry metadata,
    // and input-run metadata when input snapshots are supplied.
    // Transient encode/decode/image work, Qt control blocks, deque storage and
    // allocator overhead are outside this retained-storage budget.
    qsizetype bytesUsed() const { return bytesUsed_; }
private:
    struct Patch { QByteArray bytes; qsizetype restoredSize = 0; bool delta = false; };
    struct Entry { Patch state, pixels; QSize size, nativeSize; double aspect; std::optional<MovieInput> controls; };
    struct InputRun { InputDisplayState state; size_t count; };
    static Patch encode(const QByteArray& older, const QByteArray& newer);
    static QByteArray decode(const Patch& patch, QByteArray newer);
    static qsizetype entryBytes(const Entry& entry);
    bool append(const QByteArray& state, const QByteArray& pixels, QSize size, QSize nativeSize,
                double aspect, std::optional<MovieInput> controls, const InputDisplayState* input);
    void trimToBudget();
    void appendInput(const InputDisplayState& input);
    void dropOldestInput();
    void dropNewestInput();
    std::deque<Entry> entries_;
    std::deque<InputRun> inputRuns_;
    QByteArray currentState_, currentPixels_;
    std::optional<MovieInput> currentControls_;
    QSize currentSize_, currentNativeSize_;
    double currentAspect_ = 4.0 / 3.0;
    bool enabled_ = false;
    qsizetype byteLimit_, bytesUsed_ = 0;
};
