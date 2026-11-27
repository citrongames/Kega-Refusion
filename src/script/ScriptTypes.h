#pragma once

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QVector>
#include <memory>

namespace ScriptLimits {
inline constexpr int MaxScripts = 8;
inline constexpr int MaxSourceBytes = 256 * 1024;
inline constexpr int MaxImages = 16;
inline constexpr int MaxImageSide = 1024;
inline constexpr int MaxImageNameBytes = 128;
inline constexpr int MaxImageBytes = 8 * 1024 * 1024;
inline constexpr int MaxUpdates = 128;
}

enum class ScriptAction { Run, Stop, StopAll };
struct ScriptCommand {
    ScriptAction action = ScriptAction::Run;
    quint64 id = 0;
    QString path;
    QByteArray source;
    QHash<QString, QImage> images;
    quint64 machineRevision = 0;
};

enum class ScriptStatus { Running, Stopped, Error, Log };
struct ScriptUpdate {
    quint64 id = 0;
    QString path;
    ScriptStatus status = ScriptStatus::Stopped;
    QString text;
    int line = 0;
};

enum class ScriptEvent { Frame, Reset, LoadState, Rewind, Draw };
enum class ScriptSpace { Game, Hud };
enum class ScriptPrimitive { Text, Line, Rectangle, Circle, Image };
struct ScriptDraw {
    ScriptPrimitive primitive = ScriptPrimitive::Text;
    ScriptSpace space = ScriptSpace::Game;
    QRectF bounds;
    QColor color = Qt::white;
    QColor fill = Qt::transparent;
    QString text;
    QImage image;
};

struct ScriptOverlay {
    quint64 generation = 0, frame = 0;
    int instance = 0;
    QVector<ScriptDraw> commands;
};
using ScriptOverlayPtr = std::shared_ptr<const ScriptOverlay>;
