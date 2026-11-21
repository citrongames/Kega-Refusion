#pragma once

#include "DebugCoverage.h"

#include <array>
#include <memory>
#include <vector>

// Emulation-thread ownership. Only an explicit export snapshots the packed
// flags for another thread. The native callback never waits on that writer.
class DebugCoverageMap {
public:
    DebugCoverageMap();
    ~DebugCoverageMap();

    bool start(const QVector<kr_cov_source>& sources, QString game, QByteArray romHash, QString& error, quint32 instance = 0);
    void mark(const kr_cov_event& event) noexcept;
    void stop() noexcept { active_ = false; }
    void clear();

    bool active() const noexcept { return active_; }
    quint64 allocatedBytes() const noexcept { return packed_.size(); }
    quint64 revision() const noexcept { return revision_; }
    quint64 unknownCode() const noexcept { return unknownCode_; }
    quint64 unknownData() const noexcept { return unknownData_; }
    QVector<DebugCoverageSource> sources() const;
    QByteArray page(quint32 source, quint32 offset, unsigned length = 256) const;

    bool exportFile(QString path, QString& error);
    bool writing() const noexcept;
    QString status() const;

private:
    struct Export;
    struct Region {
        DebugCoverageSource info;
        size_t packedOffset = 0;
    };

    std::array<Region, KR_COV_MAX_SOURCES> regions_{};
    std::vector<quint8> packed_;
    QString game_;
    QByteArray romHash_;
    std::unique_ptr<Export> export_;
    quint32 instance_ = 0;
    quint64 revision_ = 0;
    quint64 unknownCode_ = 0;
    quint64 unknownData_ = 0;
    bool active_ = false;
    bool changed_ = false;
};
