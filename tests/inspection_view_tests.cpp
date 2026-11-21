#include "ui/DebugDialogs.h"
#include "ui/DebugInspectorWidgets.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QItemSelectionModel>
#include <QShortcut>
#include <QBrush>
#include <QComboBox>
#include <QDir>
#include <QImage>
#include <QInputDialog>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QLabel>
#include <QHeaderView>
#include <QTabWidget>
#include <QTableView>
#include <QScrollBar>
#include <QTableWidget>
#include <QtEndian>

#include <cstdio>
#include <cstdlib>

namespace {
void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

template<typename T>
T* named(QWidget& widget, const char* name) {
    auto* child = widget.findChild<T*>(QString::fromLatin1(name));
    check(child != nullptr, name);
    return child;
}

DebugInspectionSnapshot snapshot(quint32 part, quint32 hardware, quint32 format) {
    DebugInspectionSnapshot result;
    result.generation = 1;
    result.parts = part;
    result.info.version = KR_INSPECT_VERSION;
    result.info.hardware = hardware;
    result.info.video_format = format;
    result.info.height = hardware & KR_HW_MD ? 224 : 192;
    qstrncpy(result.info.system, "Fixture", sizeof(result.info.system));
    return result;
}

void domain(DebugInspectionSnapshot& snapshot, quint32 id, quint32 size,
            const char* name, bool bigEndian = false) {
    auto& item = snapshot.info.domains[snapshot.info.domain_count++];
    item.id = id;
    item.size = size;
    item.big_endian = bigEndian;
    qstrncpy(item.name, name, sizeof(item.name));
}

void value(DebugInspectionSnapshot& snapshot, quint32 part, const char* device,
           const char* name, quint64 data, quint32 digits = 0) {
    auto& entry = snapshot.info.values[snapshot.info.value_count++];
    entry.part = part;
    entry.digits = digits;
    entry.value = data;
    qstrncpy(entry.device, device, sizeof(entry.device));
    qstrncpy(entry.name, name, sizeof(entry.name));
}

void putWord(QByteArray& bytes, int address, quint16 value) {
    qToBigEndian(value, bytes.data() + address);
}

void verifyWindow(const QDialog& window) {
    check(!window.isModal(), "inspection windows are modeless");
    check(window.windowFlags().testFlag(Qt::WindowCloseButtonHint), "close button available");
    check(!window.windowFlags().testFlag(Qt::WindowMinimizeButtonHint) &&
          !window.windowFlags().testFlag(Qt::WindowMaximizeButtonHint), "no minimize/maximize buttons");
}

void verifyMemory(const QString& previewDirectory) {
    DebugMemoryDialog dialog;
    verifyWindow(dialog);
    check(dialog.request().parts == 0, "hidden memory does not subscribe");
    dialog.show();
    check(dialog.request().parts == KR_INSPECT_MEMORY, "memory subscribes on show");

    auto data = snapshot(KR_INSPECT_MEMORY, KR_HW_MD, KR_VIDEO_MD);
    domain(data, KR_MEM_ROM, 8192, "ROM", true);
    domain(data, KR_MEM_RAM, 0x10000, "RAM", true);
    data.memoryDomain = KR_MEM_RAM;
    data.memory = QByteArray(259, '\0');
    putWord(data.memory, 255, 0x1234);
    dialog.setSnapshot(data);
    auto* hex = named<QTableView>(dialog, "debugMemoryHex");
    auto* number = named<QLabel>(dialog, "debugMemoryValue");
    auto* width = named<QComboBox>(dialog, "debugMemoryWidth");
    auto* order = named<QComboBox>(dialog, "debugMemoryByteOrder");
    dialog.navigateTo(KR_MEM_RAM, 255);
    width->setCurrentIndex(width->findData(2));
    check(number->text().contains(QStringLiteral("0x1234")), "watch reads across page boundary");
    order->setCurrentIndex(order->findData(1));
    check(number->text().contains(QStringLiteral("0x3412")), "watch little-endian override");
    data.memory[255] = char(0xff);
    data.memory[256] = char(0xff);
    dialog.setSnapshot(data);
    width->setCurrentIndex(width->findData(-2));
    check(number->text().contains(QStringLiteral("(-1)")), "signed watch");
    check(hex->model()->index(15, 15).data(Qt::BackgroundRole).value<QBrush>().style() != Qt::NoBrush, "changed byte highlighted");
    check(hex->editTriggers() == QAbstractItemView::NoEditTriggers, "page is read-only");
    if (!previewDirectory.isEmpty()) {
        QApplication::processEvents();
        check(dialog.grab().save(QDir(previewDirectory).filePath(QStringLiteral("memory.png"))),
              "save memory preview");
    }

    auto* tabs = named<QTabWidget>(dialog, "debugMemoryTabs");
    tabs->setCurrentIndex(1);
    check(dialog.request().parts == KR_INSPECT_HARDWARE, "hardware tab drops memory subscription");
    auto hardware = snapshot(KR_INSPECT_HARDWARE, KR_HW_MD, KR_VIDEO_MD);
    value(hardware, KR_INSPECT_HARDWARE, "Mapper", "Bank", 4);
    dialog.setSnapshot(hardware);
    auto* state = named<QTableWidget>(dialog, "debugHardwareState");
    check(state->rowCount() == 1 && state->item(0, 2)->text() == QStringLiteral("4"), "hardware value");
    dialog.navigateTo(KR_MEM_RAM, 0x234);
    check(tabs->currentIndex() == 0 && dialog.request().offset == 0x200, "memory link navigates to page");

    auto blocked = DebugInspectionSnapshot{};
    blocked.status = QStringLiteral("Hardcore");
    dialog.setSnapshot(blocked);
    check(number->text() == QStringLiteral("—") && hex->model()->index(15, 15).data(Qt::DisplayRole).toString().isEmpty(), "blocked data cleared");
    auto sms = snapshot(KR_INSPECT_MEMORY, KR_HW_SMS, KR_VIDEO_SMS);
    sms.generation = 2;
    domain(sms, KR_MEM_ROM, 8192, "ROM");
    domain(sms, KR_MEM_ZRAM, 8192, "CPU RAM");
    dialog.setSnapshot(sms);
    check(dialog.request().domain == KR_MEM_ZRAM && dialog.request().offset == 0, "eight-bit RAM fallback");
    dialog.hide();
    check(dialog.request().parts == 0, "closing memory drops subscription");
}

void verifyMemoryRefresh() {
    DebugMemoryDialog dialog;
    dialog.resize(640, 380);
    auto* view = named<QTableView>(dialog, "debugMemoryHex");
    auto* model = view->model();
    int dataChanges = 0, headerChanges = 0, requests = 0;
    QModelIndex firstChanged, lastChanged;
    QObject::connect(model, &QAbstractItemModel::dataChanged, &dialog,
                     [&](const QModelIndex& first, const QModelIndex& last) {
        ++dataChanges;
        firstChanged = first;
        lastChanged = last;
    });
    QObject::connect(model, &QAbstractItemModel::headerDataChanged, &dialog, [&] { ++headerChanges; });
    QObject::connect(&dialog, &DebugMemoryDialog::inspectionChanged, &dialog, [&] { ++requests; });
    dialog.show();
    QApplication::processEvents();
    check(requests == 1 && dataChanges == 0, "opening empty memory view sends one request and changes no cells");

    auto data = snapshot(KR_INSPECT_MEMORY, KR_HW_SMS, KR_VIDEO_SMS);
    domain(data, KR_MEM_ZRAM, 8192, "Z80 RAM");
    dialog.setSnapshot(data);
    check(requests == 2 && dialog.request().domain == KR_MEM_ZRAM,
          "first catalog immediately requests bytes from fallback domain");
    dialog.setSnapshot(data);
    check(requests == 2 && dataChanges == 0, "unchanged fallback does not loop or mutate blank cells");

    data.memoryDomain = KR_MEM_ZRAM;
    data.memory = QByteArray(259, char(0));
    dialog.setSnapshot(data);
    check(dataChanges == 1 && headerChanges == 1 && requests == 2, "first page updates the model once");
    dialog.navigateTo(KR_MEM_ZRAM, 254);
    QApplication::processEvents();
    const auto selection = view->currentIndex();
    check(view->visualRect(selection).bottom() <= view->viewport()->rect().bottom(),
          "navigation brings the whole selected row into view");
    const int scroll = view->verticalScrollBar()->value();

    ++data.frame;
    dialog.setSnapshot(data);
    check(dataChanges == 1 && headerChanges == 1 && requests == 2,
          "same bytes on a new frame emit no table changes");

    data.memory[75] = char(0xa5);
    dialog.setSnapshot(data);
    check(dataChanges == 2 && firstChanged.row() == 4 && lastChanged.row() == 4,
          "one changed byte repaints one row including ASCII");
    check(model->index(4, 11).data(Qt::BackgroundRole).isValid(), "changed byte highlighted");
    data.memory.fill(char(0x36));
    dialog.setSnapshot(data);
    check(dataChanges == 3 && firstChanged.row() == 0 && lastChanged.row() == 15,
          "full page of changing RAM is one model update");
    check(view->currentIndex() == selection && view->verticalScrollBar()->value() == scroll,
          "memory refresh preserves selection and scroll");
    dialog.setSnapshot(data);
    check(dataChanges == 4 && !model->index(4, 11).data(Qt::BackgroundRole).isValid(),
          "highlight clears once on an unchanged snapshot");
    dialog.setSnapshot(data);
    check(dataChanges == 4 && headerChanges == 1 && requests == 2,
          "subsequent unchanged page stays idle");

    const int previousWidth = view->horizontalHeader()->sectionSize(0);
    auto larger = view->font();
    larger.setPixelSize(view->fontMetrics().height() + 6);
    view->setFont(larger);
    QApplication::processEvents();
    check(view->horizontalHeader()->sectionSize(0) > previousWidth, "hex width follows a font size change");
}

void verifyMemoryEditing() {
    DebugMemoryDialog dialog;
    dialog.show();
    auto data = snapshot(KR_INSPECT_MEMORY, KR_HW_MD, KR_VIDEO_MD);
    domain(data, KR_MEM_RAM, 0x10000, "RAM", true);
    data.memoryDomain = KR_MEM_RAM;
    data.memory = QByteArray(259, '\0');
    data.memory[0] = char(0xaa);
    data.memory[1] = char(0xbb);
    data.writableDomains = {KR_MEM_RAM};
    data.instanceCount = 2;
    dialog.setSnapshot(data);
    auto* edit = named<QPushButton>(dialog, "debugMemoryEdit");
    auto* instance = named<QComboBox>(dialog, "debugMemoryInstance");
    check(!edit->isEnabled() && instance->isVisible(), "running memory is read-only and both consoles selectable");

    instance->setCurrentIndex(instance->findData(1u));
    check(dialog.request().instance == 1, "memory request selects console B");
    data.instance = 1;
    data.canEdit = true;
    dialog.setSnapshot(data);
    auto* width = named<QComboBox>(dialog, "debugMemoryWidth");
    auto* order = named<QComboBox>(dialog, "debugMemoryByteOrder");
    width->setCurrentIndex(width->findData(2));
    order->setCurrentIndex(order->findData(1));
    check(edit->isEnabled(), "paused writable domain permits edit");

    DebugWatchEntry watch;
    QObject::connect(&dialog, &DebugMemoryDialog::watchRequested, &dialog,
                     [&](const DebugWatchEntry& entry) { watch = entry; });
    named<QPushButton>(dialog, "debugMemoryAddWatch")->click();
    check(watch.instance == 1 && watch.domain == KR_MEM_RAM && watch.offset == 0 && watch.width == 2 &&
          watch.order == DebugWatchEntry::Little && watch.format == DebugWatchEntry::Unsigned,
          "memory adds watch for selected physical bytes, signedness and order");
    auto* hexView = named<QTableView>(dialog, "debugMemoryHex");
    const QItemSelection firstTwo(hexView->model()->index(0, 0), hexView->model()->index(0, 1));
    hexView->selectionModel()->select(firstTwo, QItemSelectionModel::ClearAndSelect);
    auto* copyShortcut = dialog.findChild<QShortcut*>(QStringLiteral("debugMemoryCopy"));
    check(copyShortcut != nullptr, "memory copy shortcut");
    emit copyShortcut->activated();
    check(QApplication::clipboard()->text() == QStringLiteral("AA BB"), "copy selected raw bytes in address order");

    DebugMemoryWrite write;
    QObject::connect(&dialog, &DebugMemoryDialog::writeRequested, &dialog,
                     [&](const DebugMemoryWrite& request) { write = request; });
    QTimer::singleShot(0, &dialog, [] {
        auto* editor = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        check(editor != nullptr, "open memory hex editor");
        editor->setTextValue(QStringLiteral("1234"));
        editor->accept();
    });
    edit->click();
    check(write.generation == data.generation && write.instance == 1 && write.domain == KR_MEM_RAM &&
          write.offset == 0 && write.before == QByteArray::fromHex("aabb") &&
          write.after == QByteArray::fromHex("3412"), "bounded little-endian edit carries expected bytes and console");
    check(!edit->isEnabled(), "edit unavailable until acknowledgement");
    dialog.finishMemoryWrite({});
    check(!edit->isEnabled(), "successful write waits for a fresh snapshot");

    auto detached = DebugInspectionSnapshot{};
    detached.serial = 4;
    detached.instance = 1;
    detached.instanceCount = 1;
    dialog.setSnapshot(detached);
    check(dialog.request().instance == 0 && !instance->isVisible(), "peer teardown clears memory selection");

    data.instance = 0;
    dialog.setSnapshot(data);
    dialog.navigateTo(1, KR_MEM_ROM, 0x21035);
    check(dialog.request().instance == 1 && dialog.request().domain == KR_MEM_ROM &&
          dialog.request().offset == 0x21000 && instance->currentData().toUInt() == 1,
          "physical navigation switches console and keeps offset beyond the old domain size");
    dialog.setSnapshot(data);
    check(dialog.request().offset == 0x21000, "old console snapshot can't overwrite requested physical address");
    data.instance = 1;
    domain(data, KR_MEM_ROM, 0x40000, "ROM", true);
    data.memoryDomain = KR_MEM_ROM;
    data.memoryOffset = 0x21000;
    dialog.setSnapshot(data);
    check(dialog.request().domain == KR_MEM_ROM && dialog.request().offset == 0x21000,
          "peer snapshot resolves the requested physical region and page");
}

QRgb pixel(DebugVideoInspector& view, QPoint at) {
    auto* image = named<QWidget>(view, "debugVideoSnapshotImage");
    check(image->rect().contains(at), "preview bounds");
    return image->grab(QRect(at, QSize(1, 1))).toImage().pixel(0, 0);
}

void verifyGraphics(const QString& previewDirectory) {
    DebugGraphicsDialog window;
    verifyWindow(window);
    window.show();
    auto* tabs = named<QTabWidget>(window, "debugGraphicsTabs");
    check(window.request().parts == 0, "controls page does not subscribe");
    tabs->setCurrentIndex(1);
    check(window.request().parts == KR_INSPECT_VIDEO, "tiles subscribe");
    auto md = snapshot(KR_INSPECT_VIDEO, KR_HW_MD, KR_VIDEO_MD);
    auto& vram = md.blocks[KR_MEM_VRAM];
    vram = QByteArray(65536, '\0');
    for (int i = 0; i < 32; ++i) vram[i] = char(0x11);
    auto& colors = md.blocks[KR_MEM_CRAM];
    colors = QByteArray(128, '\0');
    putWord(colors, 2, 7);
    domain(md, KR_MEM_VDP_REGS, 24, "VDP registers");
    window.setSnapshot(md);
    auto* tiles = qobject_cast<DebugVideoInspector*>(tabs->currentWidget());
    check(tiles != nullptr && pixel(*tiles, {5, 5}) == qRgb(255, 0, 0), "MD packed tile and CRAM");
    quint32 domainId = 0, offset = ~quint32(0);
    QObject::connect(&window, &DebugGraphicsDialog::memoryRequested, &window,
                     [&](quint32 id, quint32 address) { domainId = id; offset = address; });
    auto* link = named<QLabel>(*tiles, "debugVideoSelection");
    check(link->text().contains(QStringLiteral("href=\"4/0\"")), "tile VRAM link");
    emit link->linkActivated(QStringLiteral("4/0"));
    check(domainId == KR_MEM_VRAM && offset == 0, "VRAM link reaches owning window");
    if (!previewDirectory.isEmpty()) {
        QApplication::processEvents();
        check(window.grab().save(QDir(previewDirectory).filePath(QStringLiteral("tiles.png"))),
              "save graphics preview");
    }

    tabs->setCurrentIndex(2);
    auto* map = qobject_cast<DebugVideoInspector*>(tabs->currentWidget());
    check(map != nullptr, "map tab exists");
    md.info.vdp_regs[2] = 0x10;
    putWord(md.blocks[KR_MEM_VRAM], 0x4000, 0x8800);
    window.setSnapshot(md);
    check(pixel(*map, {4, 4}) == qRgb(255, 0, 0), "MD map entry decodes referenced tile");
    check(named<QLabel>(*map, "debugVideoSelection")->text().contains(QStringLiteral("href=\"4/16384\"")),
          "map entry links to canonical table offset");

    tabs->setCurrentIndex(3);
    md.info.vdp_regs[5] = 0x40;
    putWord(md.blocks[KR_MEM_VRAM], 0x8000, 130);
    putWord(md.blocks[KR_MEM_VRAM], 0x8002, 0x0502);
    putWord(md.blocks[KR_MEM_VRAM], 0x8006, 140);
    window.setSnapshot(md);
    auto* sprites = named<QTableWidget>(*tabs->currentWidget(), "debugVideoSpriteList");
    check(sprites->rowCount() == 2 && sprites->item(1, 0)->text() == QStringLiteral("2"), "linked sprite order");
    check(sprites->item(0, 1)->text() == QStringLiteral("12") &&
          sprites->item(0, 3)->text() == QStringLiteral("16×16"), "sprite coordinates and dimensions");

    tabs->setCurrentIndex(5);
    window.setSnapshot(md);
    auto* registers = named<QTableWidget>(*tabs->currentWidget(), "debugVideoRegisters");
    check(registers->rowCount() == 24, "VDP registers visible");
    window.hide();
    check(window.request().parts == 0 && registers->rowCount() == 0, "hidden graphics clears data");
}

void verifyOtherFormats() {
    DebugVideoInspector tiles(DebugVideoInspector::Tiles);
    tiles.resize(360, 300);
    tiles.show();
    auto sms = snapshot(KR_INSPECT_VIDEO, KR_HW_SMS, KR_VIDEO_SMS);
    auto& vram = sms.blocks[KR_MEM_VRAM];
    vram = QByteArray(0x4000, '\0');
    for (int row = 0; row < 8; ++row) vram[row * 4] = char(0xff);
    auto& colors = sms.blocks[KR_MEM_CRAM];
    colors = QByteArray(32, '\0');
    colors[1] = char(3 << 2);
    tiles.setSnapshot(sms);
    check(pixel(tiles, {5, 5}) == qRgb(0, 255, 0), "SMS bit planes and 6-bit CRAM");
    auto gg = sms;
    gg.info.hardware = KR_HW_GG;
    gg.blocks[KR_MEM_CRAM] = QByteArray(64, '\0');
    gg.blocks[KR_MEM_CRAM][3] = char(0x0f);
    tiles.setSnapshot(gg);
    check(pixel(tiles, {5, 5}) == qRgb(0, 0, 255), "GG little-endian 12-bit CRAM");

    auto sg = snapshot(KR_INSPECT_VIDEO, KR_HW_SG, KR_VIDEO_SG);
    sg.blocks[KR_MEM_VRAM] = QByteArray(0x4000, '\0');
    sg.info.vdp_regs[1] = 8; // Multicolor: two four-pixel blocks per byte.
    sg.blocks[KR_MEM_VRAM][0] = char(0x2f);
    tiles.setSnapshot(sg);
    check(pixel(tiles, {5, 5}) == qRgb(0x21, 0xc8, 0x42), "SG Multicolor left nibble");
    check(pixel(tiles, {11, 5}) == qRgb(255, 255, 255), "SG Multicolor right nibble");
    sg.info.vdp_regs[1] = 0x10; // Text: six pixels per tile, colors from register 7.
    sg.info.vdp_regs[7] = 0xf1;
    sg.blocks[KR_MEM_VRAM].fill(char(0xff));
    tiles.setSnapshot(sg);
    check(named<QWidget>(tiles, "debugVideoSnapshotImage")->width() == 16 * 6 * 2,
          "SG Text uses six-pixel cells");

    auto firecore = snapshot(KR_INSPECT_VIDEO, KR_HW_MD | KR_HW_FIRECORE, KR_VIDEO_MD);
    firecore.info.vdp_regs[0x18] = 0x80;
    auto& patterns = firecore.blocks[KR_MEM_VRAM];
    patterns = QByteArray(0x20000, '\0');
    constexpr quint64 packed = 0x041041041041ULL; // eight pixels of color 1
    for (int row = 0; row < 8; ++row)
        for (int i = 0; i < 6; ++i)
            patterns[row * 6 + (i ^ 1)] = char((packed >> (8 * i)) & 255);
    auto& extended = firecore.blocks[KR_MEM_CRAM];
    extended = QByteArray(2048, '\0');
    extended[4] = 25;
    extended[5] = 50;
    extended[7] = 75;
    tiles.setSnapshot(firecore);
    check(pixel(tiles, {5, 5}) == qRgb(50, 25, 75), "Firecore six-bit patterns and RGB888 CRAM");
    firecore.info.video_format = KR_VIDEO_FIRECORE_BITMAP;
    tiles.setSnapshot(firecore);
    check(named<QWidget>(tiles, "debugVideoSnapshotImage")->size().isEmpty(),
          "Firecore bitmap does not show a fabricated tile image");
}

void verifyFramebuffer() {
    DebugVideoInspector view(DebugVideoInspector::Framebuffer);
    view.resize(480, 340);
    view.show();
    auto addon = snapshot(KR_INSPECT_VIDEO, KR_HW_MD | KR_HW_32X, KR_VIDEO_MD);
    value(addon, KR_INSPECT_VIDEO, "32X VDP", "Mode", 2);
    value(addon, KR_INSPECT_VIDEO, "32X VDP", "Height", 224);
    auto& page = addon.blocks[KR_MEM_32X_FB0];
    page = QByteArray(0x20000, '\0');
    putWord(page, 8, 0x100);
    putWord(page, 0x208, 31);
    view.setSnapshot(addon);
    check(pixel(view, {4, 4}) == qRgb(255, 0, 0), "32X direct-color line table");

    auto& palette = addon.blocks[KR_MEM_32X_CRAM];
    palette = QByteArray(512, '\0');
    putWord(palette, 4, 0x03e0);
    addon.info.values[0].value = 1;
    addon.blocks[KR_MEM_32X_FB0][0x204] = 2;
    view.setSnapshot(addon);
    check(pixel(view, {4, 4}) == qRgb(0, 255, 0), "32X packed pixel line");

    addon.info.values[0].value = 3;
    putWord(addon.blocks[KR_MEM_32X_FB0], 0x200, 0xff02);
    view.setSnapshot(addon);
    check(pixel(view, {4, 4}) == qRgb(0, 255, 0), "32X RLE line");
}

void verifyFirecoreBitmap() {
    DebugGraphicsDialog window;
    window.show();
    auto* tabs = named<QTabWidget>(window, "debugGraphicsTabs");
    const int tab = tabs->count() - 1;
    check(!tabs->isTabVisible(tab), "bitmap tab stays hidden without supported hardware");
    window.setControls(15, 0, false, true, QColor(), true);
    check(tabs->isTabVisible(tab) && tabs->tabText(tab).contains(QStringLiteral("Firecore")) &&
          window.request().parts == 0, "Firecore bitmap tab available without capturing on controls page");
    tabs->setCurrentIndex(tab);
    check(window.request().parts == KR_INSPECT_VIDEO, "Firecore bitmap subscribes when selected");
    auto* view = qobject_cast<DebugVideoInspector*>(tabs->currentWidget());
    check(view != nullptr, "Firecore bitmap view");
    auto* image = named<QWidget>(*view, "debugVideoSnapshotImage");
    auto* detail = named<QLabel>(*view, "debugVideoSelection");

    auto data = snapshot(KR_INSPECT_VIDEO, KR_HW_MD | KR_HW_FIRECORE, KR_VIDEO_FIRECORE_BITMAP);
    data.info.width = 256;
    data.info.vdp_regs[0x18] = 0xc0;
    data.blocks[KR_MEM_VRAM] = QByteArray(0x20000, '\0');
    constexpr int address = 0x5088;
    const auto block = QByteArray::fromHex("4523c14e"); // U=48, V=20, Y={3,9,17,28}
    data.blocks[KR_MEM_VRAM].replace(address, block.size(), block);
    window.setSnapshot(data);
    check(image->size() == QSize(256, 224) &&
          pixel(*view, {4, 2}) == qRgb(0, 23, 138) && pixel(*view, {5, 2}) == qRgb(0, 79, 194) &&
          pixel(*view, {4, 3}) == qRgb(63, 154, 255) && pixel(*view, {5, 3}) == qRgb(165, 255, 255),
          "Firecore H32 uses 320-byte stride, two chroma channels and four ordered luma values");

    const QPoint point(4, 2);
    QMouseEvent click(QEvent::MouseButtonPress, QPointF(point), QPointF(image->mapToGlobal(point)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(image, &click);
    const auto target = QStringLiteral("%1/%2").arg(KR_MEM_VRAM).arg(address);
    check(detail->text().contains(target), "selected Firecore bitmap block links to VRAM");
    quint32 memoryDomain = 0, offset = 0;
    QObject::connect(&window, &DebugGraphicsDialog::memoryRequested, &window,
                     [&](quint32 domain, quint32 address) { memoryDomain = domain; offset = address; });
    emit detail->linkActivated(target);
    check(memoryDomain == KR_MEM_VRAM && offset == address, "Firecore bitmap link reaches Memory");

    data.info.width = 320;
    data.info.height = 240;
    data.blocks[KR_MEM_VRAM].replace(0x179fc, block.size(), block);
    window.setSnapshot(data);
    check(image->size() == QSize(320, 240) && pixel(*view, {319, 239}) == qRgb(165, 255, 255),
          "Firecore H40 last bitmap block");
    data.blocks[KR_MEM_VRAM].replace(0x179fc, block.size(), QByteArray::fromHex("00008200")); // Neutral chroma, zero luma.
    window.setSnapshot(data);
    check(pixel(*view, {319, 239}) == qRgb(0, 0, 0), "changed Firecore bitmap updates the preview");
    data.blocks[KR_MEM_VRAM].resize(0x17a00 - 1);
    window.setSnapshot(data);
    check(image->size().isEmpty() && detail->text().isEmpty(), "truncated Firecore snapshot clears stale data");

    data.blocks[KR_MEM_VRAM].resize(0x20000);
    window.setSnapshot(data);
    check(!image->size().isEmpty(), "complete Firecore snapshot restores the preview");
    data.info.video_format = KR_VIDEO_MD;
    data.info.vdp_regs[0x18] = 0x80;
    window.setSnapshot(data);
    check(image->size().isEmpty() && detail->text().isEmpty(), "tile mode clears bitmap image and link");
    window.setControls(15, 0, false, true, QColor(), false);
    check(!tabs->isTabVisible(tab) && tabs->currentIndex() != tab, "switching hardware hides Firecore bitmap");
    window.setControls(47, 0, false, true, QColor(), false);
    check(tabs->isTabVisible(tab) && tabs->tabText(tab).contains(QStringLiteral("32X")),
          "32X framebuffer tab remains available");
    window.hide();
    check(window.request().parts == 0, "hidden framebuffer unsubscribes");
}

void verifyAudio() {
    DebugAudioDialog dialog;
    verifyWindow(dialog);
    dialog.show();
    check(dialog.request().parts == 0, "audio controls do not subscribe");
    named<QTabWidget>(dialog, "debugAudioTabs")->setCurrentIndex(1);
    check(dialog.request().parts == KR_INSPECT_AUDIO, "audio state subscribes");
    auto data = snapshot(KR_INSPECT_AUDIO, KR_HW_MD, KR_VIDEO_MD);
    value(data, KR_INSPECT_AUDIO, "PSG", "Period", 312);
    value(data, KR_INSPECT_AUDIO, "YM2612", "Status", 42, 2);
    dialog.setSnapshot(data);
    auto* table = named<QTableWidget>(dialog, "debugAudioState");
    check(table->rowCount() == 2 && table->item(0, 2)->text() == QStringLiteral("312") &&
          table->item(1, 2)->text() == QStringLiteral("0x2A"), "audio decimal and hexadecimal values");
    dialog.hide();
    check(dialog.request().parts == 0 && table->rowCount() == 0, "closing audio clears data");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString previewDirectory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!previewDirectory.isEmpty()) check(QDir().mkpath(previewDirectory), "preview directory");
    verifyMemory(previewDirectory);
    verifyMemoryRefresh();
    verifyMemoryEditing();
    verifyGraphics(previewDirectory);
    verifyOtherFormats();
    verifyFramebuffer();
    verifyFirecoreBitmap();
    verifyAudio();
    std::puts("PASS: inspection subscriptions, bounded memory editing, watch, links, video and sound");
}
