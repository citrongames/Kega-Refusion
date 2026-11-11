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
    bool startRecording(const QString& path, QString& error);
    bool play(const QString& path, bool resumeRecording, QString& error);
    bool rerecord(const QString& path, QString& error);
    bool finish(bool unload, QString& error);
    bool beforeFrame(const MovieInput& live, QString& error, MovieInput* applied = nullptr);
    void afterFrame();
    bool saveSlot(int slot, QString& error);
    bool saveFile(const QString& path, QString& error);
    bool loadSlot(int slot, QImage& frame, double& aspect, bool& missing, QString& error);
    bool loadFile(const QString& path, QImage& frame, double& aspect, bool& missing, QString& error);
    unsigned occupiedSlots() const;
    QByteArray captureRewind() const;
    bool restoreRewind(const QByteArray& state, QString& error);
    // Restore only the core/battery payload for a preview; leave tape/cursor untouched.
    bool restoreRewindCore(const QByteArray& state, QString& error);
    Mode mode() const { return mode_; }
    bool active() const { return mode_ != Off; }
    bool recording() const { return mode_ == Recording; }
    bool readOnly() const { return mode_ == Playback || mode_ == Finished; }
    quint64 position() const { return cursor_; }
    quint64 length() const { return tape_.size(); }
    QString path() const { return path_; }
    QByteArray controllers() const;
    double frameRate() const;
private:
    bool saveSlotFile(const QString& path, QString& error, bool keepPrevious);
    QString slotPath(int slot) const;
    bool saveRecording(QString& error);
    LibretroHost& host_;
    QString dataPath_, path_;
    MovieTape tape_;
    quint64 cursor_ = 0;
    Mode mode_ = Off;
};
