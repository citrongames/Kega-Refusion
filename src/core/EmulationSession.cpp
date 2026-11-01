#include "EmulationSession.h"
#include "LibretroHost.h"
#include "RewindBuffer.h"
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
void EmulationSession::saveState(int slot) { enqueue({Type::Save,{},slot}); }
void EmulationSession::loadState(int slot) { enqueue({Type::Restore,{},slot}); }
void EmulationSession::setButton(unsigned port, unsigned id, bool pressed) {
    if (port >= 2 || id >= 16) return;
    if (pressed) buttons_[port].fetch_or(1u << id); else buttons_[port].fetch_and(~(1u << id));
}
void EmulationSession::setButtons(unsigned port, unsigned mask) { if (port < 2) buttons_[port].store(mask & 0xffffu); }
void EmulationSession::releaseButtons() { for (auto& mask : buttons_) mask.store(0); }
void EmulationSession::setFastForward(bool enabled) { fastForward_.store(enabled); }
void EmulationSession::setFastForwardFactor(int factor) { fastForwardFactor_.store(std::clamp(factor, 2, 8)); }
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
    auto deadline = Clock::now();
    auto lastFlush = deadline;
    unsigned fastFrame = 0;
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
                    emit audioReset();
                    emit gameLoaded(host.gamePath(),host.fps(),host.contentName());
                    emit stateSlotsChanged(-1,host.occupiedStateSlots());
                    emit pausedChanged(false);
                } else {
                    if (!host.loaded()) { clearHistory(); rewindStatus(false); }
                    emit errorOccurred(error);
                }
                loaded_.store(host.loaded());
                break;
            case Type::Pause:
                paused = command.value != 0;
                emit audioReset();
                emit pausedChanged(paused);
                break;
            case Type::Reset:
                if (host.loaded()) {
                    host.reset(); clearHistory(); emit audioReset();
                    emit actionCompleted(QStringLiteral("Сброс"));
                }
                break;
            case Type::Save:
                if (host.saveState(command.value,error)) {
                    emit stateSlotsChanged(-1,host.occupiedStateSlots());
                    emit actionCompleted(QStringLiteral("Сохранено: слот %1").arg(command.value));
                } else emit errorOccurred(error);
                break;
            case Type::Restore:
                if (host.loadState(command.value,error)) {
                    clearHistory(); emit audioReset();
                    emit actionCompleted(QStringLiteral("Загружено: слот %1").arg(command.value));
                } else emit errorOccurred(error);
                emit stateSlotsChanged(-1,host.occupiedStateSlots());
                break;
            case Type::Slots:
                emit stateSlotsChanged(command.value,host.occupiedStateSlots());
                break;
            }
            deadline = Clock::now();
        }
        if (historySeconds != rewindSeconds_.load()) {
            historySeconds = rewindSeconds_.load(); clearHistory();
        }
        const bool background = backgroundPaused_.load();
        const bool nowPaused = host.loaded() && (paused || background);
        if (nowPaused != effectivePaused) {
            effectivePaused = nowPaused; emit effectivePauseChanged(nowPaused); emit audioReset();
        }
        const bool reverse = rewind_.load() && historySeconds > 0 && host.loaded() && !background;
        if (reverse != rewindLatched) {
            rewindLatched = reverse; emit audioReset(); deadline = Clock::now();
            if (!reverse) rewindStatus(false);
        }
        if (reverse) {
            QByteArray state;
            QImage frame;
            double aspect = 4.0 / 3.0;
            if (history.stepBack(state,frame,aspect)) {
                if (host.restoreRewindState(state)) {
                    captureFrame = 0; rewindStatus(true); emit videoFrame(frame,aspect);
                } else {
                    clearHistory(); historyHealthy = false; rewindStatus(false);
                    emit errorOccurred(QStringLiteral("Ядро отклонило состояние перемотки. История очищена."));
                }
            } else rewindStatus(false);
            // Hold at the oldest checkpoint rather than inadvertently running forward.
            deadline += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(2.0 / host.fps()));
            const auto now = Clock::now();
            if (now - deadline > std::chrono::milliseconds(100)) deadline = now;
            std::this_thread::sleep_until(deadline);
            continue;
        }
        if (!host.loaded() || nowPaused) { std::this_thread::sleep_for(std::chrono::milliseconds(4)); continue; }
        for (unsigned port = 0; port < 2; ++port) host.setButtons(port,buttons_[port].load());
        host.runFrame();
        if (historySeconds && historyHealthy && ++captureFrame % 2 == 0) {
            if (!history.capture(host.captureRewindState(),host.frame(),host.aspectRatio())) {
                history.clear(); historyHealthy = false;
                emit notification(QStringLiteral("История перемотки недоступна для текущей игры."));
            }
        }
        const bool fast = fastForward_.load();
        const int factor = fast ? fastForwardFactor_.load() : 1;
        if (!fast || (++fastFrame % unsigned(factor) == 0)) emit videoFrame(host.frame(),host.aspectRatio());
        if (!fast && !host.audio().isEmpty()) emit audioSamples(host.audio(),host.sampleRate());
        const auto now = Clock::now();
        if (now - lastFlush > std::chrono::seconds(5)) {
            QString error;
            if (!host.flushSaveRam(error)) emit notification(error);
            lastFlush = now;
        }
        const auto frameTime = std::chrono::duration<double>(1.0 / (host.fps() * factor));
        deadline += std::chrono::duration_cast<Clock::duration>(frameTime);
        if (now - deadline > std::chrono::milliseconds(100)) deadline = now;
        std::this_thread::sleep_until(deadline);
    }
    host.unload();
    loaded_.store(false);
}
