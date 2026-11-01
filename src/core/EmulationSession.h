#pragma once
#include <QThread>
#include <QImage>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <atomic>
#include <deque>
#include <mutex>

class EmulationSession final : public QThread {
    Q_OBJECT
public:
    explicit EmulationSession(QString corePath, QString dataPath, QObject* parent = nullptr);
    ~EmulationSession() override;
    void loadGame(const QString& path, int archiveIndex = -1);
    void setPaused(bool paused);
    void resetGame();
    void hardResetGame();
    void changeDisc(const QString& path);
    void selectDisc(int index);
    void setDiscEjected(bool ejected);
    void setRegion(int region);
    void setBlarggFilter(int filter);
    void setAutoFixChecksum(bool enabled);
    void updateAudioClock(qint64 queuedUsecs, qint64 targetUsecs, bool available);
    bool takeVideoFrame(QImage& frame, double& aspectRatio);
    void saveState(int slot);
    void loadState(int slot);
    void setButton(unsigned port, unsigned id, bool pressed);
    void setButtons(unsigned port, unsigned mask);
    void releaseButtons();
    void setFastForward(bool enabled);
    void setFastForwardFactor(int factor);
    void setRewind(bool enabled);
    void setRewindSeconds(int seconds);
    void setBackgroundPaused(bool paused);
    void requestSlotStatus(int selected);
    void shutdown();
    bool hasGame() const { return loaded_.load(); }
signals:
    void videoFrameReady();
    void videoFrame(const QImage& frame, double aspectRatio);
    void audioReset();
    void audioSamples(const QByteArray& pcm, int sampleRate);
    void gameLoaded(const QString& path, double fps, const QString& contentName);
    void discStateChanged(const QStringList& paths, int index, bool ejected);
    void pausedChanged(bool paused);
    void effectivePauseChanged(bool paused);
    void rewindingChanged(bool enabled);
    void stateSlotsChanged(int selected, unsigned occupied);
    void notification(const QString& text);
    void actionCompleted(const QString& text);
    void actionNotice(const QString& text);
    void errorOccurred(const QString& text);
protected:
    void run() override;
private:
    enum class Type { Load, Pause, Reset, HardReset, Region, Filter, Checksum, Save, Restore, Slots, Disc, DiscIndex, Eject };
    struct Command { Type type; QString path; int value = 0; };
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
    std::atomic<int> fastForwardFactor_{4}, rewindSeconds_{30};
    std::atomic<unsigned> buttons_[2]{};
};
