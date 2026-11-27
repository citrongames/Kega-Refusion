#pragma once
#include "MovieTape.h"
#include <QImage>
#include <QString>
class LibretroHost;

// Emulation-thread owner of one movie and its isolated quick-state namespace.
class MovieSession final {
public:
    enum Mode { Off, Recording, Playback, Finished };
    MovieSession(LibretroHost& host, QString dataPath);
    bool startRecording(const QString& path, QString& error, bool persistStart = true);
    bool play(const QString& path, bool resumeRecording, QString& error);
    bool playPrepared(const QString& path, MovieTape candidate, bool resumeRecording, QString& error);
    bool rerecord(const QString& path, QString& error, bool persistStart = true);
    // Capture the final anchor and take the session live at a frame boundary.
    // The caller owns the detached tape and may serialize it on a worker.
    bool detachRecording(MovieTape& tape, QString& error);
    bool finish(bool unload, QString& error);
    bool beforeFrame(const MovieInput& live, QString& error, MovieInput* applied = nullptr);
    void afterFrame();
    bool saveSlot(int slot, QString& error);
    bool saveFile(const QString& path, QString& error);
    bool loadSlot(int slot, QImage& frame, double& aspect, bool& missing, QString& error);
    bool loadFile(const QString& path, QImage& frame, double& aspect, bool& missing, QString& error);
    unsigned occupiedSlots() const;
    // In-memory movie state with the input prefix through the current cursor.
    QByteArray captureTemporaryState(QString& error, qsizetype maxBytes = 32 * 1024 * 1024) const;
    bool restoreTemporaryState(const QByteArray& state, QString& error);
    QByteArray captureRewind() const;
    bool restoreRewind(const QByteArray& state, QString& error);
    // Restore only the core/battery payload for a preview; leave tape/cursor untouched.
    bool restoreRewindCore(const QByteArray& state, QString& error);
    Mode mode() const { return mode_; }
    bool active() const { return mode_ != Off; }
    bool recording() const { return mode_ == Recording; }
    bool requiresNewFile() const { return requiresNewFile_; }
    bool readOnly() const { return mode_ == Playback || mode_ == Finished; }
    quint64 position() const { return cursor_; }
    quint64 length() const { return tape_.size(); }
    QString path() const { return path_; }
    QByteArray id() const { return tape_.id(); }
    QByteArray controllers() const;
    double frameRate() const;
private:
    bool restoreCheckpoint(const QByteArray& state, const QByteArray& prefix, QString& error);
    bool saveSlotFile(const QString& path, QString& error, bool keepPrevious);
    QString slotPath(int slot) const;
    bool prepareRecordingEnd(QString& error);
    bool leaveMovie(bool unload, QString& error);
    LibretroHost& host_;
    QString path_;
    MovieTape tape_;
    quint64 cursor_ = 0;
    Mode mode_ = Off;
    bool requiresNewFile_ = false;
};
