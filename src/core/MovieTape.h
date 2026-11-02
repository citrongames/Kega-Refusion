#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QtGlobal>
#include <array>
#include <deque>

// One canonical poll of the three controller ports. Aim uses signed 16-bit X
// in bits 0..15, signed 16-bit Y in bits 16..31, and offscreen in bit 32.
struct MovieInput {
    std::array<quint16, 3> buttons{};
    std::array<quint64, 3> aim{};
    quint8 commands = 0;

    bool operator==(const MovieInput& other) const {
        return buttons == other.buttons && aim == other.aim && commands == other.commands;
    }
    bool operator!=(const MovieInput& other) const { return !(*this == other); }
};

// Owned by the emulation worker. A frame is one runFrame input, including any
// reset command to apply immediately before that frame. Only explicit save()
// writes to disk; append(), seek and truncate() never do file I/O.
class MovieTape final {
public:
    static constexpr quint8 SoftReset = 0x01;
    static constexpr quint64 MaxFrames = 1'000'000'000ULL;
    static constexpr qsizetype MaxInitialBytes = 32 * 1024 * 1024;
    static constexpr qsizetype MaxMetadataBytes = 64 * 1024;
    static constexpr qsizetype MaxRunBytes = 64 * 1024 * 1024;

    bool create(const QJsonObject& metadata, const QByteArray& initialState, QString& error);
    bool load(const QString& path, QString& error);
    bool save(const QString& path, QString& error) const;
    // Stored in the same .krm for fast continuation across app sessions.
    // The caller captures a full core snapshot at cursor after settling any
    // recording branch; subsequent timeline changes invalidate this anchor.
    bool setResumeState(const QByteArray& state, quint64 cursor, QString& error);
    QByteArray resumeState() const { return resumeState_; }
    quint64 resumeFrame() const { return resumeFrame_; }
    bool append(const MovieInput& input, QString& error);
    bool inputAt(quint64 frame, MovieInput& input) const;
    bool truncate(quint64 frameCount);
    // A checkpoint contains the complete input prefix through cursor. It can
    // restore a recording after a later branch without copying the full tape
    // on every frame or retaining historical branches in the MovieTape.
    QByteArray checkpoint(quint64 cursor, QString& error) const;
    bool restoreCheckpoint(const QByteArray& bytes, quint64& cursor, bool readOnly, QString& error);
    // Playback takeover copies the chosen prefix into a new movie identity.
    bool forkAt(quint64 frameCount, QString& error);
    void clear();

    bool isValid() const { return id_.size() == 16; }
    quint64 size() const { return frameCount_; }
    QJsonObject metadata() const { return metadata_; }
    QByteArray initialState() const { return initialState_; }
    QByteArray id() const { return id_; }
    qsizetype bytesUsed() const;

private:
    struct Run { MovieInput input; quint64 endFrame = 0; };
    // Leaves room for deque block maps and allocator overhead within the run
    // budget; the exact allocation footprint remains implementation-specific.
    static constexpr qsizetype MaxRuns = MaxRunBytes / (sizeof(Run) + 16);
    static bool parseRuns(const char* bytes, quint32 count, quint64 frames,
                          std::deque<Run>& out, QString& error);
    void appendRuns(QByteArray& bytes, quint64 frames) const;
    void invalidateResume() { QByteArray().swap(resumeState_); resumeFrame_ = 0; }
    bool prefixMatches(const std::deque<Run>& prefix, quint64 cursor) const;

    QJsonObject metadata_;
    QByteArray metadataBytes_;
    QByteArray initialState_;
    QByteArray resumeState_;
    quint64 resumeFrame_ = 0;
    QByteArray fingerprint_;
    QByteArray id_;
    std::deque<Run> runs_;
    quint64 frameCount_ = 0;
};
