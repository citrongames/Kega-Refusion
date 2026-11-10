#pragma once
#include "RomArchive.h"
#include "InputDisplay.h"
#include "Cheats.h"
#include "Achievements.h"
#include "../input/ControllerType.h"
#include "../input/ControllerTopology.h"
#include <QThread>
#include <QImage>
#include <QByteArray>
#include <QJsonObject>
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
    void setInputDisplayEnabled(bool enabled);
    InputDisplayState inputDisplay();
    void resetGame();
    void hardResetGame();
    void changeDisc(const QString& path);
    void selectDisc(int index);
    void setDiscEjected(bool ejected);
    void setRegion(int region);
    void setBlarggFilter(int filter);
    void setDebugMasks(quint32 video, quint64 audio);
    void setDebugBackdrop(quint32 rgb);
    void setDebugAudioGain(unsigned channel, unsigned percent);
    void setFastCdLoading(bool enabled);
    void setPerGameCdBackup(bool enabled);
    void setRemoveSpriteLimit(bool enabled);
    void setAutoFixChecksum(bool enabled);
    void setSmsFmEnabled(bool enabled);
    void updateAudioClock(qint64 queuedUsecs, qint64 targetUsecs, bool available);
    bool takeVideoFrame(QImage& frame, double& aspectRatio, QImage* spriteOwnership = nullptr);
    void setSpriteOwnershipEnabled(bool enabled) { spriteOwnershipEnabled_.store(enabled); }
    void saveState(int slot);
    void loadState(int slot);
    void saveStateToFile(const QString& path);
    void loadStateFromFile(const QString& path);
    void setButton(unsigned port, unsigned id, bool pressed);
    void setButtons(unsigned port, unsigned mask);
    void setControllerAdapter(ControllerAdapter adapter);
    int controllerAdapter() const { return controllerAdapter_.load(); }
    unsigned controllerPlayerCount() const { return controllerPlayerCount_.load(); }
    void setControllerConnected(unsigned port, bool connected);
    void setControllerSixButton(unsigned port, bool sixButton);
    void setControllerKind(unsigned player, ControllerType type);
    void setGunAim(unsigned player, int x, int y, bool offscreen);
    void addMouseMotion(unsigned player, int dx, int dy);
    void clearMouseMotion();
    void setPaddlePosition(unsigned player, int position);
    void advanceFrame();
    void backFrame();
    void setFrameBackHeld(bool held);
    void setFrameAdvanceHeld(bool held);
    void startMovieRecording(const QString& path);
    void playMovie(const QString& path);
    void resumeMovieRecording(const QString& path);
    void rerecordMovie(const QString& path);
    void takeOverMovie();
    void stopMovie();
    void startVideoRecording(const QString& path);
    void exportMovieVideo(const QString& moviePath, const QString& videoPath);
    void stopVideoRecording();
    void setCheats(const CheatList& cheats);
    void searchCheats(quint64 requestId, int width, int comparison, quint32 value, bool restart);
    void clearCheatSearch();
    void releaseButtons();
    void setFastForward(bool enabled);
    void setGameSpeedPercent(int percent);
    void setFastForwardFactor(int factor);
    void setRewind(bool enabled);
    void setRewindEnabled(bool enabled);
    void setRewindSpeed(bool accelerate, int fixedFactor);
    void setRewindMemoryMiB(int mebibytes);
    void setBackgroundPaused(bool paused);
    void setRunTimerEnabled(bool enabled);
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
    bool networkClient() const { return networkMode_.load()==2; }
    int networkRole() const { return networkRole_.load(); }
    bool networkLinkCable() const { return networkLinkCableDesired_.load(); }
    bool networkActive() const { return networkMode_.load()!=0; }
    void setNetworkRole(int role);
    void setNetworkLinkCableEnabled(bool enabled);
    void setNetworkWaiting(bool waiting) { networkWaiting_.store(waiting); }
    void requestNetworkSnapshot();
    void closeNetworkGame();
    void receiveNetworkSlots(unsigned mask);
    void acknowledgeNetworkDatagram() { auto n=networkQueuedDatagrams_.load();while(n>0&&!networkQueuedDatagrams_.compare_exchange_weak(n,n-1)){} }
    void acknowledgeNetworkQuality() { networkQualityPending_.store(false); }
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
    void networkQualityChanged(const QJsonObject& quality);
    void networkModeFailed(const QString& error);
    void networkModeApplied(int mode);
    void networkLinkCableChanged(bool enabled);
    void networkCommandRequested(const QByteArray& command);
    void networkSnapshotReady(const QByteArray& snapshot);
#ifdef KEGA_NETPLAY_TEST_INPUTS
    void networkFrameReady(const QByteArray& frame);
#endif
    void networkDatagramReady(quint64 peer, const QByteArray& packet);
    void networkStepRequested(quint64 epoch);
    void networkRewindPreviewReady(const QByteArray& preview);
    void networkSnapshotApplied(bool success, const QString& error);
    void networkSnapshotDiscarded();
    void networkResyncRequested();
    void networkGameLoading(const QString& path,int archiveIndex);
    void networkGameSource(const QString& path, int archiveIndex);
    void achievementsChanged(const AchievementsState& state);
    void achievementNotice(const AchievementNotice& notice);
    void achievementHttpRequest(const AchievementHttpRequest& request);
    void achievementsHardcoreChanged(bool enabled);
    void videoRecordingChanged(bool active, bool exporting, const QString& path);
    void videoExportProgress(quint64 done, quint64 total);
    void videoRecordingFinished(const QString& path, const QString& error);
    void movieCompatibilityWarning(const QString& message);
    void movieStatusChanged(int mode, const QString& path, quint64 frame, quint64 length, double fps);
    void controllerTopologyChanged(int adapter, unsigned playerCount);
    void networkRoleChanged(int role);
    void jcartChanged(bool enabled);
    void movieHardwareChanged(const QByteArray& controllersJson);
    void inputDisplayChanged();
    void videoFrameReady();
    void videoFrame(const QImage& frame, double aspectRatio);
    void runTimerChanged(quint64 frames, quint64 microseconds);
    void performanceMetricsChanged(double emulatedFps, double speedPercent, double gameFps, bool gameFpsAvailable);
    void audioReset();
    void audioSamples(const QByteArray& pcm, int sampleRate);
    void gameLoaded(const QString& path, double fps, const QString& contentName, ConsoleProfile profile);
    void gameClosed();
    void masterSystemChanged(bool masterSystem);
    void debugControlsChanged(quint32 videoAvailable, quint64 audioAvailable);
    void discStateChanged(const QStringList& paths, int index, bool ejected);
    void pausedChanged(bool paused);
    void effectivePauseChanged(bool paused);
    void rewindingChanged(bool enabled);
    void rewindProgress(qint64 remainingMs, qint64 totalMs);
    void cheatSearchResults(quint64 requestId, const QByteArray& result);
    void cheatsChanged(const CheatList& cheats, int platform, bool supported, const QString& detail);
    void stateSlotsChanged(int selected, unsigned occupied);
    void notification(const QString& text);
    void actionCompleted(const QString& text);
    void actionNotice(const QString& text);
    void errorOccurred(const QString& text);
protected:
    void run() override;
private:
    enum class Type { Load, Close, Pause, Reset, HardReset, Region, Filter, CdLoading, SpriteLimit, Checksum, Port, ControllerType, ControllerKind, FrameAdvance, Cheats, Save, Restore, Slots, Disc, DiscIndex, Eject, MovieRecord, MoviePlay, MovieResume, MovieFork, MovieStop, MovieTakeOver, AchievementsConfigure, AchievementsLogin, AchievementsLogout, AchievementsRefresh, AchievementsResponse, PerformanceMetrics, GameFps, Expansions , NetMode, NetSnapshot, NetReceiveState, NetReceiveFrame, NetFast, NetReverse, NetFactor, NetStepHold, NetSlots, NetRoster, NetDatagram, NetAdvance, NetRewindPreview, SmsFm, NetRole, NetLinkCable, DebugMasks, DebugBackdrop, DebugGain, Adapter, CheatSearch, CheatSearchClear, SaveFile, RestoreFile, VideoStart, VideoExport, VideoStop, FrameBack, RunTimer, InputDisplay, PerGameCdBackup};
    struct Command {
        Type type;
        QString path;
        int value = 0;
        std::optional<RomArchive::Entry> archiveEntry = std::nullopt;
        CheatList cheats{};
        QByteArray bytes{};
        quint64 requestId = 0;
        bool networkAuthorized = false;
        qsizetype retainedCheatBytes = 0;
    };
    static bool networkShared(Type type);
    static QByteArray encodeNetworkCommand(const Command& command);
    std::atomic_int networkMode_{0}, networkRole_{0};
    std::atomic_int networkQueuedDatagrams_{0};
    std::atomic<quint64> networkPeerId_{0}, networkLinkCableRevision_{0};
    std::atomic_bool networkSnapshotPending_{false}, networkPreviewPending_{false};
    std::atomic_bool networkQualityPending_{false};
    std::atomic_bool networkWaiting_{false}, networkLinkCableDesired_{false};
    qint64 lastNetworkSaveMs_=0, lastNetworkHeavyMs_=0;
    void enqueue(Command command);
    void publishVideo(const QImage& frame, double aspectRatio, const QImage& spriteOwnership = {});
    QString corePath_, dataPath_;
    std::mutex mutex_, videoMutex_;
    QImage latestFrame_, latestSpriteOwnership_;
    std::atomic_bool spriteOwnershipEnabled_{false};
    double latestAspect_ = 4.0 / 3.0;
    bool videoPending_ = false, inputDisplayPending_ = false;
    InputDisplayState latestInputDisplay_;
    std::atomic<qint64> audioQueueUs_{0}, audioTargetUs_{0}, audioFeedbackNs_{0};
    std::atomic<bool> audioClockAvailable_{false};
    std::deque<Command> commands_;
    qsizetype queuedCheatBytes_ = 0;
    std::atomic<bool> quitting_{false}, loaded_{false}, fastForward_{false};
    std::atomic<bool> rewind_{false}, backgroundPaused_{false};
    std::atomic<bool> rewindEnabled_{true}, frameAdvanceHeld_{false}, frameBackHeld_{false};
    std::atomic<int> gameSpeedPercent_{100}, fastForwardFactor_{4}, rewindMemoryMiB_{128};
    // Zero selects acceleration based on how long rewind is held.
    std::atomic<int> rewindFactor_{0};
    // Local hold edges are separate from the shared state applied by the host.
    std::atomic<bool> networkFastRequested_{false}, networkRewindRequested_{false};
    std::atomic<bool> networkStepRequested_{false};
    std::atomic<int> controllerAdapter_{0};
    std::atomic<unsigned> controllerPlayerCount_{2};
    std::atomic<unsigned> buttons_[MaxControllerPlayers]{};
    // Packed pending X/Y and reset generation; sampled only when a frame accepts input.
    std::array<std::atomic<quint64>,3> mouseMotion_{};
    std::atomic_bool jcart_{false};
    std::atomic<quint64> gunPositions_[3]{{quint64(1)<<32},{quint64(1)<<32},{quint64(1)<<32}};
};
