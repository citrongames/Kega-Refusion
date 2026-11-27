#include "ui/ScriptPanelManager.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

template<typename Widget>
Widget* find(QWidget& panel, const char* name) {
    auto* widget = panel.findChild<Widget*>(QString::fromLatin1(name));
    check(widget, name);
    return widget;
}

ScriptUiControl control(const char* id, int parent, ScriptUiKind kind, const char* text = "",
                        const QVariant& value = {}) {
    ScriptUiControl result;
    result.id = QString::fromLatin1(id);
    result.parent = parent;
    result.kind = kind;
    result.text = QString::fromLatin1(text);
    result.value = value;
    return result;
}

QImage draw(QWidget* graph) {
    QImage image(graph->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    graph->render(&image);
    return image;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QWidget mainWindow;
    mainWindow.show();
    mainWindow.activateWindow();
    app.processEvents();

    ScriptPanelManager panels(&mainWindow);
    QList<ScriptUiEvent> events;
    bool reject = false;
    QObject::connect(&panels, &ScriptPanelManager::eventRequested, &panels, [&](const ScriptUiEvent& event) {
        events.append(event);
        if (reject) panels.rejectEvent(event);
    });

    ScriptUiSnapshot snapshot{1, 5, {{1, 9, 7, 0, QStringLiteral("Custom tool"), {
        control("group", -1, ScriptUiKind::Group, "Tools"),
        control("row", 0, ScriptUiKind::Row),
        control("message", 1, ScriptUiKind::Label, "<b>Ready</b>"),
        control("fire", 1, ScriptUiKind::Button, "Fire & stop"),
        control("enabled", 0, ScriptUiKind::Checkbox, "Enabled", false),
        control("name", -1, ScriptUiKind::Text, "Name", QStringLiteral("Original")),
        control("rate", -1, ScriptUiKind::Number, "Rate", 0.125),
        control("mode", -1, ScriptUiKind::Select, "Mode", 1),
        control("graph", -1, ScriptUiKind::Graph, "Samples")
    }}}};
    auto& state = snapshot.windows[0];
    state.controls[6].step = 0.001;
    state.controls[7].items = {QStringLiteral("Slow"), QStringLiteral("Fast")};
    state.controls[8].points = {0.0, 2.0, 1.0};
    const auto apply = [&] {
        panels.applySnapshot(snapshot);
        app.processEvents();
    };

    panels.applySnapshot(snapshot);
    check(QApplication::activeWindow() == &mainWindow, "Opening does not request activation");
    app.processEvents();
    auto* panel = find<QWidget>(mainWindow, "luaScriptPanel");
    check(panel->isVisible() && panel->isWindow() && !panel->isModal() &&
          !panel->windowFlags().testAnyFlags(Qt::WindowMinMaxButtonsHint) &&
          panel->testAttribute(Qt::WA_ShowWithoutActivating), "Independent modeless tool window");
    check(panel->screen()->availableGeometry().size().expandedTo(panel->size()) ==
          panel->screen()->availableGeometry().size(), "Window fits the available screen");
    auto* group = find<QGroupBox>(*panel, "group");
    auto* row = find<QWidget>(*group, "row");
    auto* label = find<QLabel>(*row, "message");
    auto* button = find<QPushButton>(*row, "fire");
    auto* checkbox = find<QCheckBox>(*group, "enabled");
    auto* field = find<QLineEdit>(*panel, "name");
    auto* rate = find<QDoubleSpinBox>(*panel, "rate");
    auto* mode = find<QComboBox>(*panel, "mode");
    auto* graph = find<QWidget>(*panel, "graph");
    check(qobject_cast<QHBoxLayout*>(row->layout()) && label->textFormat() == Qt::PlainText &&
          group->title() == QStringLiteral("Tools") && label->text() == QStringLiteral("<b>Ready</b>"),
          "Rendered groups, rows and literal label text");
    check(!checkbox->isChecked() && rate->value() == 0.125 && mode->currentIndex() == 0,
          "Applied initial control values");
    check(panels.ownsWindow(panel) && panels.ownsWindow(button) && !panels.ownsWindow(&mainWindow),
          "Panel and child ownership");
    mode->showPopup();
    app.processEvents();
    check(QApplication::activePopupWidget() && panels.ownsWindow(QApplication::activePopupWidget()),
          "Select popup belongs to panel");
    mode->hidePopup();

    const QImage previousGraph = draw(graph);
    state.controls[2].text = QStringLiteral("Updated");
    state.controls[8].points = {-std::numeric_limits<double>::max(), 0, std::numeric_limits<double>::max()};
    ++snapshot.revision;
    apply();
    check(label == find<QLabel>(*row, "message") && label->text() == QStringLiteral("Updated") &&
          graph == find<QWidget>(*panel, "graph") && previousGraph != draw(graph) && events.isEmpty(),
          "Diff updates in place with finite graph scaling and no event echo");

    button->click();
    check(events.size() == 1 && events.last().kind == ScriptUiEventKind::Click &&
          events.last().control == QStringLiteral("fire") && events.last().owner == 1 &&
          events.last().run == 9 && events.last().window == 7 && events.last().epoch == 5,
          "Routed click with script run, window and epoch");
    checkbox->click();
    check(events.size() == 2 && events.last().value.metaType().id() == QMetaType::Bool &&
          events.last().value.toBool(), "Checkbox change");
    const auto checkedSequence = events.last().sequence;
    ++snapshot.revision;
    apply();
    check(checkbox->isChecked(), "Unacknowledged checkbox edit survives older values");
    state.acknowledged = checkedSequence;
    ++snapshot.revision;
    apply();
    check(!checkbox->isChecked() && events.size() == 2, "Acknowledged value can be overridden by script");

    panel->activateWindow();
    field->setFocus();
    app.processEvents();
    check(field->hasFocus(), "Editor focused for edit protection");
    field->setText(QStringLiteral("User draft"));
    field->setModified(true);
    state.controls[5].value = QStringLiteral("Script draft");
    ++snapshot.revision;
    apply();
    check(field->text() == QStringLiteral("User draft") && events.size() == 2, "Protects unfinished edit");
    field->editingFinished();
    check(events.size() == 3 && events.last().control == QStringLiteral("name") &&
          events.last().value.toString() == QStringLiteral("User draft"), "Field commits the user's text");
    ++snapshot.revision;
    apply();
    check(field->text() == QStringLiteral("User draft"), "Protects committed text pending acknowledgement");

    rate->stepUp();
    check(events.size() == 4 && events.last().kind == ScriptUiEventKind::Change &&
          events.last().control == QStringLiteral("rate") && events.last().value.toDouble() == 0.126,
          "Numeric control preserves fractional increments");
    mode->setCurrentIndex(1);
    mode->activated(1);
    check(events.size() == 5 && events.last().control == QStringLiteral("mode") &&
          events.last().value.metaType().id() == QMetaType::Int && events.last().value.toInt() == 2,
          "Select event uses one-based index");

    const auto priorEpoch = snapshot;
    ++snapshot.epoch;
    ++snapshot.revision;
    apply();
    check(field->text() == QStringLiteral("Script draft") && mode->currentIndex() == 0 &&
          rate->value() == 0.125 && events.size() == 5, "Epoch reset reconciles pending input");
    panels.applySnapshot(priorEpoch);
    check(field->text() == QStringLiteral("Script draft"), "Older epoch is discarded");

    reject = true;
    checkbox->click();
    check(events.size() == 6 && !checkbox->isChecked(), "Rejected value reconciles immediately");
    check(!panel->close() && panel->isVisible(), "Rejected close leaves tool open");
    reject = false;
    check(panel->close() && !panel->isVisible() && events.size() == 8 &&
          events.last().kind == ScriptUiEventKind::Close && events.last().epoch == snapshot.epoch,
          "One close event hides its window");
    const auto close = events.last();
    ++snapshot.revision;
    apply();
    panel->close();
    check(!panel->isVisible() && events.size() == 8, "Waiting for close never resurrects stale tool");

    state.run++;
    state.title = QStringLiteral("New run");
    ++snapshot.revision;
    QPointer<QWidget> oldPanel(panel);
    apply();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(oldPanel.isNull(), "Superseded run deletes its tools");
    panel = find<QWidget>(mainWindow, "luaScriptPanel");
    panels.rejectEvent(close);
    check(panel->isVisible() && panel->windowTitle() == QStringLiteral("New run") &&
          events.size() == 8, "Old event cannot affect replacement script");
    oldPanel = panel;
    snapshot.windows.clear();
    ++snapshot.revision;
    apply();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(oldPanel.isNull() && !mainWindow.findChild<QWidget*>(QStringLiteral("luaScriptPanel")) &&
          events.size() == 8, "Cancellation cleans up tool windows without callbacks");
    std::puts("Lua panel view tests passed");
}
