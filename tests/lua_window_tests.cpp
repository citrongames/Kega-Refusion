#include "ui/LuaScriptsDialog.h"
#include "i18n/Localization.h"

#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>

#include <cstdio>
#include <cstdlib>

namespace {

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

template<typename Widget>
Widget* find(LuaScriptsDialog& dialog, const char* name) {
    auto* widget = dialog.findChild<Widget*>(QString::fromLatin1(name));
    check(widget, name);
    return widget;
}

template<typename Condition>
void waitFor(Condition condition, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    check(condition(), message);
}

void writeSource(const QString& path, const QByteArray& source) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(source) == source.size(), "Write script fixture");
}

void openSource(LuaScriptsDialog& dialog, const QString& path, const QString& initialDirectory) {
    find<QPushButton>(dialog, "luaOpen")->click();
    auto* picker = dialog.findChild<QFileDialog*>();
    check(picker && picker->isVisible(), "Asynchronous script picker");
    check(picker->windowModality() == Qt::WindowModal, "Native-compatible script picker modality");
    check(!picker->testOption(QFileDialog::DontUseNativeDialog), "Native script picker allowed");
    check(picker->fileMode() == QFileDialog::ExistingFile, "Script picker opens existing files");
    check(picker->directory().absolutePath() == initialDirectory, "Picker uses current scripts folder");
    picker->fileSelected(path);
    picker->reject();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir fixtures;
    check(fixtures.isValid(), "Temporary fixture directory");
    // Use the shipped catalogs: an uninitialized translator hides missing RU entries.
    check(argc == 2, "Repository path for shipped translations");
    QSettings settings(fixtures.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    for (const QString& language : {QStringLiteral("en"), QStringLiteral("ru")}) {
        settings.setValue(QStringLiteral("ui/language"), language);
        settings.sync();
        Localization::initialize(QString::fromLocal8Bit(argv[1]), fixtures.path());
        LuaScriptsDialog localized;
        const bool russian = language == QLatin1String("ru");
        check(localized.windowTitle() == (russian ? QStringLiteral("Lua-скрипты") : QStringLiteral("Lua scripts")),
              "Localized Lua window title");
        check(find<QPushButton>(localized, "luaRun")->text() ==
              (russian ? QStringLiteral("Запустить скрипт") : QStringLiteral("Run")), "Localized Run button");
        check(Localization::text(QStringLiteral("Скрипты…")) ==
              (russian ? QStringLiteral("Скрипты…") : QStringLiteral("Scripts…")), "Localized File menu entry");
    }
    const auto sourcePath = fixtures.filePath(QStringLiteral("draw.lua"));
    const auto otherDirectory = fixtures.filePath(QStringLiteral("other"));
    check(QDir().mkpath(otherDirectory), "Create second scripts folder");
    const auto secondPath = QDir(otherDirectory).filePath(QStringLiteral("other.lua"));
    const auto imagePath = fixtures.filePath(QStringLiteral("sprite.png"));
    const QByteArray source("-- @image sprite.png\nprint('first')\n");
    QImage sprite(QSize(4, 4), QImage::Format_ARGB32_Premultiplied);
    sprite.fill(Qt::red);
    check(sprite.save(imagePath), "Write PNG fixture");
    writeSource(sourcePath, source);
    writeSource(secondPath, "print('second')\n");

    LuaScriptsDialog dialog;
    quint64 machineRevision = 12;
    dialog.setMachineRevisionProvider([&] { return machineRevision; });
    QString scriptsDirectory = fixtures.path();
    dialog.setScriptsDirectoryProvider([&] { return scriptsDirectory; });
    QList<ScriptCommand> commands;
    QObject::connect(&dialog, &LuaScriptsDialog::commandRequested, &dialog,
                     [&](const ScriptCommand& command) { commands.append(command); });
    dialog.show();
    check(!dialog.isModal() && !(dialog.windowFlags() & Qt::WindowMinMaxButtonsHint), "Modeless script window");
    auto* scripts = find<QTreeWidget>(dialog, "luaScripts");
    auto* run = find<QPushButton>(dialog, "luaRun");
    auto* stop = find<QPushButton>(dialog, "luaStop");
    auto* restart = find<QPushButton>(dialog, "luaRestart");
    auto* log = find<QListWidget>(dialog, "luaLog");
    auto* edit = find<QPlainTextEdit>(dialog, "luaSource");
    auto* expanded = find<QToolButton>(dialog, "luaEditorToggle");
    check(!expanded->isChecked(), "Editor initially collapsed");
    openSource(dialog, sourcePath, fixtures.path());
    waitFor([&] { return edit->toPlainText().toUtf8() == source; }, "Open loads source asynchronously");
    check(commands.isEmpty() && !run->isEnabled(), "Opening never runs a script");

    dialog.setExecutionAvailable(true);
    run->click();
    dialog.discardPendingStarts();
    waitFor([&] { return run->isEnabled(); }, "Cancelled preparation returns to idle");
    check(commands.isEmpty(), "Pending start cannot follow a machine change");

    const auto originalRevision = machineRevision;
    run->click();
    ++machineRevision; // Machine changed before the queued GUI context signal arrived.
    waitFor([&] { return commands.size() == 1; }, "Complete preparation before context notice");
    const auto stale = commands.takeLast();
    check(stale.machineRevision == originalRevision && stale.machineRevision != machineRevision,
          "Run preserves the machine revision captured before asynchronous preparation");
    dialog.rejectCommand(stale, QStringLiteral("Machine changed"));

    run->click();
    waitFor([&] { return commands.size() == 1; }, "Start submitted while debugger is suspended");
    const auto pending = commands.takeLast();
    check(stop->isEnabled() && !run->isEnabled(), "Stop remains available during deferred start");
    stop->click();
    check(commands.size() == 1 && commands.last().action == ScriptAction::Stop &&
          commands.last().id == pending.id, "Stop cancels the pending script");
    dialog.rejectCommand(commands.takeLast(), QStringLiteral("Queue full"));
    check(stop->isEnabled() && !run->isEnabled(), "Rejected Stop preserves pending start");
    stop->click();
    commands.clear();
    dialog.applyUpdates({{pending.id, pending.path, ScriptStatus::Stopped, {}}});
    check(run->isEnabled() && !stop->isEnabled(), "Acknowledged cancellation returns to idle");

    run->click();
    waitFor([&] { return commands.size() == 1; }, "Explicit run after preparation");
    const auto first = commands.first();
    check(first.action == ScriptAction::Run && first.machineRevision == machineRevision &&
          first.source == source && first.path == sourcePath &&
          first.images.value(QStringLiteral("sprite.png")).pixelColor(0, 0) == QColor(Qt::red),
          "Run passes source and decoded image");
    dialog.applyUpdates({{first.id, sourcePath, ScriptStatus::Running, {}}});
    check(stop->isEnabled() && restart->isEnabled() && !run->isEnabled(), "Running controls");
    restart->click();
    waitFor([&] { return commands.size() == 2; }, "Explicit restart");
    check(commands.last().action == ScriptAction::Run && commands.last().id == first.id, "Restart replaces existing id");
    dialog.applyUpdates({{first.id, sourcePath, ScriptStatus::Running, {}}});

    scriptsDirectory = otherDirectory;
    openSource(dialog, secondPath, otherDirectory);
    waitFor([&] { return run->isEnabled(); }, "Second script loaded independently");
    check(scripts->topLevelItemCount() == 2, "Two loaded scripts");
    run->click();
    waitFor([&] { return commands.size() == 3; }, "Start second script");
    const auto second = commands.last();
    check(second.id != first.id && second.images.isEmpty(), "Images belong only to the requesting script");
    dialog.applyUpdates({{second.id, secondPath, ScriptStatus::Running, {}}});
    scripts->setCurrentItem(scripts->topLevelItem(0));
    stop->click();
    check(commands.size() == 4 && commands.last().action == ScriptAction::Stop && commands.last().id == first.id,
          "Stop selected script only");
    dialog.rejectCommand(commands.last(), QStringLiteral("Queue full"));
    check(stop->isEnabled() && restart->isEnabled() && !run->isEnabled(),
          "Rejected stop preserves running status");
    stop->click();
    check(commands.size() == 5 && commands.last().action == ScriptAction::Stop, "Retry rejected stop");
    dialog.applyUpdates({{first.id, sourcePath, ScriptStatus::Stopped, {}}});

    QList<ScriptUpdate> output;
    for (int i = 0; i < 300; ++i)
        output.append({second.id, secondPath, ScriptStatus::Log, QString::number(i)});
    output.append({first.id, sourcePath, ScriptStatus::Error, QStringLiteral("Syntax error"), 2});
    dialog.applyUpdates(output);
    check(log->count() == 250, "Log bounds");
    log->itemActivated(log->item(log->count() - 1));
    check(expanded->isChecked() && edit->textCursor().blockNumber() == 1, "Error opens the editor on its source line");

    edit->moveCursor(QTextCursor::End);
    edit->insertPlainText(QStringLiteral("-- edited\n"));
    dialog.close();
    dialog.show();
    check(edit->toPlainText().endsWith(QStringLiteral("-- edited\n")), "Closing the window retains edits");
    run->click();
    waitFor([&] { return commands.size() == 6; }, "Run edited source");
    check(commands.last().source.endsWith("-- edited\n"), "Run uses unsaved text");
    dialog.applyUpdates({{first.id, sourcePath, ScriptStatus::Stopped, {}}});

    const auto* oldContent = "print('changed on disk')\n";
    writeSource(sourcePath, oldContent);
    auto* save = find<QPushButton>(dialog, "luaSave");
    save->click();
    waitFor([&] { return save->isEnabled(); }, "Save returns to idle after edit conflict");
    QFile disk(sourcePath);
    check(disk.open(QIODevice::ReadOnly) && disk.readAll() == oldContent,
          "Save preserves edits made in an external editor");
    std::puts("Lua script window tests passed");
}
