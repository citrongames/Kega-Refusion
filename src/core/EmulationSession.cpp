#include "EmulationSession.h"
#include "LibretroHost.h"
#include "RewindBuffer.h"
#include "FramePacer.h"
#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

EmulationSession::EmulationSession(QString corePath, QString dataPath, QObject* parent)
    : QThread(parent), corePath_(std::move(corePath)), dataPath_(std::move(dataPath)) { qRegisterMetaType<CheatList>(); start(); }
EmulationSession::~EmulationSession() { shutdown(); }
void EmulationSession::enqueue(Command command) {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back(std::move(command));
}
void EmulationSession::loadGame(const QString& path, int archiveIndex) { enqueue({Type::Load,path,archiveIndex}); }
void EmulationSession::loadGame(const QString& path, const RomArchive::Entry& archiveEntry) {
    enqueue({Type::Load,path,archiveEntry.index,archiveEntry});
}
void EmulationSession::setPaused(bool paused) { enqueue({Type::Pause,{},paused ? 1 : 0}); }
void EmulationSession::resetGame() { enqueue({Type::Reset,{}}); }
void EmulationSession::hardResetGame() { enqueue({Type::HardReset,{}}); }
void EmulationSession::changeDisc(const QString& path) { enqueue({Type::Disc,path}); }
void EmulationSession::selectDisc(int index) { enqueue({Type::DiscIndex,{},index}); }
void EmulationSession::setDiscEjected(bool ejected) { enqueue({Type::Eject,{},ejected ? 1 : 0}); }
void EmulationSession::setRegion(int region) { enqueue({Type::Region,{},region}); }
void EmulationSession::setBlarggFilter(int filter) { enqueue({Type::Filter,{},filter}); }
void EmulationSession::setFastCdLoading(bool enabled) { enqueue({Type::CdLoading,{},enabled ? 1 : 0}); }
void EmulationSession::setRemoveSpriteLimit(bool enabled) { enqueue({Type::SpriteLimit,{},enabled ? 1 : 0}); }
void EmulationSession::setAutoFixChecksum(bool enabled) { enqueue({Type::Checksum,{},enabled ? 1 : 0}); }
void EmulationSession::setControllerConnected(unsigned port, bool connected) {
    if (port < 2) enqueue({Type::Port,{},int(port) | (connected ? 2 : 0)});
}
void EmulationSession::setControllerSixButton(unsigned port, bool sixButton) {
    if (port < 2) enqueue({Type::ControllerType,{},int(port) | (sixButton ? 2 : 0)});
}
void EmulationSession::setControllerKind(unsigned player, ControllerType type) {
    if (player < 2 && int(type) >= 0 && int(type) <= int(ControllerType::Justifier))
        enqueue({Type::ControllerKind,{},int(player) | (int(type) << 1)});
}
void EmulationSession::setGunAim(unsigned player, int x, int y, bool offscreen) {
    if (player >= 2) return;
    const quint64 packed = quint16(std::clamp(x,-32767,32767)) |
        (quint64(quint16(std::clamp(y,-32767,32767))) << 16) | (quint64(offscreen) << 32);
    gunPositions_[player].store(packed);
}
void EmulationSession::advanceFrame() { enqueue({Type::FrameAdvance,{}}); }
void EmulationSession::setCheats(const CheatList& cheats) {
    Command command{Type::Cheats,{}}; command.cheats = cheats; enqueue(std::move(command));
}
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
void EmulationSession::setRewindEnabled(bool enabled) { rewindEnabled_.store(enabled); }
void EmulationSession::setRewindMemoryMiB(int mebibytes) { rewindMemoryMiB_.store(std::clamp(mebibytes,16,1024)); }
void EmulationSession::setBackgroundPaused(bool paused) { backgroundPaused_.store(paused); }
void EmulationSession::requestSlotStatus(int selected) { enqueue({Type::Slots,{},selected}); }
void EmulationSession::shutdown() { quitting_.store(true); if (isRunning()) wait(); }
void EmulationSession::run() {
    using Clock = std::chrono::steady_clock;
    LibretroHost host(corePath_,dataPath_);
    host.message = [this](const QString& message) { emit notification(message); };
    auto syncInput = [&] {
        for (unsigned player=0;player<2;++player) {
            host.setButtons(player,buttons_[player].load());
            const quint64 aim = gunPositions_[player].load();
            host.setGunAim(player,int16_t(aim & 0xffff),int16_t((aim >> 16) & 0xffff),bool(aim >> 32));
        }
    };
    auto cheatStatus = [&] {
        emit cheatsChanged(host.cheats(),int(host.cheatPlatform()),host.cheatsSupported(),host.cheatSupportText());
    };
    bool lastDiscEjected = false;
    auto discStatus = [&] {
        lastDiscEjected = host.discEjected();
        emit discStateChanged(host.discPaths(),host.discIndex(),lastDiscEjected);
    };
    RewindBuffer history;
    bool historyEnabled = rewindEnabled_.load();
    int historyMemoryMiB = rewindMemoryMiB_.load();
    bool historyHealthy = true;
    unsigned captureFrame = 0;
    auto clearHistory = [&] {
        history.setByteLimit(qsizetype(historyMemoryMiB) * 1024 * 1024);
        history.configure(historyEnabled);
        historyHealthy = true;
        captureFrame = 0;
    };
    clearHistory();
    bool paused = false, effectivePaused = false, rewindLatched = false, rewinding = false;
    auto rewindStatus = [&](bool enabled) {
        if (rewinding != enabled) { rewinding = enabled; emit rewindingChanged(enabled); }
    };
    auto nowNs = [] { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); };
    qint64 rewindTotalMs = 0, rewindLastProgressMs = 0, nextRewindProgressNs = 0;
    double rewindStepMs = 0;
    auto rewindAvailableMs = [&] {
        const size_t checkpoints = history.size();
        return qint64(std::llround(double(checkpoints ? checkpoints-1 : 0) * rewindStepMs));
    };
    FramePacer pacer;
    FrameWaiter waiter;
    pacer.reset(nowNs());
    auto clearAudio = [&] { audioClockAvailable_.store(false); emit audioReset(); };
    auto lastBatteryCheck = Clock::now();
    bool optionsPending = false, fastLatched = false;
    qint64 nextPresentationNs = 0;
    while (!quitting_.load()) {
        std::deque<Command> pending;
        { std::lock_guard<std::mutex> lock(mutex_); pending.swap(commands_); }
        bool frameRequested = false;
        for (const auto& command : pending) {
            QString error;
            switch (command.type) {
            case Type::Load:
                if (host.load(command.path,error,command.value,
                              command.archiveEntry ? &*command.archiveEntry : nullptr)) {
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
                loaded_.store(host.loaded()); discStatus(); cheatStatus();
                break;
            case Type::FrameAdvance:
                if (host.loaded() && paused && !backgroundPaused_.load() && !rewind_.load()) frameRequested = true;
                break;
            case Type::Cheats: {
                bool changed = false;
                if (!host.setCheats(command.cheats,error,&changed)) emit errorOccurred(error);
                else if (changed) { clearHistory(); clearAudio(); rewind_.store(false); rewindStatus(false); pacer.reset(nowNs()); }
                cheatStatus();
                break;
            }
            case Type::ControllerKind:
                if (host.setControllerKind(unsigned(command.value & 1),ControllerType(command.value >> 1))) {
                    clearHistory(); clearAudio(); rewind_.store(false); rewindStatus(false); pacer.reset(nowNs());
                }
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
                    loaded_.store(host.loaded()); discStatus();
                }
                break;
            case Type::Disc:
            case Type::DiscIndex:
            case Type::Eject: {
                const bool ok = command.type == Type::Disc ? host.changeDisc(command.path,error) :
                    command.type == Type::DiscIndex ? host.selectDisc(command.value,error) :
                    host.setDiscEjected(command.value != 0,error);
                // Failed mounts can also disturb the drive; do not retain an
                // older timeline across any attempted media operation.
                clearHistory(); clearAudio(); rewindStatus(false); pacer.reset(nowNs());
                discStatus(); cheatStatus(); emit stateSlotsChanged(-1,host.occupiedStateSlots());
                if (ok) emit actionCompleted(command.type == Type::Eject ?
                    (command.value ? QStringLiteral("Привод открыт") : QStringLiteral("Привод закрыт")) :
                    QStringLiteral("Диск: %1").arg(QFileInfo(host.discPaths().value(host.discIndex())).completeBaseName()));
                else emit errorOccurred(error);
                break;
            }
            case Type::Port:
                if (host.setControllerConnected(unsigned(command.value & 1), (command.value & 2) != 0)) {
                    clearHistory(); clearAudio(); rewind_.store(false); rewindStatus(false);
                    pacer.reset(nowNs());
                }
                break;
            case Type::ControllerType:
                if (host.setControllerSixButton(unsigned(command.value & 1), (command.value & 2) != 0)) {
                    clearHistory(); clearAudio(); rewind_.store(false); rewindStatus(false);
                    pacer.reset(nowNs());
                }
                break;
            case Type::SpriteLimit:
                if (host.setRemoveSpriteLimit(command.value != 0,error)) {
                    optionsPending = true; clearHistory();
                } else emit errorOccurred(error);
                break;
            case Type::CdLoading:
                if (host.setFastCdLoading(command.value != 0,error)) {
                    if (host.isCd()) { optionsPending = true; clearHistory(); }
                } else emit errorOccurred(error);
                break;
            case Type::Checksum:
                // Update the policy only. load() and an explicit hard reset
                // rebuild the in-memory ROM with this setting.
                if (!host.setAutoFixChecksum(command.value != 0,error))
                    emit errorOccurred(error);
                break;
            case Type::Region:
            case Type::Filter:
                if (command.type == Type::Region ? host.setRegion(command.value,error)
                                                 : host.setBlarggFilter(command.value,error)) {
                    if (command.type == Type::Region && host.isCd()) {
                        if (!host.hardReset(error)) emit errorOccurred(error);
                        loaded_.store(host.loaded()); discStatus();
                    }
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
        const bool requestedHistoryEnabled = rewindEnabled_.load();
        const int requestedHistoryMemoryMiB = rewindMemoryMiB_.load();
        if (historyEnabled != requestedHistoryEnabled || historyMemoryMiB != requestedHistoryMemoryMiB) {
            historyEnabled = requestedHistoryEnabled; historyMemoryMiB = requestedHistoryMemoryMiB;
            clearHistory();
        }
        const bool background = backgroundPaused_.load();
        const bool nowPaused = host.loaded() && (paused || background);
        if (nowPaused != effectivePaused) {
            effectivePaused = nowPaused; emit effectivePauseChanged(nowPaused); clearAudio(); pacer.reset(nowNs());
        }
        const bool reverse = rewind_.load() && historyEnabled && host.loaded() && !background && !optionsPending;
        if (reverse != rewindLatched) {
            rewindLatched = reverse; clearAudio(); pacer.reset(nowNs());
            if (reverse) {
                // Checkpoints are recorded every two emulated frames. Freeze
                // this hold's origin while the remaining step count decreases.
                rewindStepMs = 2000.0 / host.fps();
                rewindTotalMs = rewindAvailableMs();
                rewindLastProgressMs = rewindTotalMs;
                nextRewindProgressNs = nowNs() + 100000000;
                rewindStatus(true);
                emit rewindProgress(rewindTotalMs,rewindTotalMs);
            } else rewindStatus(false);
        }
        if (reverse) {
            rewindStatus(true);
            QByteArray state;
            QImage frame;
            double aspect = 4.0 / 3.0;
            if (history.stepBack(state,frame,aspect)) {
                if (host.restoreRewindState(state)) {
                    captureFrame = 0; publishVideo(frame,aspect);
                } else {
                    clearHistory(); historyHealthy = false;
                    emit errorOccurred(QStringLiteral("Ядро отклонило состояние перемотки. История очищена."));
                }
            }
            const qint64 remainingMs = std::min(rewindTotalMs,rewindAvailableMs());
            const qint64 progressNow = nowNs();
            if (remainingMs != rewindLastProgressMs && progressNow >= nextRewindProgressNs) {
                rewindLastProgressMs = remainingMs;
                nextRewindProgressNs = progressNow + 100000000;
                emit rewindProgress(remainingMs,rewindTotalMs);
            }
            // Hold at the oldest checkpoint rather than inadvertently running forward.
            const auto deadline = pacer.next(nowNs(),qint64(2000000000.0 / host.fps()));
            waiter.waitUntil(deadline);
            continue;
        }
        const auto batteryNow = Clock::now();
        if (host.loaded() && !reverse &&
            batteryNow - lastBatteryCheck >= std::chrono::seconds(1)) {
            QString error;
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                batteryNow.time_since_epoch()).count();
            if (!host.pollSaveRam(milliseconds, error)) emit notification(error);
            lastBatteryCheck = batteryNow;
        }
        if (frameRequested && host.loaded() && paused && !background) {
            syncInput();
            host.runFrame(true,false); // Exactly one video frame; no burst of queued sound.
            if (host.isCd() && host.discEjected() != lastDiscEjected) { clearHistory(); discStatus(); cheatStatus(); }
            if (optionsPending) { optionsPending = false; clearHistory(); }
            if (historyEnabled && historyHealthy && ++captureFrame % 2 == 0 &&
                !history.capture(host.captureRewindState(),host.frame(),host.aspectRatio())) {
                history.clear(); historyHealthy = false;
            }
            publishVideo(host.frame(),host.aspectRatio());
            clearAudio(); pacer.reset(nowNs());
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
        syncInput();
        // Fast-forward omits unseen rendering, PCM copies and rewind recording.
        // Emulated CPU/audio state still advances normally.
        host.runFrame(!fast || present || optionsPending, !fast);
        if (host.isCd() && host.discEjected() != lastDiscEjected) {
            clearHistory(); discStatus();
        }
        if (optionsPending) {
            optionsPending = false; clearHistory(); clearAudio(); pacer.reset(nowNs());
        }
        if (!fast && historyEnabled && historyHealthy && ++captureFrame % 2 == 0) {
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
