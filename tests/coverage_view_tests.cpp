#include "core/DebugInspectionAbi.h"
#include "i18n/Localization.h"
#include "ui/CoverageDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
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
T* find(CoverageDialog& dialog, const char* name) {
    auto* widget = dialog.findChild<T*>(QString::fromLatin1(name));
    check(widget, name);
    return widget;
}

DebugCoverageSource source(quint32 id, quint32 size, const char* name) {
    DebugCoverageSource result;
    result.native.id = id;
    result.native.size = size;
    qstrncpy(result.native.name, name, sizeof(result.native.name));
    return result;
}

kr_debug_target target(quint32 id, const char* name) {
    kr_debug_target result{};
    result.cpu = id;
    qstrncpy(result.name, name, sizeof(result.name));
    return result;
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString previewPath = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!previewPath.isEmpty())
        Localization::initialize(QApplication::applicationDirPath(), {});
    CoverageDialog dialog;
    QVector<DebugCoverageCommand> commands;
    QObject::connect(&dialog, &CoverageDialog::commandRequested, &dialog,
                     [&](const DebugCoverageCommand& command) { commands.append(command); });
    quint32 navigatedInstance = 0;
    quint32 navigatedDomain = 0;
    quint32 navigatedAddress = 0;
    QObject::connect(&dialog, &CoverageDialog::navigateRequested, &dialog, [&](quint32 instance, quint32 domain, quint32 offset) {
        navigatedInstance = instance;
        navigatedDomain = domain;
        navigatedAddress = offset;
    });

    dialog.show();
    QApplication::processEvents();
    check(commands.size() == 1 && commands.last().kind == DebugCoverageCommand::Refresh, "Show only refreshes");
    auto* consoles = find<QComboBox>(dialog, "coverageConsole");
    auto* cpus = find<QComboBox>(dialog, "coverageCpu");
    auto* sources = find<QComboBox>(dialog, "coverageSource");
    auto* offset = find<QLineEdit>(dialog, "coverageOffset");
    auto* start = find<QPushButton>(dialog, "coverageStart");
    auto* clear = find<QPushButton>(dialog, "coverageClear");
    auto* memory = find<QPushButton>(dialog, "coverageOpenMemory");
    auto* reads = find<QCheckBox>(dialog, "coverageReads");
    auto* table = find<QTableView>(dialog, "coveragePage");
    auto* model = table->model();
    int changes = 0;
    int resets = 0;
    QObject::connect(model, &QAbstractItemModel::dataChanged, &dialog, [&] { ++changes; });
    QObject::connect(model, &QAbstractItemModel::modelReset, &dialog, [&] { ++resets; });

    DebugCoverageSnapshot snapshot;
    snapshot.available = true;
    snapshot.instances = {0, 1};
    snapshot.targets = {target(KR_DEBUG_M68K, "68000"), target(KR_DEBUG_Z80, "Z80")};
    snapshot.execCpus = KR_DEBUG_M68K | KR_DEBUG_Z80;
    snapshot.readCpus = KR_DEBUG_M68K;
    dialog.setSnapshot(snapshot);
    check(cpus->count() == 3 && cpus->currentData().toUInt() == 0, "All and actual CPU targets");
    consoles->setCurrentIndex(consoles->findData(1u));
    check(commands.last().kind == DebugCoverageCommand::Refresh && commands.last().instance == 1,
          "Console change refreshes available CPUs");
    check(!start->isEnabled(), "Cannot start with stale console capabilities");

    snapshot.instance = 1;
    dialog.setSnapshot(snapshot);
    cpus->setCurrentIndex(cpus->findData(quint32(KR_DEBUG_Z80)));
    check(!reads->isEnabled() && !reads->isChecked(), "Unavailable read coverage is disabled for selected CPU");
    start->click();
    const auto config = commands.last().config;
    check(commands.last().kind == DebugCoverageCommand::Start && config.instance == 1 &&
          config.cpu == KR_DEBUG_Z80 && config.execution && !config.reads, "Selected console, CPU and modes");
    snapshot.hasCapture = snapshot.active = snapshot.canNavigate = true;
    snapshot.captureId = 11;
    snapshot.capturedInstance = 1;
    snapshot.game = QStringLiteral("Coverage fixture");
    snapshot.allocatedBytes = 0x10100;
    snapshot.unknownCode = 32;
    snapshot.sources = {source(1, 0x40000, "Cartridge ROM"), source(2, 31, "Work RAM")};
    snapshot.sources[0].code = 399;
    snapshot.sources[0].data = 85;
    snapshot.sources[0].both = 85;
    snapshot.sources[0].native.inspect_domain = KR_MEM_ROM;
    snapshot.sources[1].native.flags = KR_COV_SOURCE_PARTIAL;
    snapshot.source = 1;
    snapshot.inspectDomain = KR_MEM_ROM;
    snapshot.inspectOffset = 0x400;
    snapshot.page = QByteArray(256, '\0');
    for (int i = 0; i < 89; ++i)
        snapshot.page[i] = i % 4 == 0 ? 3 : i % 7 == 0 ? 2 : 1;
    dialog.setSnapshot(snapshot);
    check(model->rowCount() == 16 && model->columnCount() == 17, "One 256-byte page");
    check(model->index(0, 1).data().toString() == QLatin1String("B") &&
          model->index(0, 2).data().toString() == QLatin1String("C") &&
          model->index(0, 8).data().toString() == QLatin1String("R") &&
          model->index(15, 16).data().toString() == QLatin1String("U"), "Distinct coverage marks");
    const int previousResets = resets;
    dialog.setSnapshot(snapshot);
    check(previousResets == resets && changes == 0, "Identical snapshot does no model updates");
    snapshot.page[200] = 1;
    ++snapshot.revision;
    dialog.setSnapshot(snapshot);
    check(previousResets == resets && changes == 1, "One notification for modified page");

    table->setCurrentIndex(model->index(1, 3));
    memory->click();
    check(navigatedInstance == 1 && navigatedDomain == KR_MEM_ROM && navigatedAddress == 0x412,
          "Navigate exact physical offset on captured console");
    if (!previewPath.isEmpty()) {
        QApplication::processEvents();
        check(dialog.grab().save(previewPath), "Save coverage preview");
    }

    clear->click();
    check(commands.last().kind == DebugCoverageCommand::Clear && commands.last().captureId == 11,
          "Clear targets current capture without stopping it");
    snapshot.active = false;
    dialog.setSnapshot(snapshot);
    consoles->setCurrentIndex(consoles->findData(0u));
    navigatedInstance = 0;
    memory->click();
    check(navigatedInstance == 1, "Changing next capture console preserves current capture navigation");
    snapshot.instance = 0;
    dialog.setSnapshot(snapshot);
    offset->setText(QStringLiteral("0x101"));
    offset->returnPressed();
    check(commands.last().kind == DebugCoverageCommand::Refresh && commands.last().offset == 0x100,
          "Offset navigation requests aligned physical page");

    sources->setCurrentIndex(sources->findData(2u));
    check(dialog.refreshCommand().source == 2 && dialog.refreshCommand().offset == 0,
          "Source navigation resets its offset");
    dialog.setSnapshot(snapshot);
    check(model->rowCount() == 0 && !memory->isEnabled(), "A late page cannot show or navigate the wrong source");
    snapshot.source = 2;
    snapshot.inspectDomain = 0;
    snapshot.page = QByteArray(31, '\0');
    dialog.setSnapshot(snapshot);
    check(model->rowCount() == 2 && !model->index(1, 16).data().isValid() && !memory->isEnabled(),
          "Partial last row and unavailable physical mapping");
    dialog.close();
    check(commands.last().kind == DebugCoverageCommand::Stop && model->rowCount() == 0,
          "Hide stops capture and releases page");
    std::puts("coverage_view_tests passed");
}
