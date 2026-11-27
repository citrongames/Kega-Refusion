#pragma once

#include "ScriptTypes.h"

class LibretroHost;

// Owned, called and destroyed on the emulation worker. No file or Qt widget access.
class ScriptRuntime final {
public:
    ScriptRuntime();
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    bool hasScripts() const;
    bool compatible(const LibretroHost& host) const;
    void command(const ScriptCommand& command, const LibretroHost& host);
    ScriptOverlayPtr event(ScriptEvent event, const LibretroHost& host,
                           quint64 generation, quint64 frame, int instance = 0,
                           bool draw = true);
    void stopAll(const QString& reason = {});
    QVector<ScriptUpdate> takeUpdates();

private:
    struct Script;
    struct State;
    std::unique_ptr<State> state_;
    QVector<ScriptUpdate> updates_;

    void update(ScriptUpdate update);
};
