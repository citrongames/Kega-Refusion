#include "EmulationSession.h"
#include "LibretroHost.h"
#include <chrono>
#include <thread>

EmulationSession::EmulationSession(QString corePath, QString dataPath, QObject* parent)
    : QThread(parent), corePath_(std::move(corePath)), dataPath_(std::move(dataPath)) { start(); }
EmulationSession::~EmulationSession() { shutdown(); }
void EmulationSession::enqueue(Command command) {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back(std::move(command));
}
void EmulationSession::loadGame(const QString& path) { enqueue({Type::Load,path}); }
void EmulationSession::setPaused(bool paused) { enqueue({Type::Pause,{},paused ? 1 : 0}); }
void EmulationSession::resetGame() { enqueue({Type::Reset,{}}); }
void EmulationSession::saveState(int slot) { enqueue({Type::Save,{},slot}); }
void EmulationSession::loadState(int slot) { enqueue({Type::Restore,{},slot}); }
void EmulationSession::setButton(unsigned port, unsigned id, bool pressed) {
    if (port >= 2 || id >= 16) return;
    if (pressed) buttons_[port].fetch_or(1u << id); else buttons_[port].fetch_and(~(1u << id));
}
void EmulationSession::releaseButtons() { for (auto& mask : buttons_) mask.store(0); }
void EmulationSession::setFastForward(bool enabled) { fastForward_.store(enabled); }
void EmulationSession::shutdown() { quitting_.store(true); if (isRunning()) wait(); }
void EmulationSession::run() {
    using Clock = std::chrono::steady_clock;
    LibretroHost host(corePath_,dataPath_);
    host.message = [this](const QString& message) { emit notification(message); };
    bool paused = false;
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
                if (host.load(command.path,error)) {
                    loaded_.store(true); releaseButtons(); paused = false;
                    emit audioReset();
                    emit gameLoaded(host.gamePath(),host.fps()); emit pausedChanged(false);
                } else emit errorOccurred(error);
                loaded_.store(host.loaded());
                break;
            case Type::Pause:
                paused = command.value != 0;
                emit audioReset();
                emit pausedChanged(paused);
                break;
            case Type::Reset:
                host.reset(); emit audioReset(); emit notification(QStringLiteral("Консоль перезапущена")); break;
            case Type::Save:
                if (host.saveState(command.value,error)) emit notification(QStringLiteral("Сохранено в слот %1").arg(command.value));
                else emit errorOccurred(error);
                break;
            case Type::Restore:
                if (host.loadState(command.value,error)) { emit audioReset(); emit notification(QStringLiteral("Загружен слот %1").arg(command.value)); }
                else emit errorOccurred(error);
                break;
            }
            deadline = Clock::now();
        }
        if (!host.loaded() || paused) { std::this_thread::sleep_for(std::chrono::milliseconds(4)); continue; }
        for (unsigned port = 0; port < 2; ++port) host.setButtons(port,buttons_[port].load());
        host.runFrame();
        const bool fast = fastForward_.load();
        if (!fast || (++fastFrame % 4 == 0)) emit videoFrame(host.frame(),host.aspectRatio());
        if (!fast && !host.audio().isEmpty()) emit audioSamples(host.audio(),host.sampleRate());
        const auto now = Clock::now();
        if (now - lastFlush > std::chrono::seconds(5)) {
            QString error;
            if (!host.flushSaveRam(error)) emit notification(error);
            lastFlush = now;
        }
        const auto frameTime = std::chrono::duration<double>(1.0 / (host.fps() * (fast ? 4.0 : 1.0)));
        deadline += std::chrono::duration_cast<Clock::duration>(frameTime);
        // A stalled window/OS must not trigger an unbounded catch-up burst.
        if (now - deadline > std::chrono::milliseconds(100)) deadline = now;
        std::this_thread::sleep_until(deadline);
    }
    host.unload();
    loaded_.store(false);
}
