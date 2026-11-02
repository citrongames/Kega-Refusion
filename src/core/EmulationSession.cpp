#include "i18n/Localization.h"
#include "EmulationSession.h"
#include "LibretroHost.h"
#include "MovieSession.h"
#include "AchievementsClient.h"
#include <QtEndian>
#include "RewindBuffer.h"
#include "FramePacer.h"
#include "PerformanceMetrics.h"
#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

EmulationSession::EmulationSession(QString corePath, QString dataPath, QObject* parent)
    : QThread(parent), corePath_(std::move(corePath)), dataPath_(std::move(dataPath)) {
    qRegisterMetaType<CheatList>(); qRegisterMetaType<AchievementsState>();
    qRegisterMetaType<AchievementNotice>(); qRegisterMetaType<AchievementHttpRequest>(); start();
}
EmulationSession::~EmulationSession() { shutdown(); }
void EmulationSession::enqueue(Command command) {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back(std::move(command));
}
void EmulationSession::loadGame(const QString& path, int archiveIndex) { enqueue({Type::Load,path,archiveIndex}); }
void EmulationSession::loadGame(const QString& path, const RomArchive::Entry& archiveEntry) {
    enqueue({Type::Load,path,archiveEntry.index,archiveEntry});
}
void EmulationSession::closeGame() { enqueue({Type::Close,{}}); }
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
    if (port < 3) enqueue({Type::Port,{},int(port) | (connected ? 4 : 0)});
}
void EmulationSession::setControllerSixButton(unsigned port, bool sixButton) {
    if (port < 3) enqueue({Type::ControllerType,{},int(port) | (sixButton ? 4 : 0)});
}
void EmulationSession::setControllerKind(unsigned player, ControllerType type) {
    if (player < 3 && int(type) >= 0 && int(type) <= int(ControllerType::Justifier))
        enqueue({Type::ControllerKind,{},int(player) | (int(type) << 2)});
}
void EmulationSession::setGunAim(unsigned player, int x, int y, bool offscreen) {
    if (player >= 3) return;
    const quint64 packed = quint16(std::clamp(x,-32767,32767)) |
        (quint64(quint16(std::clamp(y,-32767,32767))) << 16) | (quint64(offscreen) << 32);
    gunPositions_[player].store(packed);
}
void EmulationSession::advanceFrame() { enqueue({Type::FrameAdvance,{}}); }
void EmulationSession::setFrameAdvanceHeld(bool held) { frameAdvanceHeld_.store(held); }
void EmulationSession::startMovieRecording(const QString& path) { enqueue({Type::MovieRecord,path}); }
void EmulationSession::playMovie(const QString& path) { enqueue({Type::MoviePlay,path}); }
void EmulationSession::resumeMovieRecording(const QString& path) { enqueue({Type::MovieResume,path}); }
void EmulationSession::rerecordMovie(const QString& path) { enqueue({Type::MovieFork,path}); }
void EmulationSession::takeOverMovie() { enqueue({Type::MovieTakeOver,{}}); }
void EmulationSession::stopMovie() { enqueue({Type::MovieStop,{}}); }
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
    if (port >= 3 || id >= 16) return;
    if (pressed) buttons_[port].fetch_or(1u << id); else buttons_[port].fetch_and(~(1u << id));
}
void EmulationSession::setButtons(unsigned port, unsigned mask) { if (port < 3) buttons_[port].store(mask & 0xffffu); }
void EmulationSession::releaseButtons() { for (auto& mask : buttons_) mask.store(0); }
void EmulationSession::setFastForward(bool enabled) { fastForward_.store(enabled); }
void EmulationSession::setFastForwardFactor(int factor) { fastForwardFactor_.store(factor == 0 ? 0 : std::clamp(factor, 2, 32)); }
void EmulationSession::setRewind(bool enabled) { rewind_.store(enabled); }
void EmulationSession::setRewindEnabled(bool enabled) { rewindEnabled_.store(enabled); }
void EmulationSession::setRewindMemoryMiB(int mebibytes) { rewindMemoryMiB_.store(std::clamp(mebibytes,16,1024)); }
void EmulationSession::setBackgroundPaused(bool paused) { backgroundPaused_.store(paused); }
void EmulationSession::setPerformanceMetricsEnabled(bool enabled) { enqueue({Type::PerformanceMetrics,{},enabled ? 1 : 0}); }
void EmulationSession::setGameFpsEnabled(bool enabled) { enqueue({Type::GameFps,{},enabled ? 1 : 0}); }
void EmulationSession::setExpansionDevices(bool cd, bool x32) { enqueue({Type::Expansions,{},(cd ? 1 : 0) | (x32 ? 2 : 0)}); }
void EmulationSession::requestSlotStatus(int selected) { enqueue({Type::Slots,{},selected}); }
void EmulationSession::configureAchievements(bool enabled, bool hardcore) {
    enqueue({Type::AchievementsConfigure,{},(enabled ? 1 : 0) | (hardcore ? 2 : 0)});
}
void EmulationSession::loginAchievements(QString user, QString password) {
    Command command{Type::AchievementsLogin,std::move(user)};
    command.bytes = password.toUtf8(); enqueue(std::move(command));
}
void EmulationSession::logoutAchievements() { enqueue({Type::AchievementsLogout,{}}); }
void EmulationSession::requestAchievements() { enqueue({Type::AchievementsRefresh,{}}); }
void EmulationSession::achievementResponse(quint64 id, int status, QByteArray body) {
    if (body.size() > 8*1024*1024) { body.clear(); status = 0; }
    Command command{Type::AchievementsResponse,{},status};
    command.requestId = id; command.bytes = std::move(body); enqueue(std::move(command));
}
void EmulationSession::shutdown() { quitting_.store(true); if (isRunning()) wait(); }
void EmulationSession::run() {
    using Clock = std::chrono::steady_clock;
    LibretroHost host(corePath_,dataPath_);
    host.message = [this](const QString& message) { emit notification(message); };
    MovieSession movie(host, dataPath_);
    AchievementsState achievementState;
    bool achievementEnabled = false, achievementHardcoreRequested = false;
    bool achievementHardcore = false, achievementResetRequested = false;
    bool achievementPolicyRejected = false;
    bool achievementSuppressed = false;
    QString achievementStatusOverride;
    AchievementsClient achievements(dataPath_, {
        [&](uint32_t address, uint8_t* buffer, uint32_t count) {
            return host.readAchievementMemory(address,buffer,count);
        },
        [this](const AchievementHttpRequest& request) { emit achievementHttpRequest(request); },
        [&](const AchievementsState& state) {
            achievementState = state;
            if (!achievementStatusOverride.isEmpty()) achievementState.status = achievementStatusOverride;
            if (achievementSuppressed) achievementState.status = Localization::text(QStringLiteral("Учёт достижений отключён для воспроизведённой записи управления."));
            emit achievementsChanged(achievementState);
            const bool hardcore = state.enabled && state.loggedIn && state.hardcore && host.loaded() && !achievementSuppressed;
            if (hardcore && !host.achievementHardcoreBlockReason().isEmpty()) {
                achievementPolicyRejected = true;
                achievementStatusOverride = Localization::text(QStringLiteral("Hardcore недоступен: %1")).arg(host.achievementHardcoreBlockReason());
            }
            if (hardcore != achievementHardcore) {
                if (hardcore && !achievementPolicyRejected) achievementResetRequested = true;
                achievementHardcore = hardcore;
                emit achievementsHardcoreChanged(hardcore);
            }
        },
        [this](const AchievementNotice& notice) { emit achievementNotice(notice); },
        [&] { achievementResetRequested = true; }
    });
    host.captureAchievementProgress = [&] {
        return achievementSuppressed ? QByteArray() : achievements.saveProgress();
    };
    host.restoreAchievementProgress = [&](const QByteArray& progress) { achievements.restoreProgress(progress); };
    const auto loadAchievements = [&](bool changedMedia = false) {
        if (!achievementEnabled || !host.loaded()) { achievements.unloadGame(); achievements.refresh(); return; }
        if (!achievements.wantsGame()) return;
        QString hashError;
        const auto hash = host.achievementHash(hashError);
        if (hash.isEmpty()) {
            achievements.unloadGame(); achievementStatusOverride = hashError; achievements.refresh(); return;
        }
        if (changedMedia) achievements.changeMedia(hash,host.achievementConsoleId());
        else achievements.loadGame(hash,host.achievementConsoleId());
        achievements.refresh();
    };
    const auto configureAchievements = [&] {
        achievementStatusOverride.clear();
        const auto blocked = achievementHardcoreRequested && host.loaded()
            ? host.achievementHardcoreBlockReason() : QString();
        if (!blocked.isEmpty()) achievementStatusOverride = Localization::text(QStringLiteral("Hardcore недоступен: %1")).arg(blocked);
        if (achievementSuppressed && achievementHardcoreRequested)
            achievementStatusOverride = Localization::text(QStringLiteral("Завершите запись управления перед включением Hardcore."));
        if (achievementHardcoreRequested && !AchievementsHardcoreApproved)
            achievementStatusOverride = Localization::text(QStringLiteral("Hardcore требует одобрения Kega Refusion сервисом RetroAchievements. Учёт работает в Softcore."));
        achievements.configure(achievementEnabled,achievementHardcoreRequested && AchievementsHardcoreApproved &&
            blocked.isEmpty() && !achievementSuppressed);
        achievements.refresh();
    };
    auto nextAchievementIdle = Clock::now();
    bool lastAchievementBackground = false;
    auto liveInput = [&] {
        MovieInput input;
        for (unsigned player = 0; player < 3; ++player) {
            input.buttons[player] = quint16(buttons_[player].load());
            input.aim[player] = gunPositions_[player].load();
        }
        return input;
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
    PerformanceMetrics metrics;
    metrics.reset(nowNs());
    bool performanceMetricsEnabled = false, gameFpsEnabled = false;
    bool metricsWindowRunning = false;
    auto resetMetrics = [&] {
        metrics.reset(nowNs());
        if (performanceMetricsEnabled || gameFpsEnabled)
            emit performanceMetricsChanged(0.0, 0.0, 0.0, false);
    };
    FramePacer pacer;
    FrameWaiter waiter;
    pacer.reset(nowNs());
    auto clearAudio = [&] { audioClockAvailable_.store(false); emit audioReset(); };
    auto lastBatteryCheck = Clock::now();
    bool optionsPending = false, fastLatched = false;
    qint64 nextPresentationNs = 0;
    qint64 nextMovieStatusNs = 0, nextHeldFrameNs = 0;
    bool stepHeldLatched = false;
    auto slotStatus = [&](int selected = -1) {
        emit stateSlotsChanged(selected, movie.active() ? movie.occupiedSlots() : host.occupiedStateSlots());
    };
    auto movieStatus = [&](bool force = false) {
        if (force || (movie.active() && nowNs() >= nextMovieStatusNs)) {
            emit movieStatusChanged(int(movie.mode()), movie.path(), movie.position(), movie.length(), movie.frameRate());
            nextMovieStatusNs = nowNs() + 100000000;
        }
    };
    auto movieBoundary = [&] {
        emit movieHardwareChanged(movie.controllers());
        movieStatus(true); slotStatus();
        clearHistory(); clearAudio(); rewind_.store(false); rewindStatus(false);
        frameAdvanceHeld_.store(false); stepHeldLatched = false;
        pacer.reset(nowNs()); nextPresentationNs = 0;
        resetMetrics();
    };
    auto beforeFrame = [&] {
        QString error;
        if (movie.beforeFrame(liveInput(), error)) return true;
        paused = true; frameAdvanceHeld_.store(false);
        clearAudio(); emit pausedChanged(true);
        if (movie.recording() && !error.isEmpty()) {
            QString saveError;
            if (movie.finish(false,saveError)) {
                achievementSuppressed = false; achievements.reset(); achievements.refresh();
                movieBoundary();
                error += Localization::text(QStringLiteral("\nЗаписанная часть записи сохранена; игра оставлена на паузе."));
            } else error += QStringLiteral("\n") + saveError;
        }
        if (!error.isEmpty()) emit errorOccurred(error);
        return false;
    };
    auto afterFrame = [&] {
        movie.afterFrame();
        if (movie.mode() == MovieSession::Finished) {
            paused = true; frameAdvanceHeld_.store(false);
            clearAudio(); emit pausedChanged(true); movieStatus(true);
            emit actionNotice(Localization::text(QStringLiteral("Воспроизведение завершено")));
        } else movieStatus();
    };
    auto captureHistory = [&] {
        const auto state = movie.active() ? movie.captureRewind() : host.captureRewindState();
        const auto progress = achievementSuppressed ? QByteArray() : achievements.saveProgress(false);
        if (progress.isEmpty() || state.isEmpty()) return state;
        QByteArray wrapped("KRACHRW1",8);
        for (quint32 length : {quint32(state.size()),quint32(progress.size())}) {
            const auto encoded = qToBigEndian(length);
            wrapped.append(reinterpret_cast<const char*>(&encoded),4);
        }
        wrapped += state; wrapped += progress;
        return wrapped;
    };
    auto restoreHistory = [&](const QByteArray& packed, QString& error) {
        QByteArray state = packed, progress;
        if (packed.startsWith("KRACHRW1")) {
            if (packed.size() < 16) return false;
            const auto stateSize = qFromBigEndian<quint32>(packed.constData()+8);
            const auto progressSize = qFromBigEndian<quint32>(packed.constData()+12);
            if (progressSize > 2*1024*1024 || quint64(stateSize)+progressSize+16 != quint64(packed.size())) return false;
            state = packed.mid(16,stateSize); progress = packed.mid(16+stateSize,progressSize);
        }
        const bool ok = movie.active() ? movie.restoreRewind(state,error) : host.restoreRewindState(state);
        if (ok) achievements.restoreProgress(progress,true);
        return ok;
    };
    while (!quitting_.load()) {
        std::deque<Command> pending;
        { std::lock_guard<std::mutex> lock(mutex_); pending.swap(commands_); }
        bool frameRequested = false;
        for (const auto& command : pending) {
            QString error;
            if (achievementHardcore) {
                const bool prohibited = command.type == Type::Restore || command.type == Type::FrameAdvance ||
                    command.type == Type::MoviePlay || command.type == Type::MovieResume || command.type == Type::MovieFork ||
                    command.type == Type::Cheats || command.type == Type::Region ||
                    (command.type == Type::Checksum && command.value) ||
                    (command.type == Type::CdLoading && command.value) ||
                    (command.type == Type::SpriteLimit && command.value);
                if (prohibited) {
                    achievementStatusOverride = Localization::text(QStringLiteral("Это действие недоступно в Hardcore. Переключите режим в настройках достижений."));
                    achievements.refresh(); continue;
                }
            }
            if (movie.active()) {
                // Recorded hardware/logic is fixed; pause, speed, input bindings and rewind remain live.
                bool blocked = false;
                switch (command.type) {
                case Type::Port:
                    blocked = host.controllerConnected(unsigned(command.value & 3)) != bool(command.value & 4);
                    if (!blocked) continue;
                    break;
                case Type::ControllerType:
                    blocked = host.controllerSixButton(unsigned(command.value & 3)) != bool(command.value & 4);
                    if (!blocked) continue;
                    break;
                case Type::ControllerKind:
                    blocked = host.controllerKind(unsigned(command.value & 3)) != ControllerType(command.value >> 2);
                    if (!blocked) continue;
                    break;
                case Type::Reset: case Type::HardReset: case Type::Region: case Type::Filter:
                case Type::CdLoading: case Type::SpriteLimit: case Type::Checksum: case Type::Cheats:
                case Type::Disc: case Type::DiscIndex: case Type::Eject: blocked = true; break;
                default: break;
                }
                if (blocked) {
                    emit actionNotice(Localization::text(QStringLiteral("Сначала завершите запись: параметры консоли зафиксированы в записи.")));
                    continue;
                }
            }
            switch (command.type) {
            case Type::PerformanceMetrics:
                if (performanceMetricsEnabled != (command.value != 0)) {
                    performanceMetricsEnabled = command.value != 0;
                    resetMetrics();
                }
                break;
            case Type::GameFps:
                if (gameFpsEnabled != (command.value != 0)) {
                    gameFpsEnabled = command.value != 0;
                    resetMetrics();
                }
                break;
            case Type::Expansions:
                host.setExpansionDevices(command.value & 1, command.value & 2);
                break;
            case Type::AchievementsConfigure:
                achievementEnabled = bool(command.value & 1);
                achievementHardcoreRequested = bool(command.value & 2);
                configureAchievements();
                if (host.loaded()) loadAchievements();
                clearHistory();
                break;
            case Type::AchievementsLogin:
                achievementStatusOverride.clear();
                achievements.login(command.path,QString::fromUtf8(command.bytes));
                if (host.loaded()) loadAchievements();
                break;
            case Type::AchievementsLogout:
                achievementStatusOverride.clear(); achievements.logout(); clearHistory();
                break;
            case Type::AchievementsRefresh: achievements.refresh(); break;
            case Type::AchievementsResponse:
                achievements.respond(command.requestId,command.value,command.bytes);
                break;
            case Type::MovieRecord:
            case Type::MoviePlay:
            case Type::MovieResume:
            case Type::MovieFork: {
                const bool ok = command.type == Type::MovieRecord ? movie.startRecording(command.path,error)
                    : command.type == Type::MovieFork ? movie.rerecord(command.path,error)
                    : movie.play(command.path,command.type == Type::MovieResume,error);
                loaded_.store(host.loaded());
                if (!ok) {
                    if (!host.loaded()) emit gameClosed();
                    emit errorOccurred(error); break;
                }
                achievementSuppressed = command.type != Type::MovieRecord;
                achievements.reset(); achievements.refresh();
                releaseButtons(); movieBoundary(); optionsPending = false;
                if ((command.type == Type::MoviePlay || command.type == Type::MovieResume) &&
                    !host.movieCompatibilityWarning().isEmpty()) {
                    emit movieCompatibilityWarning(host.movieCompatibilityWarning());
                    emit notification(host.movieCompatibilityWarning());
                }
                paused = movie.mode() == MovieSession::Finished;
                emit pausedChanged(paused);
                break;
            }
            case Type::MovieStop:
            case Type::MovieTakeOver:
                if (movie.active()) {
                    if (!movie.finish(false,error)) { emit errorOccurred(error); break; }
                    achievementSuppressed = false; achievements.reset(); achievements.refresh();
                    movieBoundary(); cheatStatus(); discStatus();
                    if (command.type == Type::MovieTakeOver) { paused = false; emit pausedChanged(false); }
                } else movieStatus(true); // Acknowledge stop after a failed pending start (window-close handshake).
                break;
            case Type::Load:
                if (movie.active()) {
                    if (!movie.finish(true,error)) { emit errorOccurred(error); break; }
                    movieBoundary();
                }
                if (host.load(command.path,error,command.value,
                              command.archiveEntry ? &*command.archiveEntry : nullptr)) {
                    loaded_.store(true); releaseButtons(); paused = false;
                    achievementSuppressed = false; configureAchievements(); loadAchievements();
                    rewind_.store(false); rewindStatus(false); clearHistory();
                    clearAudio();
                    resetMetrics();
                    emit gameLoaded(host.gamePath(),host.fps(),host.contentName());
                    emit stateSlotsChanged(-1,host.occupiedStateSlots());
                    emit pausedChanged(false);
                    pacer.reset(nowNs());
                } else {
                    if (!host.loaded()) { clearHistory(); rewindStatus(false); }
                    emit errorOccurred(error);
                }
                loaded_.store(host.loaded());
                if (!host.loaded()) { achievements.unloadGame(); achievements.refresh(); }
                discStatus(); cheatStatus();
                break;
            case Type::Close:
                if (movie.active()) {
                    if (!movie.finish(true,error)) { emit errorOccurred(error); break; }
                    movieBoundary();
                }
                // Keep the running game intact if a changed battery save cannot be written.
                if (host.loaded() && !host.flushSaveRam(error)) {
                    emit errorOccurred(error);
                    break;
                }
                achievements.unloadGame(); achievementSuppressed = false;
                host.unload(); achievements.refresh();
                loaded_.store(false);
                releaseButtons();
                for (auto& aim : gunPositions_) aim.store(quint64(1) << 32);
                paused = false;
                fastForward_.store(false);
                rewind_.store(false);
                rewindLatched = false;
                fastLatched = false;
                clearHistory();
                rewindStatus(false);
                rewindTotalMs = rewindLastProgressMs = nextRewindProgressNs = 0;
                rewindStepMs = 0;
                emit rewindProgress(0,0);
                optionsPending = false;
                frameRequested = false;
                nextPresentationNs = 0;
                {
                    std::lock_guard<std::mutex> lock(videoMutex_);
                    latestFrame_ = QImage();
                    latestAspect_ = 4.0 / 3.0;
                    videoPending_ = false;
                }
                clearAudio();
                emit pausedChanged(false);
                effectivePaused = false;
                emit effectivePauseChanged(false);
                emit stateSlotsChanged(-1,0);
                discStatus();
                cheatStatus();
                lastBatteryCheck = Clock::now();
                pacer.reset(nowNs()); resetMetrics();
                emit gameClosed();
                break;
            case Type::FrameAdvance:
                if (movie.mode() == MovieSession::Finished) {
                    if (!movie.finish(false,error)) { emit errorOccurred(error); break; }
                    achievementSuppressed = false; achievements.reset(); achievements.refresh();
                    movieBoundary(); cheatStatus();
                }
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
                if (host.setControllerKind(unsigned(command.value & 3),ControllerType(command.value >> 2))) {
                    clearHistory(); clearAudio(); rewind_.store(false); rewindStatus(false); pacer.reset(nowNs());
                }
                break;
            case Type::Pause:
                if (command.value && achievementHardcore && !achievements.canPause()) {
                    achievementStatusOverride = Localization::text(QStringLiteral("Частые паузы: Hardcore отключён для этой сессии."));
                    achievements.configure(achievementEnabled,false); achievements.refresh();
                }
                if (!command.value && movie.mode() == MovieSession::Finished) {
                    if (!movie.finish(false,error)) { emit errorOccurred(error); break; }
                    achievementSuppressed = false; achievements.reset(); achievements.refresh();
                    movieBoundary(); cheatStatus();
                }
                paused = command.value != 0;
                clearAudio();
                emit pausedChanged(paused);
                pacer.reset(nowNs());
                break;
            case Type::Reset:
                if (host.loaded()) {
                    host.reset(); achievements.reset(); clearHistory(); clearAudio(); resetMetrics();
                    emit actionCompleted(Localization::text(QStringLiteral("Сброс")));
                    pacer.reset(nowNs());
                }
                break;
            case Type::HardReset:
                if (host.loaded()) {
                    if (host.hardReset(error)) {
                        achievements.reset();
                        clearHistory(); clearAudio(); rewindStatus(false); resetMetrics();
                        emit actionCompleted(Localization::text(QStringLiteral("Перезапуск питания")));
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
                if (ok && command.type != Type::Eject) loadAchievements(true);
                if (ok) emit actionCompleted(command.type == Type::Eject ?
                    (command.value ? Localization::text(QStringLiteral("Привод открыт")) : Localization::text(QStringLiteral("Привод закрыт"))) :
                    Localization::text(QStringLiteral("Диск: %1")).arg(QFileInfo(host.discPaths().value(host.discIndex())).completeBaseName()));
                else emit errorOccurred(error);
                break;
            }
            case Type::Port:
                if (host.setControllerConnected(unsigned(command.value & 3), (command.value & 4) != 0)) {
                    clearHistory(); clearAudio(); rewind_.store(false); rewindStatus(false);
                    pacer.reset(nowNs());
                }
                break;
            case Type::ControllerType:
                if (host.setControllerSixButton(unsigned(command.value & 3), (command.value & 4) != 0)) {
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
                        achievements.reset();
                        loaded_.store(host.loaded()); discStatus();
                    }
                    optionsPending = true; clearHistory(); clearAudio();
                } else emit errorOccurred(error);
                break;
            case Type::Save:
                if (movie.active() ? movie.saveSlot(command.value,error) : host.saveState(command.value,error)) {
                    slotStatus();
                    emit actionCompleted(Localization::text(QStringLiteral("Сохранено: слот %1")).arg(command.value));
                } else emit errorOccurred(error);
                break;
            case Type::Restore: {
                if (movie.active()) {
                    bool missing = false; QImage frame; double aspect = host.aspectRatio();
                    if (movie.loadSlot(command.value,frame,aspect,missing,error)) {
                        achievements.reset();
                        clearHistory(); clearAudio(); resetMetrics();
                        if (!frame.isNull()) publishVideo(frame,aspect);
                        movieStatus(true);
                        emit actionCompleted(Localization::text(QStringLiteral("Загружено: слот %1")).arg(command.value));
                        pacer.reset(nowNs());
                    } else emit actionNotice(error);
                } else {
                    LibretroHost::StateLoadError failure;
                    if (host.loadState(command.value,error,&failure)) {
                        clearHistory(); clearAudio(); resetMetrics();
                        emit actionCompleted(Localization::text(QStringLiteral("Загружено: слот %1")).arg(command.value));
                        pacer.reset(nowNs());
                    } else if (failure == LibretroHost::StateLoadError::Missing) emit actionNotice(error);
                    else emit errorOccurred(error);
                }
                slotStatus();
                break;
            }
            case Type::Slots: slotStatus(command.value); break;
            }
        }
        if (Clock::now() >= nextAchievementIdle) {
            achievements.idle(); nextAchievementIdle = Clock::now() + std::chrono::milliseconds(100);
        }
        if (achievementPolicyRejected) {
            achievementPolicyRejected = false; achievementResetRequested = false;
            achievements.configure(achievementEnabled,false); achievements.refresh();
        }
        if (achievementResetRequested) {
            achievementResetRequested = false;
            QString resetError;
            const bool hadGame = host.loaded();
            if (hadGame && host.hardReset(resetError)) {
                achievements.reset(); clearHistory(); clearAudio(); pacer.reset(nowNs()); resetMetrics();
            } else if (hadGame) {
                achievementStatusOverride = resetError;
                achievements.configure(achievementEnabled,false); achievements.refresh();
                loaded_.store(host.loaded());
                if (!host.loaded()) { achievements.unloadGame(); emit gameClosed(); }
            }
        }
        const bool requestedHistoryEnabled = rewindEnabled_.load() && !achievementHardcore;
        const int requestedHistoryMemoryMiB = rewindMemoryMiB_.load();
        if (historyEnabled != requestedHistoryEnabled || historyMemoryMiB != requestedHistoryMemoryMiB) {
            historyEnabled = requestedHistoryEnabled; historyMemoryMiB = requestedHistoryMemoryMiB;
            clearHistory();
        }
        const bool background = backgroundPaused_.load();
        if (background && !lastAchievementBackground && achievementHardcore && !achievements.canPause()) {
            achievementStatusOverride = Localization::text(QStringLiteral("Частые паузы: Hardcore отключён для этой сессии."));
            achievements.configure(achievementEnabled,false); achievements.refresh();
        }
        lastAchievementBackground = background;
        const bool achievementLoading = achievementEnabled && achievements.loading();
        const bool nowPaused = host.loaded() && (paused || background || achievementLoading);
        if (nowPaused != effectivePaused) {
            effectivePaused = nowPaused; emit effectivePauseChanged(nowPaused); clearAudio(); pacer.reset(nowNs());
        }
        const bool reverse = rewind_.load() && historyEnabled && host.loaded() && !background && !achievementLoading && !achievementHardcore && !optionsPending;
        const bool metricsRunning = (performanceMetricsEnabled || gameFpsEnabled) &&
            host.loaded() && !nowPaused && !reverse;
        if (metricsRunning != metricsWindowRunning) {
            metricsWindowRunning = metricsRunning;
            resetMetrics();
        }
        if (metricsRunning) {
            PerformanceMetrics::Snapshot sample;
            if (metrics.takeSample(nowNs(), host.fps(), gameFpsEnabled && host.supportsInputPollMetrics(), sample))
                emit performanceMetricsChanged(sample.emulatedFps, sample.speedPercent,
                                               sample.gameFps, sample.gameFpsAvailable);
        }
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
                QString restoreError;
                if (restoreHistory(state,restoreError)) {
                    captureFrame = 0; publishVideo(frame,aspect); movieStatus();
                } else {
                    clearHistory(); historyHealthy = false;
                    emit errorOccurred(Localization::text(QStringLiteral("Ядро отклонило состояние перемотки. История очищена.")));
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
        const bool stepHeld = frameAdvanceHeld_.load() && host.loaded() && paused && !background && !achievementLoading && !achievementHardcore;
        if (stepHeld && !stepHeldLatched) nextHeldFrameNs = nowNs() + 350000000;
        if (stepHeld && stepHeldLatched && nowNs() >= nextHeldFrameNs) {
            frameRequested = true; nextHeldFrameNs = nowNs() + 33333333;
        }
        stepHeldLatched = stepHeld;
        if (frameRequested && host.loaded() && paused && !background && !achievementLoading && !achievementHardcore) {
            if (!beforeFrame()) continue;
            host.runFrame(true,false); // Exactly one video frame; no burst of queued sound.
            if (!achievementSuppressed) achievements.frame();
            afterFrame();
            if (host.isCd() && host.discEjected() != lastDiscEjected) { clearHistory(); discStatus(); cheatStatus(); }
            if (optionsPending) { optionsPending = false; clearHistory(); }
            if (historyEnabled && historyHealthy && ++captureFrame % 2 == 0 &&
                !history.capture(captureHistory(),host.frame(),host.aspectRatio())) {
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
            resetMetrics();
            // Fast-forward creates a gap: discard the old timeline so rewind
            // cannot jump across it when normal recording resumes.
            if (fast) clearHistory();
        }
        const int factor = fast ? fastForwardFactor_.load() : 1;
        const bool finalMovieFrame = movie.readOnly() && movie.position() + 1 >= movie.length();
        const bool present = !fast || finalMovieFrame || nowNs() >= nextPresentationNs;
        if (!beforeFrame()) continue;
        // Fast-forward omits unseen rendering, PCM copies and rewind recording.
        // Emulated CPU/audio state still advances normally.
        host.runFrame(!fast || present || optionsPending, !fast);
        if (metricsWindowRunning) metrics.recordFrame(gameFpsEnabled && host.frameInputPolled());
        if (!achievementSuppressed) achievements.frame();
        afterFrame();
        if (host.isCd() && host.discEjected() != lastDiscEjected) {
            clearHistory(); discStatus();
        }
        if (optionsPending) {
            optionsPending = false; clearHistory(); clearAudio(); pacer.reset(nowNs());
        }
        if (!fast && historyEnabled && historyHealthy && ++captureFrame % 2 == 0) {
            if (!history.capture(captureHistory(),host.frame(),host.aspectRatio())) {
                history.clear(); historyHealthy = false;
                emit notification(Localization::text(QStringLiteral("История перемотки недоступна для текущей игры.")));
            }
        }
        if (present) {
            publishVideo(host.frame(),host.aspectRatio());
            nextPresentationNs = nowNs() + 16666667; // Fast-forward display stays near60Hz, independent of emulation speed.
        }
        if (!fast && !paused && !host.audio().isEmpty()) emit audioSamples(host.audio(),host.sampleRate());
        const double clockScale = fast ? 1.0 : pacer.audioPeriodScale(nowNs(),audioFeedbackNs_.load(),
            audioQueueUs_.load(),audioTargetUs_.load(),audioClockAvailable_.load());
        if (factor != 0) {
            const auto deadline = pacer.next(nowNs(),qint64(1000000000.0 * clockScale / (host.fps() * factor)));
            waiter.waitUntil(deadline);
        } else pacer.reset(nowNs());
    }
    QString movieError;
    if (movie.active()) {
        if (!movie.finish(true,movieError)) emit errorOccurred(movieError);
    }
    achievements.unloadGame();
    host.captureAchievementProgress = {}; host.restoreAchievementProgress = {};
    host.unload();
    loaded_.store(false);
}
