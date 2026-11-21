#include "ui/TraceDialog.h"
#include "i18n/Localization.h"

#include <QApplication>
#include <QComboBox>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTableView>

#include <cstdio>
#include <cstdlib>

namespace {
void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

template<typename T>
T* find(TraceDialog& dialog, const char* name) {
    auto* widget = dialog.findChild<T*>(QString::fromLatin1(name));
    check(widget, name);
    return widget;
}

kr_debug_target target(quint32 cpu, const char* name) {
    kr_debug_target result{};
    result.cpu = cpu;
    result.address_bits = 16;
    qstrncpy(result.name, name, sizeof(result.name));
    return result;
}
}

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    const QString previewPath = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!previewPath.isEmpty())
        Localization::initialize(QApplication::applicationDirPath(), {});
    TraceDialog dialog;
    QVector<DebugTraceCommand> commands;
    QObject::connect(&dialog, &TraceDialog::commandRequested, &dialog,
                     [&](const DebugTraceCommand& command) { commands.append(command); });
    quint32 navigationCpu = 0;
    quint32 navigationAddress = 0;
    QObject::connect(&dialog, &TraceDialog::navigateRequested, &dialog, [&](quint32 cpu, quint32 address) {
        navigationCpu = cpu;
        navigationAddress = address;
    });
    dialog.show();
    QApplication::processEvents();
    check(commands.size() == 1 && commands.back().kind == DebugTraceCommand::Refresh,
          "Opening trace should subscribe without starting capture");
    auto* cpu = find<QComboBox>(dialog, "traceCpu");
    auto* start = find<QPushButton>(dialog, "traceStart");
    auto* stop = find<QPushButton>(dialog, "traceStop");
    auto* clear = find<QPushButton>(dialog, "traceClear");
    auto* table = find<QTableView>(dialog, "traceEvents");
    auto* model = table->model();
    int resets = 0;
    QObject::connect(model, &QAbstractItemModel::modelReset, &dialog, [&] { ++resets; });

    const quint32 peerCpu = (1u << 8) | KR_DEBUG_Z80;
    DebugTraceSnapshot snapshot;
    snapshot.available = true;
    snapshot.filters = KR_TRACE_INSTRUCTION | KR_TRACE_DMA;
    snapshot.targets = {target(KR_DEBUG_Z80, "Z80"), target(peerCpu, "Z80")};
    dialog.setSnapshot(snapshot);
    check(cpu->count() == 3 && cpu->currentData().toUInt() == 0, "All and both Link targets");
    cpu->setCurrentIndex(cpu->findData(peerCpu));
    find<QSpinBox>(dialog, "traceMemoryMiB")->setValue(8);
    start->click();
    check(commands.back().kind == DebugTraceCommand::Start && commands.back().config.cpu == peerCpu
          && commands.back().config.memoryMiB == 8 && commands.back().config.filters == snapshot.filters,
          "Start must use available event filters and selected target and buffer");

    snapshot.active = true;
    snapshot.canNavigate = true;
    snapshot.captureId = 1;
    snapshot.total = 260;
    snapshot.capacity = 259;
    snapshot.overwritten = 1;
    snapshot.recordTargets = snapshot.targets;
    for (quint64 sequence = 1; sequence <= snapshot.total; ++sequence) {
        DebugTraceRow row;
        row.record.sequence = sequence;
        row.record.instance = 1;
        row.record.native.cpu = KR_DEBUG_Z80;
        row.record.native.kind = KR_TRACE_EXEC;
        row.record.frame = 18 + sequence / 64;
        row.record.native.mclk = 1600 + sequence * 72;
        row.record.native.address = sequence == snapshot.total ? 0x1234 : 0x1000 + sequence * 2;
        row.record.native.valid_bytes = 16;
        row.record.native.bytes[0] = 0x3e;
        row.record.native.bytes[1] = 0x55;
        row.instructionBytes = 2;
        row.instruction = QStringLiteral("LD A,$55");
        snapshot.rows.append(row);
    }
    auto& hardware = snapshot.rows[snapshot.rows.size() - 2];
    hardware.record.native.kind = KR_TRACE_DMA_START;
    hardware.record.native.device = KR_TRACE_VDP;
    hardware.record.native.address = 0x9400;
    hardware.record.native.value = 0x0094;
    hardware.record.native.width = 2;
    hardware.record.native.aux = 0x0040;

    dialog.setSnapshot(snapshot);
    check(model->rowCount() == 256 && stop->isEnabled() && !start->isEnabled(), "Bounded active trace view");
    check(model->index(255, 5).data().toString() == QStringLiteral("3E 55"), "Show decoded length only");
    check(model->index(254, 2).data().toString().contains(QStringLiteral("VDP")), "Hardware device attribution");
    const int activeResets = resets;
    dialog.setSnapshot(snapshot);
    check(resets == activeResets, "Unchanged events must not reset the model");
    table->doubleClicked(model->index(254, 4));
    check(navigationCpu == 0, "Hardware addresses cannot navigate to a CPU instruction");
    table->doubleClicked(model->index(255, 4));
    check(navigationCpu == peerCpu && navigationAddress == 0x1234, "Navigate to peer instruction");

    if (!previewPath.isEmpty()) {
        QApplication::processEvents();
        table->scrollToBottom();
        QApplication::processEvents();
        check(dialog.grab().save(previewPath), "Save trace preview");
    }

    snapshot.active = false;
    snapshot.canNavigate = false;
    snapshot.targets.clear();
    snapshot.available = false;
    dialog.setSnapshot(snapshot);
    check(model->index(255, 2).data().toString().contains(QStringLiteral("Z80")), "Retained capture keeps CPU names");
    navigationCpu = 0;
    table->doubleClicked(model->index(255, 4));
    check(navigationCpu == 0, "Detached capture cannot navigate into another game");
    ++snapshot.captureId;
    dialog.setSnapshot(snapshot);
    check(resets == activeResets + 1, "New capture resets reused sequence numbers");
    clear->click();
    check(commands.back().kind == DebugTraceCommand::Clear, "Clear recent trace");
    dialog.close();
    check(commands.back().kind == DebugTraceCommand::Stop && model->rowCount() == 0,
          "Closing stops capture and releases visible rows");
    std::puts("trace_view_tests passed");
}
