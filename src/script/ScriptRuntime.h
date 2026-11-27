#pragma once

#include "ScriptTypes.h"
#include <functional>

class LibretroHost;
struct MovieInput;

enum class ScriptControlType { Pause, Reset, LoadState, FrameAdvance };
struct ScriptControl {
    ScriptControlType type;
    quint64 owner;
    quint64 run;
    bool value = false;
    std::shared_ptr<const QByteArray> snapshot;
};

struct ScriptHostServices {
    bool inputAllowed = false;
    bool controlAllowed = false;
    bool saveAllowed = false;
    std::function<QByteArray(qsizetype remaining, QString& error)> captureSave;
};

// Owned, called and destroyed on the emulation worker. No file or Qt widget access.
class ScriptRuntime final {
public:
    ScriptRuntime();
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    bool hasScripts() const;
    bool compatible(const LibretroHost& host) const;
    void setHostServices(ScriptHostServices services);
    void setHostPolicy(bool inputAllowed, bool controlAllowed, bool saveAllowed);
    QVector<ScriptControl> takeControls();
    bool ownsControl(const ScriptControl& control) const;
    void controlFailed(const ScriptControl& control, const QString& error);
    void invalidateTimeline(bool discardSaves, quint64 preserveRun = 0);
    void beforeFrame(const LibretroHost& host, quint64 generation, quint64 frame, MovieInput& input);
    void command(const ScriptCommand& command, const LibretroHost& host);
    ScriptOverlayPtr event(ScriptEvent event, const LibretroHost& host,
                           quint64 generation, quint64 frame, int instance = 0,
                           bool draw = true, const MovieInput* input = nullptr);
    void stopAll(const QString& reason = {});
    QVector<ScriptUpdate> takeUpdates();

private:
    struct Script;
    struct State;
    ScriptHostServices services_;
    QVector<ScriptControl> controls_;
    QVector<ScriptUpdate> updates_;
    quint64 nextRun_ = 0;
    std::shared_ptr<qsizetype> saveBytes_;
    std::unique_ptr<State> state_;

    void update(ScriptUpdate update);
};
