#pragma once
#include "RomArchive.h"
#include "Cheats.h"
#include "../input/ControllerType.h"
#include <QThread>
#include <QImage>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>

class EmulationSession final : public QThread {
    Q_OBJECT
public:
    explicit EmulationSession(QString corePath, QString dataPath, QObject* parent = nullptr);
    ~EmulationSession() override;
    void loadGame(const QString& path, int archiveIndex = -1);
    void loadGame(const QString& path, const RomArchive::Entry& archiveEntry);
    void closeGame();
    void setPaused(bool paused);
    void resetGame();
    void hardResetGame();
    void changeDisc(const QString& path);
    void selectDisc(int index);
    void setDiscEjected(bool ejected);
    void setRegion(int region);
    void setBlarggFilter(int filter);
    void setFastCdLoading(bool enabled);
    void setRemoveSpriteLimit(bool enabled);
    void setAutoFixChecksum(bool enabled);
    void updateAudioClock(qint64 queuedUsecs, qint64 targetUsecs, bool available);
    bool takeVideoFrame(QImage& frame, double& aspectRatio);
    void saveState(int slot);
    void loadState(int slot);
    void setButton(unsigned port, unsigned id, bool pressed);
    void setButtons(unsigned port, unsigned mask);
    void setControllerConnected(unsigned port, bool connected);
    void setControllerSixButton(unsigned port, bool sixButton);
    void setControllerKind(unsigned player, ControllerType type);
    void setGunAim(unsigned player, int x, int y, bool offscreen);
    void advanceFrame();
    void setFrameAdvanceHeld(bool held);
    void startMovieRecording(const QString& path);
    void playMovie(const QString& path);
    void resumeMovieRecording(const QString& path);
    void rerecordMovie(const QString& path);
    void takeOverMovie();
    void stopMovie();
    void setCheats(const CheatList& cheats);
    void releaseButtons();
    void setFastForward(bool enabled);
    void setFastForwardFactor(int factor);
    void setRewind(bool enabled);
    void setRewindEnabled(bool enabled);
    void setRewindMemoryMiB(int mebibytes);
    void setBackgroundPaused(bool paused);
    void requestSlotStatus(int selected);
    void shutdown();
    bool hasGame() const { return loaded_.load(); }
signals:
    void movieStatusChanged(int mode, const QString& path, quint64 frame, quint64 length);
    void movieHardwareChanged(const QByteArray& controllersJson);
    void videoFrameReady();
    void videoFrame(const QImage& frame, double aspectRatio);
    void audioReset();
    void audioSamples(const QByteArray& pcm, int sampleRate);
    void gameLoaded(const QString& path, double fps, const QString& contentName);
    void gameClosed();
    void discStateChanged(const QStringList& paths, int index, bool ejected);
    void pausedChanged(bool paused);
    void effectivePauseChanged(bool paused);
    void rewindingChanged(bool enabled);
    void rewindProgress(qint64 remainingMs, qint64 totalMs);
    void cheatsChanged(const CheatList& cheats, int platform, bool supported, const QString& detail);
    void stateSlotsChanged(int selected, unsigned occupied);
    void notification(const QString& text);
    void actionCompleted(const QString& text);
    void actionNotice(const QString& text);
    void errorOccurred(const QString& text);
protected:
    void run() override;
private:
    enum class Type { Load, Close, Pause, Reset, HardReset, Region, Filter, CdLoading, SpriteLimit, Checksum, Port, ControllerType, ControllerKind, FrameAdvance, Cheats, Save, Restore, Slots, Disc, DiscIndex, Eject, MovieRecord, MoviePlay, MovieResume, MovieFork, MovieStop, MovieTakeOver };
    struct Command {
        Type type;
        QString path;
        int value = 0;
        std::optional<RomArchive::Entry> archiveEntry = std::nullopt;
        CheatList cheats{};
    };
    void enqueue(Command command);
    void publishVideo(const QImage& frame, double aspectRatio);
    QString corePath_, dataPath_;
    std::mutex mutex_, videoMutex_;
    QImage latestFrame_;
    double latestAspect_ = 4.0 / 3.0;
    bool videoPending_ = false;
    std::atomic<qint64> audioQueueUs_{0}, audioTargetUs_{0}, audioFeedbackNs_{0};
    std::atomic<bool> audioClockAvailable_{false};
    std::deque<Command> commands_;
    std::atomic<bool> quitting_{false}, loaded_{false}, fastForward_{false};
    std::atomic<bool> rewind_{false}, backgroundPaused_{false};
    std::atomic<bool> rewindEnabled_{true}, frameAdvanceHeld_{false};
    std::atomic<int> fastForwardFactor_{4}, rewindMemoryMiB_{128};
    std::atomic<unsigned> buttons_[3]{};
    std::atomic<quint64> gunPositions_[3]{{quint64(1)<<32},{quint64(1)<<32},{quint64(1)<<32}};
};
