#include "EmulationSession.h"
#include "LibretroHost.h"
#include "RewindBuffer.h"
#include "FramePacer.h"
#include <algorithm>
#include <chrono>
#include <thread>

EmulationSession::EmulationSession(QString corePath, QString dataPath, QObject* parent)
    : QThread(parent), corePath_(std::move(corePath)), dataPath_(std::move(dataPath)) { start(); }
EmulationSession::~EmulationSession() { shutdown(); }
void EmulationSession::enqueue(Command command) {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back(std::move(command));
}
void EmulationSession::loadGame(const QString& path, int archiveIndex) { enqueue({Type::Load,path,archiveIndex}); }
void EmulationSession::setPaused(bool paused) { enqueue({Type::Pause,{},paused ? 1 : 0}); }
void EmulationSession::resetGame() { enqueue({Type::Reset,{}}); }
void EmulationSession::hardResetGame() { enqueue({Type::HardReset,{}}); }
void EmulationSession::setRegion(int region) { enqueue({Type::Region,{},region}); }
void EmulationSession::setBlarggFilter(int filter) { enqueue({Type::Filter,{},filter}); }
void EmulationSession::setAutoFixChecksum(bool enabled) { enqueue({Type::Checksum,{},enabled ? 1 : 0}); }
void EmulationSession::updateAudioClock(qint64 queuedUsecs, qint64 targetUsecs, bool available) {
    audioQueueUs_.store(queuedUsecs); audioTargetUs_.store(targetUsecs);
    audioClockAvailable_.store(available);
    audioFeedbackNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void EmulationSession::publishVideo(const QImage& frame, double aspectRatio) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        latestFrame_ = frame; latestAspect_ = aspectRatio;
        if (!videoPending_) { videoPending_ = true; notify = true; }
    }
    if (notify) emit videoFrameReady();
    // Kept for instrumentation; the application uses the coalesced ready signal.
    emit videoFrame(frame,aspectRatio);
}
bool EmulationSession::takeVideoFrame(QImage& frame, double& aspectRatio) {
    std::lock_guard<std::mutex> lock(videoMutex_);
    if (!videoPending_) return false;
    frame = latestFrame_; aspectRatio = latestAspect_; videoPending_ = false;
    return !frame.isNull();
}
void EmulationSession::saveState(int slot) { enqueue({Type::Save,{},slot}); }
void EmulationSession::loadState(int slot) { enqueue({Type::Restore,{},slot}); }
void EmulationSession::setButton(unsigned port, unsigned id, bool pressed) {
    if (port >= 2 || id >= 16) return;
    if (pressed) buttons_[port].fetch_or(1u << id); else buttons_[port].fetch_and(~(1u << id));
}
void EmulationSession::setButtons(unsigned port, unsigned mask) { if (port < 2) buttons_[port].store(mask & 0xffffu); }
void EmulationSession::releaseButtons() { for (auto& mask : buttons_) mask.store(0); }
void EmulationSession::setFastForward(bool enabled) { fastForward_.store(enabled); }
void EmulationSession::setFastForwardFactor(int factor) { fastForwardFactor_.store(factor == 0 ? 0 : std::clamp(factor, 2, 32)); }
void EmulationSession::setRewind(bool enabled) { rewind_.store(enabled); }
void EmulationSession::setRewindSeconds(int seconds) { rewindSeconds_.store(std::clamp(seconds, 0, 60)); }
void EmulationSession::setBackgroundPaused(bool paused) { backgroundPaused_.store(paused); }
void EmulationSession::requestSlotStatus(int selected) { enqueue({Type::Slots,{},selected}); }
void EmulationSession::shutdown() { quitting_.store(true); if (isRunning()) wait(); }
void EmulationSession::run() {
    using Clock = std::chrono::steady_clock;
    LibretroHost host(corePath_,dataPath_);
    host.message = [this](const QString& message) { emit notification(message); };
    RewindBuffer history;
    int historySeconds = rewindSeconds_.load();
    bool historyHealthy = true;
    unsigned captureFrame = 0;
    auto clearHistory = [&] {
        history.configure(historySeconds, host.fps() / 2.0);
        historyHealthy = true;
        captureFrame = 0;
    };
    clearHistory();
    bool paused = false, effectivePaused = false, rewindLatched = false, rewinding = false;
    auto rewindStatus = [&](bool enabled) {
        if (rewinding != enabled) { rewinding = enabled; emit rewindingChanged(enabled); }
    };
    auto nowNs = [] { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); };
    FramePacer pacer;
    FrameWaiter waiter;
    pacer.reset(nowNs());
    auto clearAudio = [&] { audioClockAvailable_.store(false); emit audioReset(); };
    auto lastFlush = Clock::now();
    bool optionsPending = false, fastLatched = false;
    qint64 nextPresentationNs = 0;
    while (!quitting_.load()) {
        std::deque<Command> pending;
        { std::lock_guard<std::mutex> lock(mutex_); pending.swap(commands_); }
        for (const auto& command : pending) {
            QString error;
            switch (command.type) {
            case Type::Load:
                if (host.load(command.path,error,command.value)) {
                    loaded_.store(true); releaseButtons(); paused = false;
                    rewind_.store(false); rewindStatus(false); clearHistory();
                    clearAudio();
                    emit gameLoaded(host.gamePath(),host.fps(),host.contentName());
                    emit stateSlotsChanged(-1,host.occupiedStateSlots());
                    emit pausedChanged(false);
                    pacer.reset(nowNs());
                } else {
                    if (!host.loaded()) { clearHistory(); rewindStatus(false); }
                    emit errorOccurred(error);
                }
                loaded_.store(host.loaded());
                break;
            case Type::Pause:
                paused = command.value != 0;
                clearAudio();
                emit pausedChanged(paused);
                pacer.reset(nowNs());
                break;
            case Type::Reset:
                if (host.loaded()) {
                    host.reset(); clearHistory(); clearAudio();
                    emit actionCompleted(QStringLiteral("Мягкий сброс"));
                    pacer.reset(nowNs());
                }
                break;
            case Type::HardReset:
                if (host.loaded()) {
                    if (host.hardReset(error)) {
                        clearHistory(); clearAudio(); rewindStatus(false);
                        emit actionCompleted(QStringLiteral("Жёсткий сброс"));
                        emit stateSlotsChanged(-1,host.occupiedStateSlots());
                        pacer.reset(nowNs());
                    } else emit errorOccurred(error);
                    loaded_.store(host.loaded());
                }
                break;
            case Type::Checksum:
                if (host.setAutoFixChecksum(command.value != 0,error)) {
                    if (host.loaded()) {
                        if (host.hardReset(error)) {
                            clearHistory(); clearAudio(); rewindStatus(false); pacer.reset(nowNs());
                            emit actionCompleted(command.value ? QStringLiteral("Исправление суммы: включено")
                                                               : QStringLiteral("Исправление суммы: выключено"));
                        } else emit errorOccurred(error);
                        loaded_.store(host.loaded());
                    }
                } else emit errorOccurred(error);
                break;
            case Type::Region:
            case Type::Filter:
                if (command.type == Type::Region ? host.setRegion(command.value,error)
                                                 : host.setBlarggFilter(command.value,error)) {
                    optionsPending = true; clearHistory(); clearAudio();
                } else emit errorOccurred(error);
                break;
            case Type::Save:
                if (host.saveState(command.value,error)) {
                    emit stateSlotsChanged(-1,host.occupiedStateSlots());
                    emit actionCompleted(QStringLiteral("Сохранено: слот %1").arg(command.value));
                } else emit errorOccurred(error);
                break;
            case Type::Restore: {
                LibretroHost::StateLoadError failure;
                if (host.loadState(command.value,error,&failure)) {
                    clearHistory(); clearAudio();
                    emit actionCompleted(QStringLiteral("Загружено: слот %1").arg(command.value));
                    pacer.reset(nowNs());
                } else if (failure == LibretroHost::StateLoadError::Missing) {
                    emit actionNotice(error);
                } else emit errorOccurred(error);
                emit stateSlotsChanged(-1,host.occupiedStateSlots());
                break;
            }
            case Type::Slots:
                emit stateSlotsChanged(command.value,host.occupiedStateSlots());
                break;
            }
        }
        if (historySeconds != rewindSeconds_.load()) {
            historySeconds = rewindSeconds_.load(); clearHistory();
        }
        const bool background = backgroundPaused_.load();
        const bool nowPaused = host.loaded() && (paused || background);
        if (nowPaused != effectivePaused) {
            effectivePaused = nowPaused; emit effectivePauseChanged(nowPaused); clearAudio(); pacer.reset(nowNs());
        }
        const bool reverse = rewind_.load() && historySeconds > 0 && host.loaded() && !background && !optionsPending;
        if (reverse != rewindLatched) {
            rewindLatched = reverse; clearAudio(); pacer.reset(nowNs());
            if (!reverse) rewindStatus(false);
        }
        if (reverse) {
            QByteArray state;
            QImage frame;
            double aspect = 4.0 / 3.0;
            if (history.stepBack(state,frame,aspect)) {
                if (host.restoreRewindState(state)) {
                    captureFrame = 0; rewindStatus(true); publishVideo(frame,aspect);
                } else {
                    clearHistory(); historyHealthy = false; rewindStatus(false);
                    emit errorOccurred(QStringLiteral("Ядро отклонило состояние перемотки. История очищена."));
                }
            } else rewindStatus(false);
            // Hold at the oldest checkpoint rather than inadvertently running forward.
            const auto deadline = pacer.next(nowNs(),qint64(2000000000.0 / host.fps()));
            waiter.waitUntil(deadline);
            continue;
        }
        if (!host.loaded() || nowPaused) { std::this_thread::sleep_for(std::chrono::milliseconds(4)); continue; }
        const bool fast = fastForward_.load();
        if (fast != fastLatched) {
            fastLatched = fast; clearAudio(); pacer.reset(nowNs()); nextPresentationNs = 0;
            // Fast-forward creates a gap: discard the old timeline so rewind
            // cannot jump across it when normal recording resumes.
            if (fast) clearHistory();
        }
        const int factor = fast ? fastForwardFactor_.load() : 1;
        const bool present = !fast || nowNs() >= nextPresentationNs;
        for (unsigned port = 0; port < 2; ++port) host.setButtons(port,buttons_[port].load());
        // Fast-forward omits unseen rendering, PCM copies and rewind recording.
        // Emulated CPU/audio state still advances normally.
        host.runFrame(!fast || present || optionsPending, !fast);
        if (optionsPending) {
            optionsPending = false; clearHistory(); clearAudio(); pacer.reset(nowNs());
        }
        if (!fast && historySeconds && historyHealthy && ++captureFrame % 2 == 0) {
            if (!history.capture(host.captureRewindState(),host.frame(),host.aspectRatio())) {
                history.clear(); historyHealthy = false;
                emit notification(QStringLiteral("История перемотки недоступна для текущей игры."));
            }
        }
        if (present) {
            publishVideo(host.frame(),host.aspectRatio());
            nextPresentationNs = nowNs() + 16666667; // Fast-forward display stays near60Hz, independent of emulation speed.
        }
        if (!fast && !host.audio().isEmpty()) emit audioSamples(host.audio(),host.sampleRate());
        const auto now = Clock::now();
        if (now - lastFlush > std::chrono::seconds(5)) {
            QString error;
            if (!host.flushSaveRam(error)) emit notification(error);
            lastFlush = now;
        }
        const double clockScale = fast ? 1.0 : pacer.audioPeriodScale(nowNs(),audioFeedbackNs_.load(),
            audioQueueUs_.load(),audioTargetUs_.load(),audioClockAvailable_.load());
        if (factor != 0) {
            const auto deadline = pacer.next(nowNs(),qint64(1000000000.0 * clockScale / (host.fps() * factor)));
            waiter.waitUntil(deadline);
        } else pacer.reset(nowNs());
    }
    host.unload();
    loaded_.store(false);
}
