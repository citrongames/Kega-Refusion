#pragma once
#include "RomArchive.h"
#include "Cheats.h"
#include "Achievements.h"
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
    void setPerformanceMetricsEnabled(bool enabled);
    void setGameFpsEnabled(bool enabled);
    void setExpansionDevices(bool cd, bool x32);
    void requestSlotStatus(int selected);
    void configureAchievements(bool enabled, bool hardcore);
    void loginAchievements(QString user, QString password);
    void logoutAchievements();
    void requestAchievements();
    void achievementResponse(quint64 id, int status, QByteArray body);
    // Network commands cross to the core only through this worker queue.
    void setNetworkMode(int mode, const QString& storageId = {}); // 0 off, 1 host, 2 client
    bool networkActive() const { return networkMode_.load()!=0; }
    void setNetworkRole(int role) { networkRole_.store(role); }
    void setNetworkWaiting(bool waiting) { networkWaiting_.store(waiting); }
    void requestNetworkSnapshot();
    void closeNetworkGame();
    void receiveNetworkSlots(unsigned mask);
    void acknowledgeNetworkDatagram() { auto n=networkQueuedDatagrams_.load();while(n>0&&!networkQueuedDatagrams_.compare_exchange_weak(n,n-1)){} }
    void acknowledgeNetworkSnapshot() { networkSnapshotPending_.store(false); }
    void receiveNetworkSnapshot(const QByteArray& snapshot);
    void receiveNetworkRewindPreview(const QByteArray& bytes);
    void acknowledgeNetworkRewindPreview() { networkPreviewPending_.store(false); }
    void setNetworkRoster(const QByteArray& roster);
    void setNetworkPeerId(quint64 id) { networkPeerId_.store(id); }
    void receiveNetworkDatagram(quint64 peer, const QByteArray& packet);
    void advanceNetworkFrame();
    void applyNetworkCommand(const QByteArray& command);
    static bool isNetworkMomentaryCommand(const QByteArray& command);
    static bool isNetworkMomentaryRelease(const QByteArray& command);
    void shutdown();
    bool hasGame() const { return loaded_.load(); }
signals:
    void networkModeFailed(const QString& error);
    void networkCommandRequested(const QByteArray& command);
    void networkSnapshotReady(const QByteArray& snapshot);
#ifdef KEGA_NETPLAY_TEST_INPUTS
    void networkFrameReady(const QByteArray& frame);
#endif
    void networkDatagramReady(quint64 peer, const QByteArray& packet);
    void networkStepRequested(quint64 epoch);
    void networkRewindPreviewReady(const QByteArray& preview);
    void networkSnapshotApplied(bool success, const QString& error);
    void networkResyncRequested();
    void networkGameLoading(const QString& path,int archiveIndex);
    void networkGameSource(const QString& path, int archiveIndex);
    void achievementsChanged(const AchievementsState& state);
    void achievementNotice(const AchievementNotice& notice);
    void achievementHttpRequest(const AchievementHttpRequest& request);
    void achievementsHardcoreChanged(bool enabled);
    void movieCompatibilityWarning(const QString& message);
    void movieStatusChanged(int mode, const QString& path, quint64 frame, quint64 length, double fps);
    void movieHardwareChanged(const QByteArray& controllersJson);
    void videoFrameReady();
    void videoFrame(const QImage& frame, double aspectRatio);
    void performanceMetricsChanged(double emulatedFps, double speedPercent, double gameFps, bool gameFpsAvailable);
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
    enum class Type { Load, Close, Pause, Reset, HardReset, Region, Filter, CdLoading, SpriteLimit, Checksum, Port, ControllerType, ControllerKind, FrameAdvance, Cheats, Save, Restore, Slots, Disc, DiscIndex, Eject, MovieRecord, MoviePlay, MovieResume, MovieFork, MovieStop, MovieTakeOver, AchievementsConfigure, AchievementsLogin, AchievementsLogout, AchievementsRefresh, AchievementsResponse, PerformanceMetrics, GameFps, Expansions , NetMode, NetSnapshot, NetReceiveState, NetReceiveFrame, NetFast, NetReverse, NetFactor, NetStepHold, NetSlots, NetRoster, NetDatagram, NetAdvance, NetRewindPreview};
    struct Command {
        Type type;
        QString path;
        int value = 0;
        std::optional<RomArchive::Entry> archiveEntry = std::nullopt;
        CheatList cheats{};
        QByteArray bytes{};
        quint64 requestId = 0;
        bool networkAuthorized = false;
    };
    static bool networkShared(Type type);
    static QByteArray encodeNetworkCommand(const Command& command);
    std::atomic_int networkMode_{0}, networkRole_{0};
    std::atomic_int networkQueuedDatagrams_{0};
    std::atomic<quint64> networkPeerId_{0};
    std::atomic_bool networkSnapshotPending_{false}, networkPreviewPending_{false};
    std::atomic_bool networkWaiting_{false};
    qint64 lastNetworkSaveMs_=0, lastNetworkHeavyMs_=0;
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
    // Local hold edges are separate from the shared state applied by the host.
    std::atomic<bool> networkFastRequested_{false}, networkRewindRequested_{false};
    std::atomic<bool> networkStepRequested_{false};
    std::atomic<unsigned> buttons_[3]{};
    std::atomic<quint64> gunPositions_[3]{{quint64(1)<<32},{quint64(1)<<32},{quint64(1)<<32}};
};
