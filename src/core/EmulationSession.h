#pragma once
#include <QThread>
#include <QImage>
#include <QByteArray>
#include <QString>
#include <atomic>
#include <deque>
#include <mutex>

class EmulationSession final : public QThread {
    Q_OBJECT
public:
    explicit EmulationSession(QString corePath, QString dataPath, QObject* parent = nullptr);
    ~EmulationSession() override;
    void loadGame(const QString& path);
    void setPaused(bool paused);
    void resetGame();
    void saveState(int slot);
    void loadState(int slot);
    void setButton(unsigned port, unsigned id, bool pressed);
    void setButtons(unsigned port, unsigned mask);
    void releaseButtons();
    void setFastForward(bool enabled);
    void shutdown();
    bool hasGame() const { return loaded_.load(); }
signals:
    void videoFrame(const QImage& frame, double aspectRatio);
    void audioReset();
    void audioSamples(const QByteArray& pcm, int sampleRate);
    void gameLoaded(const QString& path, double fps);
    void pausedChanged(bool paused);
    void notification(const QString& text);
    void actionCompleted(const QString& text);
    void errorOccurred(const QString& text);
protected:
    void run() override;
private:
    enum class Type { Load, Pause, Reset, Save, Restore };
    struct Command { Type type; QString path; int value = 0; };
    void enqueue(Command command);
    QString corePath_, dataPath_;
    std::mutex mutex_;
    std::deque<Command> commands_;
    std::atomic<bool> quitting_{false}, loaded_{false}, fastForward_{false};
    std::atomic<unsigned> buttons_[2]{};
};
