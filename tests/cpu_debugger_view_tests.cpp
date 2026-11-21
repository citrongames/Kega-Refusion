#include "ui/CpuDebuggerDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

namespace {
void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

template<typename T>
T* find(QWidget& parent, const char* name) {
    auto* widget = parent.findChild<T*>(QString::fromLatin1(name));
    check(widget != nullptr, name);
    return widget;
}

DebuggerSnapshot attached() {
    DebuggerSnapshot snapshot;
    snapshot.attached = true;
    snapshot.generation = 1;
    snapshot.gameKey = QStringLiteral("fixture.md\nMD fixture");
    snapshot.frame = 10;
    kr_debug_cpu md{};
    md.cpu = KR_DEBUG_M68K;
    md.pc = 0x102;
    md.sp = 0xff4000;
    md.cycles = 200;
    md.count = 1;
    md.regs[0].digits = 4;
    md.regs[0].value = 0x2704;
    qstrncpy(md.regs[0].name, "SR", sizeof(md.regs[0].name));
    snapshot.cpus.append(md);
    kr_debug_cpu z80{};
    z80.cpu = KR_DEBUG_Z80;
    z80.pc = 0xc123;
    z80.count = 1;
    z80.regs[0].value = 0x5540;
    z80.regs[0].digits = 4;
    qstrncpy(z80.regs[0].name, "AF", sizeof(z80.regs[0].name));
    snapshot.cpus.append(z80);
    kr_debug_instruction instruction{};
    instruction.address = md.pc;
    instruction.valid = 1;
    instruction.length = 2;
    instruction.bytes[0] = 0x4e;
    instruction.bytes[1] = 0x71;
    qstrncpy(instruction.text, "nop", sizeof(instruction.text));
    snapshot.instructions.append(instruction);
    snapshot.stackAddress = md.sp;
    snapshot.stack = QByteArray::fromHex("7700");
    snapshot.stackValid = QByteArray::fromHex("0100");
    return snapshot;
}

void verify(const QString& previews) {
    CpuDebuggerDialog dialog;
    QVector<DebuggerCommand> commands;
    QObject::connect(&dialog, &CpuDebuggerDialog::command, &dialog,
                     [&](const DebuggerCommand& command) { commands.append(command); });
    check(!dialog.isModal(), "CPU debugger is modeless");
    check(dialog.windowFlags().testFlag(Qt::WindowCloseButtonHint) &&
          !dialog.windowFlags().testFlag(Qt::WindowMaximizeButtonHint) &&
          !dialog.windowFlags().testFlag(Qt::WindowMinimizeButtonHint), "close-only frame");
    dialog.show();
    check(commands.size() == 1 && commands.last().kind == DebuggerCommand::Attach,
          "opening attaches without pause");

    auto snapshot = attached();
    dialog.setSnapshot(snapshot);
    auto* cpu = find<QComboBox>(dialog, "debugCpuSelector");
    auto* code = find<QTableWidget>(dialog, "debugCpuDisassembly");
    auto* registers = find<QTableWidget>(dialog, "debugCpuRegisters");
    auto* stack = find<QPlainTextEdit>(dialog, "debugCpuStack");
    auto* status = find<QLabel>(dialog, "debugCpuStatus");
    auto* pause = find<QPushButton>(dialog, "debugCpuPause");
    auto* resume = find<QPushButton>(dialog, "debugCpuContinue");
    auto* into = find<QPushButton>(dialog, "debugCpuStepInto");
    auto* over = find<QPushButton>(dialog, "debugCpuStepOver");
    auto* out = find<QPushButton>(dialog, "debugCpuStepOut");
    check(cpu->isVisible() && cpu->count() == 2, "two CPU choices for MD");
    check(code->rowCount() == 1 && code->item(0, 1)->text() == QStringLiteral("4E 71"),
          "instruction bytes in guest order");
    check(registers->item(0, 1)->text() == QStringLiteral("0x2704") &&
          registers->item(2, 1)->text().contains(QStringLiteral("IPL=7")), "captured SR and flags");
    check(stack->isReadOnly() && stack->toPlainText() == QStringLiteral("FF4000 77 ??"), "stack validity");
    check(pause->isEnabled() && !resume->isEnabled() && !into->isEnabled(), "running controls");
    pause->click();
    check(commands.last().kind == DebuggerCommand::Pause, "explicit pause command");

    snapshot.stopped = true;
    snapshot.partialFrame = true;
    snapshot.event = KR_DEBUG_READ;
    snapshot.eventCpu = KR_DEBUG_Z80;
    snapshot.address = 0xc123;
    snapshot.value = 0x55;
    snapshot.accessSize = 1;
    dialog.setSnapshot(snapshot);
    check(status->text().contains(QStringLiteral("Z80")) && status->text().contains(QStringLiteral("0xC123")),
          "data stop uses actual event CPU");
    check(!pause->isEnabled() && resume->isEnabled() && into->isEnabled(), "stopped controls");
    cpu->setCurrentIndex(cpu->findData(quint32(KR_DEBUG_M68K)));
    check(commands.last().kind == DebuggerCommand::Navigate && commands.last().cpu == KR_DEBUG_M68K &&
          commands.last().followPc, "CPU switch follows its PC");
    auto* address = find<QLineEdit>(dialog, "debugCpuAddress");
    address->setText(QStringLiteral("$1234"));
    find<QPushButton>(dialog, "debugCpuNavigate")->click();
    check(commands.last().kind == DebuggerCommand::Navigate && commands.last().address == 0x1234 &&
          !dialog.refreshCommand().followPc, "explicit hex navigation");
    find<QPushButton>(dialog, "debugCpuFollowPc")->click();
    check(dialog.refreshCommand().followPc, "PC tracking resumes");
    into->click();
    check(commands.last().kind == DebuggerCommand::StepInto, "step into");
    over->click();
    check(commands.last().kind == DebuggerCommand::StepOver, "step over");
    out->click();
    check(commands.last().kind == DebuggerCommand::StepOut, "step out");
    resume->click();
    check(commands.last().kind == DebuggerCommand::Continue, "continue");

    // Pending break and step actions only affect the UI after the owner sends a snapshot.
    dialog.setSnapshot(snapshot);
    emit code->cellDoubleClicked(0, 0);
    check(commands.last().kind == DebuggerCommand::Configure &&
          commands.last().breakpoints.size() == 1 && commands.last().breakpoints[0].first == 0x102,
          "double-click enables an execute breakpoint");
    snapshot.breakpoints = commands.last().breakpoints;
    dialog.setSnapshot(snapshot);

    auto* tabs = find<QTabWidget>(dialog, "debugCpuTabs");
    tabs->setCurrentIndex(1);
    find<QPushButton>(dialog, "debugBreakNew")->click();
    find<QLineEdit>(dialog, "debugBreakFirst")->setText(QStringLiteral("FF0000"));
    find<QLineEdit>(dialog, "debugBreakLast")->setText(QStringLiteral("FF0010"));
    find<QCheckBox>(dialog, "debugBreakEvent1")->setChecked(false);
    find<QCheckBox>(dialog, "debugBreakEvent4")->setChecked(true);
    find<QCheckBox>(dialog, "debugBreakOneShot")->setChecked(true);
    find<QLineEdit>(dialog, "debugBreakIgnoreHits")->setText(QStringLiteral("2"));
    find<QLineEdit>(dialog, "debugBreakCondition")->setText(QStringLiteral("VALUE == $55"));
    find<QPushButton>(dialog, "debugBreakSave")->click();
    check(commands.last().kind == DebuggerCommand::Configure && commands.last().breakpoints.size() == 2,
          "range editor preserves existing breakpoint");
    auto bp = commands.last().breakpoints.last();
    check(commands.last().editedBreakpointId == bp.id, "command identifies the edited breakpoint");
    check(bp.cpu == KR_DEBUG_M68K && bp.events == KR_DEBUG_WRITE && bp.first == 0xff0000 &&
          bp.last == 0xff0010 && bp.oneShot && bp.ignoreHits == 2 &&
          bp.condition == QStringLiteral("VALUE == $55"), "data breakpoint configuration");
    snapshot.breakpoints = commands.last().breakpoints;
    dialog.setSnapshot(snapshot);
    check(find<QTableWidget>(dialog, "debugCpuBreakpoints")->rowCount() == 2, "breakpoint list snapshot");

    if (!previews.isEmpty()) {
        QApplication::processEvents();
        check(dialog.grab().save(QDir(previews).filePath(QStringLiteral("breakpoints.png"))),
              "save breakpoint preview");
    }

    tabs->setCurrentIndex(0);
    QTemporaryDir directory;
    check(directory.isValid(), "temporary symbol directory");
    QFile symbols(directory.filePath(QStringLiteral("fixture.sym")));
    check(symbols.open(QIODevice::WriteOnly), "create fixture symbol file");
    check(symbols.write("# test\n0102 main ; original fixture\n") > 0, "write fixture symbol file");
    symbols.close();
    QTimer::singleShot(0, &dialog, [&] {
        auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        check(picker != nullptr, "explicit symbol file dialog");
        picker->selectFile(symbols.fileName());
        check(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection), "accept symbol file");
    });
    QTimer pickerTimeout;
    pickerTimeout.setSingleShot(true);
    QObject::connect(&pickerTimeout, &QTimer::timeout, &dialog, [] {
        check(false, "symbol file dialog finished within two seconds");
    });
    pickerTimeout.start(2000);
    find<QPushButton>(dialog, "debugCpuImportSymbols")->click();
    pickerTimeout.stop();
    check(code->item(0, 3)->text().contains(QStringLiteral("original fixture")), "symbol and comment annotate code");
    address->setText(QStringLiteral("main"));
    find<QPushButton>(dialog, "debugCpuNavigate")->click();
    check(commands.last().kind == DebuggerCommand::Navigate && commands.last().address == 0x102,
          "symbol navigation resolves imported name");
    if (!previews.isEmpty()) {
        QApplication::processEvents();
        check(dialog.grab().save(QDir(previews).filePath(QStringLiteral("cpu.png"))),
              "save code preview");
    }

    ++snapshot.generation;
    dialog.setSnapshot(snapshot);
    check(code->item(0, 3)->text().contains(QStringLiteral("original fixture")), "reset retains same-game symbols");
    auto nextGame = snapshot;
    ++nextGame.generation;
    nextGame.gameKey = QStringLiteral("second.md\nMD fixture 2");
    dialog.setSnapshot(nextGame);
    check(code->item(0, 3)->text().isEmpty(), "symbols clear on game change");

    DebuggerSnapshot sms;
    sms.attached = true;
    sms.generation = nextGame.generation + 1;
    sms.gameKey = QStringLiteral("fixture.sms\nSMS fixture");
    sms.selectedCpu = KR_DEBUG_Z80;
    sms.cpus.append(snapshot.cpus.last());
    dialog.setSnapshot(sms);
    check(!cpu->isVisible() && dialog.refreshCommand().cpu == KR_DEBUG_Z80,
          "eight-bit system selects its only CPU automatically");
    check(registers->item(2, 1)->text().contains(QStringLiteral("Z=1")), "Z80 AF flags");
    dialog.hide();
    check(commands.last().kind == DebuggerCommand::Detach && code->rowCount() == 0 &&
          registers->rowCount() == 0 && stack->toPlainText().isEmpty(), "closing detaches and clears snapshot");
}
} // namespace

int main(int argc, char** argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    const QString previews = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!previews.isEmpty()) check(QDir().mkpath(previews), "create preview directory");
    verify(previews);
    std::puts("PASS: CPU debugger commands, stopped CPU, range breakpoints, disassembly and symbol navigation");
}
