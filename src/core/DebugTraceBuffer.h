#pragma once

#include "DebugTrace.h"

#include <memory>
#include <vector>

// The emulation thread owns the ring and every public operation. Only the
// writer consumes the separately published batches. It never reads the ring.
class DebugTraceBuffer {
public:
    DebugTraceBuffer();
    ~DebugTraceBuffer();

    bool start(const DebugTraceConfig& config, QString game, QString& error);
    void append(const DebugTraceRecord& record) noexcept;
    void stop() noexcept;
    bool clear(QString& error);
    bool exportRecent(QString path, QString& error);
    QVector<DebugTraceRecord> latest(unsigned limit) const;

    bool active() const noexcept;
    bool writing() const noexcept;
    quint64 total() const noexcept { return total_; }
    quint64 overwritten() const noexcept { return total_ - retained(); }
    quint32 capacity() const noexcept { return quint32(ring_.size()); }
    QString status() const;
    QString filePath() const { return filePath_; }

private:
    struct Writer;

    size_t retained() const noexcept;
    size_t firstSlot(size_t count) const noexcept;

    std::vector<DebugTraceRecord> ring_;
    std::unique_ptr<Writer> writer_;
    QString game_, filePath_;
    quint64 total_ = 0;
    size_t nextSlot_ = 0;
    bool capturing_ = false;
};
