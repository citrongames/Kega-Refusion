#pragma once

#include "ScriptTypes.h"
#include "core/DebugAccess.h"
#include <functional>

class LibretroHost;
struct MovieInput;

enum class ScriptControlType { Pause, Reset, LoadState, FrameAdvance, MemoryWrite, RegisterWrite };
struct ScriptControl {
    ScriptControlType type;
    quint64 owner;
    quint64 run;
    bool value = false;
    std::shared_ptr<const QByteArray> snapshot;
    unsigned instance = 0, target = 0;
    quint32 offset = 0, valueBits = 0;
    QByteArray bytes, registerName;
    quint64 saveRevision = 0;
};

struct ScriptHostServices {
    bool inputAllowed = false;
    bool controlAllowed = false;
    bool saveAllowed = false;
    std::function<QByteArray(qsizetype remaining, QString& error)> captureSave;
    bool mutationAllowed = false;
    bool accessAllowed = false;
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
    void setHostPolicy(bool inputAllowed, bool controlAllowed, bool saveAllowed,
                       bool mutationAllowed = false, bool accessAllowed = false);
    QVector<ScriptControl> takeControls();
    bool ownsControl(const ScriptControl& control) const;
    void controlFailed(const ScriptControl& control, const QString& error);
    void invalidateTimeline(bool discardSaves, quint64 preserveRun = 0);
    void discardSavedStates();
    quint64 accessRevision() const { return accessRevision_; }
    QVector<DebugAccessSubscription> accessSubscriptions() const;
    void beginAccessFrame(const LibretroHost& host, quint64 frame);
    void dispatchMemoryAccess(const DebugAccessEvent& event);
    void endAccessFrame();
    quint64 annotationRevision() const { return annotationRevision_; }
    ScriptDebugAnnotations annotations() const;
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
    quint64 saveRevision_ = 0, accessRevision_ = 0, annotationRevision_ = 0;
    qsizetype accessCount_ = 0, labelCount_ = 0, watchCount_ = 0;
    quint32 nextLabel_ = 0, nextWatch_ = 0x7fffffff;
    const LibretroHost* accessHost_ = nullptr;
    quint64 accessFrame_ = 0;
    int accessCallbacks_ = 0, accessActiveScripts_ = 0;
    qint64 accessNanoseconds_ = 0;
    bool accessFailed_ = false;
    std::shared_ptr<qsizetype> saveBytes_;
    std::unique_ptr<State> state_;

    void update(ScriptUpdate update);
};
