#include "i18n/Localization.h"
#include "ui/DebugWatchDialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QTableView>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

namespace {
constexpr int valueColumn = 4;
constexpr int previousColumn = 5;
constexpr int changesColumn = 6;

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

template<typename T>
T* find(QObject& parent, const char* name) {
    auto* widget = parent.findChild<T*>(QString::fromLatin1(name));
    check(widget, name);
    return widget;
}

DebugWatchEntry entry(quint32 id, quint32 offset, quint32 width, DebugWatchEntry::Format format) {
    DebugWatchEntry result;
    result.id = id;
    result.offset = offset;
    result.width = width;
    result.format = format;
    return result;
}

DebugWatchDomain ram(quint32 instance) {
    DebugWatchDomain domain;
    domain.instance = instance;
    domain.native.id = KR_MEM_RAM;
    domain.native.size = 65536;
    domain.native.big_endian = 1;
    qstrncpy(domain.native.name, "Work RAM", sizeof(domain.native.name));
    return domain;
}
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    const QString previewPath = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!previewPath.isEmpty()) Localization::initialize(QApplication::applicationDirPath(), {});
    DebugWatchDialog dialog;
    int subscriptions = 0;
    QObject::connect(&dialog, &DebugWatchDialog::inspectionChanged, &dialog, [&] { ++subscriptions; });
    dialog.show();
    QApplication::processEvents();
    check(subscriptions == 1 && !dialog.isModal(), "Opening only subscribes to watch snapshots");
    auto* table = find<QTableView>(dialog, "debugWatchTable");
    auto* model = table->model();
    auto value = [model](int row, int column) { return model->index(row, column).data().toString(); };
    auto* signedEntry = "Enemy health";
    auto s16 = entry(10, 0xa0, 2, DebugWatchEntry::Signed);
    s16.name = QString::fromLatin1(signedEntry);
    s16.instance = 1;
    auto u32 = entry(20, 0xc2, 4, DebugWatchEntry::Hex);
    u32.name = QStringLiteral("Position");
    u32.order = DebugWatchEntry::Little;
    auto bits = entry(30, 0xd4, 1, DebugWatchEntry::Binary);
    bits.name = QStringLiteral("Object flags");
    auto turns = entry(40, 0xe0, 1, DebugWatchEntry::Unsigned);
    turns.name = QStringLiteral("Animation");
    dialog.setEntries({s16, u32, bits, turns});

    DebugInspectionSnapshot snapshot;
    snapshot.info.version = KR_INSPECT_VERSION;
    snapshot.generation = 2;
    snapshot.gameKey = QStringLiteral("Watch fixture");
    snapshot.watchDomains = {ram(0), ram(1)};
    snapshot.watchValues = {
        {s16.id, QByteArray::fromHex("ff80"), true},
        {u32.id, QByteArray::fromHex("78563412"), true},
        {bits.id, QByteArray::fromHex("a5"), true},
        {turns.id, QByteArray::fromHex("09"), true}
    };
    int changed = 0;
    QObject::connect(model, &QAbstractItemModel::dataChanged, &dialog, [&] { ++changed; });
    dialog.setSnapshot(snapshot);
    check(value(0, valueColumn) == QStringLiteral("-128"), "Native signed 16-bit decode");
    check(value(1, valueColumn) == QStringLiteral("0x12345678"), "Forced little-endian 32-bit decode");
    check(value(2, valueColumn) == QStringLiteral("0b10100101") &&
          value(3, valueColumn) == QStringLiteral("9"), "Binary and unsigned formatting");
    check(value(0, previousColumn) == QStringLiteral("—") && value(0, changesColumn) == QStringLiteral("0"),
          "First sample starts each baseline");

    changed = 0;
    ++snapshot.frame;
    dialog.setSnapshot(snapshot);
    check(changed == 0, "Same samples on a later frame cause no model update");
    snapshot.watchValues[0].bytes = QByteArray::fromHex("0008");
    dialog.setSnapshot(snapshot);
    check(changed == 1 && value(0, valueColumn) == QStringLiteral("8") &&
          value(0, previousColumn) == QStringLiteral("-128") && value(0, changesColumn) == QStringLiteral("1"),
          "One changed sample updates only its value/history cells");
    check(model->index(0, valueColumn).data(Qt::BackgroundRole).isValid(), "Changed sample highlighted");
    if (!previewPath.isEmpty()) {
        QApplication::processEvents();
        check(dialog.grab().save(previewPath), "Save RAM Watch preview");
    }
    dialog.setSnapshot(snapshot);
    changed = 0;
    dialog.setSnapshot(snapshot);
    check(changed == 0 && value(0, changesColumn) == QStringLiteral("1") &&
          value(0, previousColumn) == QStringLiteral("-128"), "Identical values retain the last differing value");

    auto* open = find<QPushButton>(dialog, "debugWatchMemory");
    table->selectRow(0);
    quint32 navigatedInstance = 0, navigatedDomain = 0, navigatedOffset = 0;
    QObject::connect(&dialog, &DebugWatchDialog::navigateRequested, &dialog,
                     [&](quint32 instance, quint32 domain, quint32 offset) {
        navigatedInstance = instance;
        navigatedDomain = domain;
        navigatedOffset = offset;
    });
    open->click();
    check(navigatedInstance == 1 && navigatedDomain == KR_MEM_RAM && navigatedOffset == s16.offset,
          "Open Memory uses the watched console and physical region");

    auto* clearHistory = find<QPushButton>(dialog, "debugWatchClearHistory");
    clearHistory->click();
    check(value(0, valueColumn) == QStringLiteral("8") && value(0, previousColumn) == QStringLiteral("—") &&
          value(0, changesColumn) == QStringLiteral("0"), "Clear history retains values and entries");
    snapshot.watchValues[0].bytes = QByteArray::fromHex("0009");
    dialog.setSnapshot(snapshot);
    ++snapshot.generation;
    snapshot.watchValues[0].bytes = QByteArray::fromHex("000d");
    dialog.setSnapshot(snapshot);
    check(value(0, valueColumn) == QStringLiteral("13") && value(0, changesColumn) == QStringLiteral("0"),
          "A new generation resets sampled change history");

    snapshot.watchValues[0].bytes.clear();
    snapshot.watchDomains = {ram(0)};
    dialog.setSnapshot(snapshot);
    check(value(0, valueColumn) == QStringLiteral("—") && !open->isEnabled(), "Unavailable console clears stale data");
    snapshot.watchDomains.append(ram(1));
    snapshot.watchValues[0].bytes = QByteArray::fromHex("000e");
    dialog.setSnapshot(snapshot);
    check(value(0, changesColumn) == QStringLiteral("0"), "Returning from an unavailable sample starts a baseline");

    auto* edit = find<QPushButton>(dialog, "debugWatchEdit");
    QTimer::singleShot(0, &dialog, [&] {
        auto* active = QApplication::activeModalWidget();
        check(active != nullptr, "Open watch editor");
        auto* name = find<QLineEdit>(*active, "debugWatchEditorName");
        name->setText(QStringLiteral("Health"));
        active->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    edit->click();
    check(dialog.entries().front().name == QStringLiteral("Health") &&
          dialog.entries().front().id == s16.id, "Editing retains the watch identity");

    const int beforeHide = subscriptions;
    dialog.hide();
    dialog.setSnapshot(snapshot);
    check(subscriptions == beforeHide + 1 && value(0, valueColumn) == QStringLiteral("—") && dialog.entries().size() == 4,
          "Hiding unsubscribes and clears samples, preserving saved watches");

    QVector<DebugWatchEntry> tooMany(DebugWatchLimit + 5, s16);
    auto invalid = s16;
    invalid.width = 3;
    tooMany.prepend(invalid);
    dialog.setEntries(tooMany);
    const auto bounded = dialog.entries();
    QSet<quint32> ids;
    for (const auto& item : bounded) ids.insert(item.id);
    check(bounded.size() == DebugWatchLimit && ids.size() == DebugWatchLimit && !ids.contains(0),
          "Restored watch lists enforce valid widths, unique IDs and capacity");
    dialog.addWatch(s16);
    check(dialog.entries().size() == DebugWatchLimit, "Add respects the watch limit");
    std::puts("debug_watch_view_tests passed");
}
