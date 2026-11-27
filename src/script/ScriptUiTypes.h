#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

namespace ScriptUiLimits {
inline constexpr int MaxTextBytes = 1024;
}

enum class ScriptUiKind { Row, Group, Label, Button, Checkbox, Text, Number, Select, Graph };
enum class ScriptUiEventKind { Close, Click, Change };

struct ScriptUiControl {
    QString id, text;
    int parent = -1;
    ScriptUiKind kind = ScriptUiKind::Label;
    QVariant value;
    QStringList items;
    QVector<double> points;
    double minimum = 0, maximum = 100, step = 1;
    bool enabled = true;
};

struct ScriptUiWindow {
    quint64 owner = 0, run = 0, handle = 0, acknowledged = 0;
    QString title;
    QVector<ScriptUiControl> controls;
};

struct ScriptUiSnapshot {
    quint64 revision = 0, epoch = 0;
    QVector<ScriptUiWindow> windows;
};

struct ScriptUiEvent {
    quint64 owner = 0, run = 0, window = 0, sequence = 0, epoch = 0;
    QString control;
    ScriptUiEventKind kind = ScriptUiEventKind::Close;
    QVariant value;
};
