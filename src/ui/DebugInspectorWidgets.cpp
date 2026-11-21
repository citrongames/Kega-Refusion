#include "DebugInspectorWidgets.h"
#include "i18n/Localization.h"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QComboBox>
#include <QEvent>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QTableView>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <bitset>
#include <functional>

namespace {
constexpr quint32 pageBytes = 256;
constexpr int rowBytes = 16;
constexpr int pageRows = pageBytes / rowBytes;

QString trText(const char* text) {
    return Localization::text(QString::fromUtf8(text));
}

QString hexNumber(quint64 value, int digits = 0) {
    return QString::number(value, 16).toUpper().rightJustified(digits, QLatin1Char('0'));
}

QString stateCaption(const DebugInspectionSnapshot& snapshot) {
    QString name = QString::fromUtf8(snapshot.info.system);
    if (snapshot.instanceCount > 1)
        name += QStringLiteral(" · ") + (snapshot.instance ? trText("Консоль B") : trText("Консоль A"));
    return QStringLiteral("%1 · %2").arg(name, trText("Снимок кадра %1").arg(snapshot.frame));
}

QString emptyCaption(const DebugInspectionSnapshot& snapshot) {
    return snapshot.status.isEmpty() ? trText("Нет данных снимка") : snapshot.status;
}

void setupTable(QTableWidget* table, const QStringList& columns) {
    table->setColumnCount(columns.size());
    table->setHorizontalHeaderLabels(columns);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 8);
}

QTableWidgetItem* cell(QTableWidget* table, int row, int col, const QString& value) {
    auto* item = table->item(row, col);
    if (!item) {
        item = new QTableWidgetItem;
        table->setItem(row, col, item);
    }
    if (item->text() != value) item->setText(value);
    return item;
}
} // namespace

class DebugMemoryPageModel final : public QAbstractTableModel {
public:
    explicit DebugMemoryPageModel(QObject* parent) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : pageRows; }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : rowBytes + 1; }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        if (!index.isValid()) return Qt::NoItemFlags;
        if (index.column() == rowBytes) return Qt::ItemIsEnabled;
        return index.row() * rowBytes + index.column() < bytes_.size()
            ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) return {};
        const int rowOffset = index.row() * rowBytes;
        if (index.column() == rowBytes) {
            if (role != Qt::DisplayRole) return {};
            QString ascii;
            const auto end = std::min(qsizetype(rowOffset + rowBytes), bytes_.size());
            ascii.reserve(rowBytes);
            for (qsizetype i = rowOffset; i < end; ++i) {
                const auto byte = quint8(bytes_.at(i));
                ascii += byte >= 32 && byte <= 126 ? QLatin1Char(byte) : QLatin1Char('.');
            }
            return ascii;
        }
        const int offset = rowOffset + index.column();
        if (role == Qt::TextAlignmentRole) return int(Qt::AlignCenter);
        if (offset >= bytes_.size()) return {};
        if (role == Qt::DisplayRole) {
            static const auto hexBytes = [] {
                std::array<QString, 256> labels;
                for (size_t i = 0; i < labels.size(); ++i) labels[i] = hexNumber(i, 2);
                return labels;
            }();
            return hexBytes[quint8(bytes_.at(offset))];
        }
        if (!changed_[offset]) return {};
        if (role == Qt::BackgroundRole) return QBrush(QColor(233, 190, 80));
        if (role == Qt::ForegroundRole) return QBrush(Qt::black);
        if (role == Qt::FontRole) {
            QFont underline;
            underline.setUnderline(true);
            return underline;
        }
        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (role != Qt::DisplayRole) return {};
        if (orientation == Qt::Horizontal)
            return section == rowBytes ? QStringLiteral("ASCII") : hexNumber(section, 2);
        const quint32 row = section * rowBytes;
        return row < bytes_.size() ? hexNumber(offset_ + row, 6) : QString();
    }

    const QByteArray& bytes() const { return bytes_; }

    void setPage(quint32 offset, const QByteArray& bytes, bool comparePrevious) {
        const bool moved = offset_ != offset;
        if (!moved && bytes_ == bytes && changed_.none()) return;

        const int previousSize = int(std::min(bytes_.size(), qsizetype(pageBytes)));
        const int nextSize = int(std::min(bytes.size(), qsizetype(pageBytes)));
        int firstRow = pageRows;
        int lastRow = -1;
        std::bitset<pageBytes> changed;
        for (int i = 0; i < std::max(previousSize, nextSize); ++i) {
            const bool hadByte = i < previousSize;
            const bool hasByte = i < nextSize;
            const bool differing = hadByte && hasByte && bytes_.at(i) != bytes.at(i);
            changed[i] = comparePrevious && !moved && differing;
            if (hadByte != hasByte || differing || changed[i] != changed_[i]) {
                firstRow = std::min(firstRow, i / rowBytes);
                lastRow = i / rowBytes;
            }
        }
        const auto visibleRows = [](int size) { return (size + rowBytes - 1) / rowBytes; };
        offset_ = offset;
        bytes_ = bytes;
        changed_ = changed;

        if (firstRow <= lastRow) emit dataChanged(index(firstRow, 0), index(lastRow, rowBytes));
        if ((moved && (previousSize || nextSize)) || visibleRows(previousSize) != visibleRows(nextSize))
            emit headerDataChanged(Qt::Vertical, 0, pageRows - 1);
    }

private:
    QByteArray bytes_;
    std::bitset<pageBytes> changed_;
    quint32 offset_ = 0;
};

DebugMemoryDialog::DebugMemoryDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                      Qt::WindowCloseButtonHint) {
    setObjectName(QStringLiteral("debugMemoryDialog"));
    setWindowTitle(trText("Память"));
    setWindowModality(Qt::NonModal);
    resize(720, 530);

    auto* outer = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("debugMemoryTabs"));
    instances_ = new QComboBox(tabs_);
    instances_->setObjectName(QStringLiteral("debugMemoryInstance"));
    instances_->setAccessibleName(trText("Консоль Link Cable"));
    instances_->addItem(trText("Консоль A"), 0u);
    instances_->addItem(trText("Консоль B"), 1u);
    tabs_->setCornerWidget(instances_);
    instances_->hide();
    connect(instances_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0) changeInstance(instances_->itemData(index).toUInt());
    });
    outer->addWidget(tabs_);
    auto* memory = new QWidget(tabs_);
    tabs_->addTab(memory, trText("Память"));
    hardware_ = new DebugStateInspector(KR_INSPECT_HARDWARE, tabs_);
    tabs_->addTab(hardware_, trText("Оборудование"));
    auto* layout = new QVBoxLayout(memory);
    status_ = new QLabel(trText("Нет данных снимка"), this);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);

    auto* navigation = new QHBoxLayout;
    domains_ = new QComboBox(this);
    domains_->setObjectName(QStringLiteral("debugMemoryDomain"));
    domains_->setAccessibleName(trText("Область памяти"));
    domains_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    domains_->setMinimumContentsLength(14);
    navigation->addWidget(domains_, 1);
    auto* offsetLabel = new QLabel(trText("Смещение (hex)"), this);
    navigation->addWidget(offsetLabel);
    offset_ = new QLineEdit(this);
    offset_->setObjectName(QStringLiteral("debugMemoryOffset"));
    offset_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("(?:0[xX])?[0-9a-fA-F]{1,8}")), offset_));
    offset_->setMaximumWidth(offset_->fontMetrics().horizontalAdvance(QLatin1Char('0')) * 12 + 12);
    offsetLabel->setBuddy(offset_);
    navigation->addWidget(offset_);
    previous_ = new QPushButton(QStringLiteral("‹"), this);
    next_ = new QPushButton(QStringLiteral("›"), this);
    previous_->setAutoDefault(false);
    next_->setAutoDefault(false);
    previous_->setMaximumWidth(40);
    next_->setMaximumWidth(40);
    previous_->setToolTip(trText("Предыдущая страница"));
    next_->setToolTip(trText("Следующая страница"));
    previous_->setAccessibleName(previous_->toolTip());
    next_->setAccessibleName(next_->toolTip());
    navigation->addWidget(previous_);
    navigation->addWidget(next_);
    layout->addLayout(navigation);

    hex_ = new QTableView(this);
    hex_->setObjectName(QStringLiteral("debugMemoryHex"));
    hex_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    pageModel_ = new DebugMemoryPageModel(hex_);
    hex_->setModel(pageModel_);
    hex_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    hex_->setSelectionMode(QAbstractItemView::SingleSelection);
    hex_->setAlternatingRowColors(true);
    hex_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    hex_->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    hex_->horizontalHeader()->setStretchLastSection(true);
    hex_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    resizePageColumns();
    hex_->installEventFilter(this);
    hex_->horizontalHeader()->installEventFilter(this);
    layout->addWidget(hex_, 1);

    auto* findRow = new QHBoxLayout;
    auto* find = new QLineEdit(this);
    find->setAccessibleName(trText("Байты в hex"));
    find->setMaxLength(64);
    find->setPlaceholderText(QStringLiteral("00 FF"));
    find->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9a-fA-F ]*")), find));
    auto* findButton = new QPushButton(trText("Найти на странице"), this);
    findButton->setAutoDefault(false);
    auto* findStatus = new QLabel(this);
    findRow->addWidget(find);
    findRow->addWidget(findButton);
    findRow->addWidget(findStatus, 1);
    layout->addLayout(findRow);
    const auto findNext = [this, find, findStatus] {
        QString text = find->text();
        text.remove(QLatin1Char(' '));
        if (text.isEmpty() || text.size() % 2) return;
        const auto pattern = QByteArray::fromHex(text.toLatin1());
        const QByteArray page = pageModel_->bytes().left(pageBytes);
        auto at = page.indexOf(pattern, selection_ - page_ + 1);
        if (at < 0) at = page.indexOf(pattern);
        if (at >= 0) changeOffset(page_ + quint32(at));
        findStatus->setText(at >= 0 ? QString() : trText("Не найдено"));
    };
    connect(findButton, &QPushButton::clicked, this, findNext);
    connect(find, &QLineEdit::returnPressed, this, findNext);
    connect(find, &QLineEdit::textChanged, findStatus, &QLabel::clear);
    connect(this, &DebugMemoryDialog::inspectionChanged, findStatus, &QLabel::clear);

    auto* watch = new QHBoxLayout;
    auto* valueLabel = new QLabel(trText("Значение"), this);
    watch->addWidget(valueLabel);
    width_ = new QComboBox(this);
    width_->setObjectName(QStringLiteral("debugMemoryWidth"));
    width_->setAccessibleName(trText("Тип числа"));
    for (const int width : {1, 2, 4}) {
        width_->addItem(QStringLiteral("U%1").arg(width * 8), width);
        width_->addItem(QStringLiteral("S%1").arg(width * 8), -width);
    }
    watch->addWidget(width_);
    order_ = new QComboBox(this);
    order_->setObjectName(QStringLiteral("debugMemoryByteOrder"));
    order_->setAccessibleName(trText("Порядок байтов"));
    order_->addItem(trText("Порядок области"), 0);
    order_->addItem(QStringLiteral("Little-endian"), 1);
    order_->addItem(QStringLiteral("Big-endian"), 2);
    watch->addWidget(order_);
    value_ = new QLabel(this);
    value_->setObjectName(QStringLiteral("debugMemoryValue"));
    value_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    watch->addWidget(value_, 1);
    edit_ = new QPushButton(trText("Изменить…"), this);
    edit_->setObjectName(QStringLiteral("debugMemoryEdit"));
    edit_->setAutoDefault(false);
    watch->addWidget(edit_);
    layout->addLayout(watch);
    editStatus_ = new QLabel(this);
    editStatus_->setObjectName(QStringLiteral("debugMemoryWriteStatus"));
    editStatus_->setTextFormat(Qt::PlainText);
    editStatus_->setWordWrap(true);
    editStatus_->hide();
    layout->addWidget(editStatus_);
    connect(edit_, &QPushButton::clicked, this, [this] { editMemory(qAbs(width_->currentData().toInt())); });
    connect(hex_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const int relative = index.row() * rowBytes + index.column();
        if (index.isValid() && index.column() < rowBytes && relative < pageModel_->bytes().size()) {
            selection_ = page_ + relative;
            editMemory(1);
        }
    });

    connect(domains_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0) return;
        domain_ = domains_->itemData(index).toUInt();
        writable_ = false;
        editStatus_->hide();
        size_ = domains_->itemData(index, Qt::UserRole + 1).toUInt();
        bigEndian_ = domains_->itemData(index, Qt::UserRole + 2).toBool();
        selection_ = 0;
        page_ = 0;
        updatePage({});
        updateNavigation();
        emit inspectionChanged();
    });
    connect(offset_, &QLineEdit::returnPressed, this, [this] {
        bool ok = false;
        const quint32 address = offset_->text().toUInt(&ok, 16);
        if (ok) {
            offset_->setModified(false);
            changeOffset(address);
        }
    });
    connect(previous_, &QPushButton::clicked, this, [this] { changeOffset(page_ - pageBytes); });
    connect(next_, &QPushButton::clicked, this, [this] { changeOffset(page_ + pageBytes); });
    connect(hex_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& index) {
        if (!index.isValid() || index.column() >= rowBytes) return;
        const int relative = index.row() * rowBytes + index.column();
        if (relative < pageModel_->bytes().size()) {
            selection_ = page_ + relative;
            updateNavigation();
            updateWatch();
        }
    });
    connect(width_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateWatch(); });
    connect(order_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateWatch(); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        if (tabs_->currentIndex() == 1) {
            updatePage({});
            updateNavigation();
        } else {
            hardware_->setSnapshot({});
        }
        emit inspectionChanged();
    });
    updateNavigation();
    updateWatch();
}

DebugInspectionRequest DebugMemoryDialog::request() const {
    DebugInspectionRequest request;
    if (isVisible())
        request.parts = tabs_->currentIndex() == 0 ? KR_INSPECT_MEMORY : KR_INSPECT_HARDWARE;
    request.domain = domain_;
    request.offset = page_;
    request.length = pageBytes + 3;
    request.instance = instance_;
    return request;
}

void DebugMemoryDialog::navigateTo(quint32 instance, quint32 domain, quint32 offset) {
    const int index = instances_->findData(instance);
    if (index < 0) return;
    {
        const QSignalBlocker block(instances_);
        instances_->setCurrentIndex(index);
    }
    changeInstance(instance);
    navigateTo(domain, offset);
}

void DebugMemoryDialog::navigateTo(quint32 domain, quint32 offset) {
    tabs_->setCurrentIndex(0);
    if (available_) {
        const int index = domains_->findData(domain);
        if (index < 0) return;
        const QSignalBlocker block(domains_);
        domains_->setCurrentIndex(index);
        size_ = domains_->itemData(index, Qt::UserRole + 1).toUInt();
        bigEndian_ = domains_->itemData(index, Qt::UserRole + 2).toBool();
    } else {
        size_ = 0;
    }
    const bool changed = domain_ != domain;
    domain_ = domain;
    if (changed) {
        writable_ = false;
        updatePage({});
    }
    const quint32 oldPage = page_;
    changeOffset(offset);
    if (changed && oldPage == page_) emit inspectionChanged();
}

void DebugMemoryDialog::changeInstance(quint32 instance) {
    if (instance == instance_) return;
    instance_ = instance;
    available_ = false;
    canEdit_ = writable_ = false;
    status_->setText(trText("Нет данных снимка"));
    editStatus_->hide();
    updatePage({});
    updateNavigation();
    hardware_->setSnapshot({});
    emit inspectionInstanceChanged();
    emit inspectionChanged();
}

void DebugMemoryDialog::setSnapshot(const DebugInspectionSnapshot& snapshot) {
    if (snapshot.available() || snapshot.serial) {
        instances_->setVisible(snapshot.instanceCount > 1);
        if (instance_ >= snapshot.instanceCount && instance_ != 0) {
            const QSignalBlocker block(instances_);
            instances_->setCurrentIndex(0);
            changeInstance(0);
        }
        if (snapshot.instance != instance_) return;
    }
    if (!snapshot.available()) {
        hardware_->setSnapshot(snapshot);
        status_->setText(emptyCaption(snapshot));
        if (!available_) return;
        available_ = false;
        canEdit_ = writable_ = false;
        size_ = 0;
        {
            const QSignalBlocker block(domains_);
            domains_->clear();
        }
        updatePage({});
        updateNavigation();
        return;
    }
    if (!isVisible()) return;
    if (tabs_->currentWidget() == hardware_) {
        hardware_->setSnapshot(snapshot);
        return;
    }
    if (!(snapshot.parts & KR_INSPECT_MEMORY)) return;
    status_->setText(stateCaption(snapshot));
    const auto count = std::min(snapshot.info.domain_count, KR_INSPECT_MAX_DOMAINS);
    const bool contextChanged = !available_ || generation_ != snapshot.generation;
    const QSignalBlocker block(domains_);
    bool catalogChanged = quint32(domains_->count()) != count;
    for (quint32 i = 0; !catalogChanged && i < count; ++i) {
        const auto& domain = snapshot.info.domains[i];
        catalogChanged = domains_->itemData(i).toUInt() != domain.id ||
            domains_->itemData(i, Qt::UserRole + 1).toUInt() != domain.size ||
            domains_->itemData(i, Qt::UserRole + 2).toBool() != bool(domain.big_endian) ||
            domains_->itemText(i) != Localization::text(QString::fromUtf8(domain.name));
    }
    if (contextChanged || catalogChanged) {
        domains_->clear();
        for (quint32 i = 0; i < count; ++i) {
            const auto& domain = snapshot.info.domains[i];
            domains_->addItem(Localization::text(QString::fromUtf8(domain.name)), domain.id);
            const int index = domains_->count() - 1;
            domains_->setItemData(index, domain.size, Qt::UserRole + 1);
            domains_->setItemData(index, bool(domain.big_endian), Qt::UserRole + 2);
        }
    }
    available_ = true;
    generation_ = snapshot.generation;
    int index = domains_->findData(domain_);
    if (index < 0) index = domains_->findData(quint32(KR_MEM_RAM));
    if (index < 0) index = domains_->findData(quint32(KR_MEM_ZRAM));
    if (index < 0 && count) index = 0;
    domains_->setCurrentIndex(index);
    if (index < 0) {
        size_ = 0;
        updateNavigation();
        return;
    }
    const quint32 requestedDomain = domain_;
    const quint32 requestedPage = page_;
    const quint32 resolved = domains_->itemData(index).toUInt();
    if (domain_ != resolved) selection_ = 0;
    domain_ = resolved;
    canEdit_ = snapshot.canEdit;
    writable_ = snapshot.writableDomains.contains(domain_);
    size_ = domains_->itemData(index, Qt::UserRole + 1).toUInt();
    bigEndian_ = domains_->itemData(index, Qt::UserRole + 2).toBool();
    if (size_) selection_ = std::min(selection_, size_ - 1);
    page_ = selection_ - selection_ % pageBytes;
    if (snapshot.memoryDomain == domain_ && snapshot.memoryOffset == page_) {
        updatePage(snapshot.memory, !contextChanged && !catalogChanged && requestedDomain == resolved);
    } else {
        updatePage({});
    }
    updateNavigation();
    // A newly discovered domain or a clamped page needs its own bytes now
    // rather than after the next periodic refresh.
    if (requestedDomain != domain_ || requestedPage != page_) emit inspectionChanged();
}

void DebugMemoryDialog::changeOffset(quint32 offset) {
    selection_ = size_ ? std::min(offset, size_ - 1) : offset;
    const quint32 page = selection_ - selection_ % pageBytes;
    if (page_ != page) {
        page_ = page;
        updatePage({});
        updateNavigation();
        emit inspectionChanged();
        return;
    }
    updateNavigation();
    updateWatch();
}

void DebugMemoryDialog::updatePage(const QByteArray& bytes, bool comparePrevious) {
    const QSignalBlocker block(hex_->selectionModel());
    pageModel_->setPage(page_, bytes, comparePrevious);
    updateWatch();
}

void DebugMemoryDialog::resizePageColumns() {
    auto* header = hex_->horizontalHeader();
    const int margin = std::max(12, 2 * header->style()->pixelMetric(QStyle::PM_HeaderMargin, nullptr, header));
    const QString byteLabel = QStringLiteral("FF");
    const int width = std::max(header->fontMetrics().horizontalAdvance(byteLabel),
                               hex_->fontMetrics().horizontalAdvance(byteLabel));
    header->setDefaultSectionSize(width + margin);
    header->resizeSection(rowBytes, hex_->fontMetrics().horizontalAdvance(QString(rowBytes, QLatin1Char('M'))) + margin);
    hex_->verticalHeader()->setDefaultSectionSize(hex_->fontMetrics().height() + 8);
}

bool DebugMemoryDialog::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::StyleChange:
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    case QEvent::DevicePixelRatioChange:
#endif
        if (hex_) resizePageColumns();
        break;
    default:
        break;
    }
    return QDialog::eventFilter(watched, event);
}

void DebugMemoryDialog::updateNavigation() {
    const bool ready = available_ && size_ != 0;
    domains_->setEnabled(available_ && domains_->count());
    offset_->setEnabled(ready);
    width_->setEnabled(ready);
    order_->setEnabled(ready);
    previous_->setEnabled(ready && page_ > 0);
    next_->setEnabled(ready && quint64(page_) + pageBytes < size_);
    if (!offset_->hasFocus() || !offset_->isModified()) {
        const QString value = ready ? hexNumber(selection_) : QString();
        if (offset_->text() != value) offset_->setText(value);
    }
    const quint32 relative = selection_ - page_;
    const QModelIndex index = relative < pageBytes && relative < pageModel_->bytes().size()
        ? pageModel_->index(relative / rowBytes, relative % rowBytes) : QModelIndex{};
    if (index != hex_->currentIndex()) {
        const QSignalBlocker block(hex_->selectionModel());
        if (!index.isValid()) hex_->clearSelection();
        hex_->setCurrentIndex(index);
        if (index.isValid()) {
            hex_->scrollTo(index, QAbstractItemView::EnsureVisible);
            // Qt can lay out a newly populated header after setting the current
            // cell. Bring any clipped row into view once that layout has run.
            QTimer::singleShot(0, this, [this, index] {
                if (!hex_->isVisible() || hex_->currentIndex() != index) return;
                hex_->doItemsLayout();
                const QRect cell = hex_->visualRect(index);
                auto* scroll = hex_->verticalScrollBar();
                if (cell.top() < 0)
                    scroll->setValue(scroll->value() + cell.top());
                else if (cell.bottom() >= hex_->viewport()->height())
                    scroll->setValue(scroll->value() + cell.bottom() - hex_->viewport()->height() + 1);
            });
        }
    }
}

void DebugMemoryDialog::updateWatch() {
    const int type = width_->currentData().toInt();
    const int width = qAbs(type);
    edit_->setEnabled(canWrite(width));
    edit_->setToolTip(writable_
        ? trText("Изменение доступно на паузе, вне записи и воспроизведения ввода.")
        : trText("Эта область памяти доступна только для чтения."));
    const quint32 relative = selection_ - page_;
    const auto& bytes = pageModel_->bytes();
    if (!width || relative + width > quint64(bytes.size())) {
        value_->setText(QStringLiteral("—"));
        return;
    }
    const bool bigEndian = watchBigEndian();
    const auto* data = bytes.constData() + relative;
    quint32 number = quint8(*data);
    if (width == 2) number = bigEndian ? qFromBigEndian<quint16>(data) : qFromLittleEndian<quint16>(data);
    if (width == 4) number = bigEndian ? qFromBigEndian<quint32>(data) : qFromLittleEndian<quint32>(data);
    const quint64 limit = quint64(1) << (width * 8);
    const qint64 decimal = type < 0 && number >= limit / 2 ? qint64(number) - qint64(limit) : number;
    value_->setText(QStringLiteral("0x%1  (%2)").arg(hexNumber(number, width * 2)).arg(decimal));
}

bool DebugMemoryDialog::watchBigEndian() const {
    const int order = order_->currentData().toInt();
    return order == 0 ? bigEndian_ : order == 2;
}

bool DebugMemoryDialog::canWrite(int width) const {
    return isVisible() && tabs_->currentIndex() == 0 && available_ && canEdit_ && writable_ &&
           !writePending_ && (width == 1 || width == 2 || width == 4) &&
           quint64(selection_ - page_) + width <= quint64(pageModel_->bytes().size());
}

void DebugMemoryDialog::editMemory(int width) {
    if (!canWrite(width)) return;
    DebugMemoryWrite request;
    request.generation = generation_;
    request.instance = instance_;
    request.domain = domain_;
    request.offset = selection_;
    request.before = pageModel_->bytes().mid(selection_ - page_, width);
    const bool bigEndian = watchBigEndian();
    const auto* data = request.before.constData();
    quint32 previous = quint8(*data);
    if (width == 2) previous = bigEndian ? qFromBigEndian<quint16>(data) : qFromLittleEndian<quint16>(data);
    if (width == 4) previous = bigEndian ? qFromBigEndian<quint32>(data) : qFromLittleEndian<quint32>(data);
    bool accepted = false;
    const QString input = QInputDialog::getText(this, trText("Изменить память"),
        trText("Новое значение по 0x%1 (hex)").arg(hexNumber(request.offset)),
        QLineEdit::Normal, hexNumber(previous, width * 2), &accepted);
    if (!accepted || generation_ != request.generation || instance_ != request.instance ||
        domain_ != request.domain || selection_ != request.offset || !canWrite(width)) return;
    QString digits = input.trimmed();
    if (digits.startsWith(QLatin1Char('$'))) digits.remove(0, 1);
    else if (digits.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) digits.remove(0, 2);
    static const QRegularExpression hexDigits(QStringLiteral("^[0-9a-fA-F]{1,8}$"));
    bool ok = false;
    const auto value = digits.toUInt(&ok, 16);
    if (!ok || !hexDigits.match(digits).hasMatch() || quint64(value) >= (quint64(1) << (width * 8))) {
        finishMemoryWrite(trText("Введите шестнадцатеричное значение в пределах выбранного размера."));
        return;
    }
    request.after = QByteArray(width, '\0');
    if (width == 1) request.after[0] = char(value);
    else if (width == 2) {
        if (bigEndian) qToBigEndian(quint16(value), request.after.data());
        else qToLittleEndian(quint16(value), request.after.data());
    } else {
        if (bigEndian) qToBigEndian(value, request.after.data());
        else qToLittleEndian(value, request.after.data());
    }
    if (request.after == request.before) return;
    writePending_ = true;
    editStatus_->hide();
    updateWatch();
    emit writeRequested(request);
}

void DebugMemoryDialog::finishMemoryWrite(const QString& error) {
    writePending_ = false;
    if (error.isEmpty()) canEdit_ = false;
    if (!isVisible()) return;
    editStatus_->setText(error.isEmpty() ? trText("Байты записаны") : error);
    editStatus_->show();
    updateWatch();
}

void DebugMemoryDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    emit inspectionChanged();
}

void DebugMemoryDialog::hideEvent(QHideEvent* event) {
    QDialog::hideEvent(event);
    editStatus_->hide();
    setSnapshot({});
    emit inspectionChanged();
}

DebugStateInspector::DebugStateInspector(quint32 part, QWidget* parent)
    : QWidget(parent), part_(part) {
    auto* layout = new QVBoxLayout(this);
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    table_ = new QTableWidget(this);
    table_->setObjectName(part == KR_INSPECT_AUDIO ? QStringLiteral("debugAudioState") :
                          part == KR_INSPECT_VIDEO ? QStringLiteral("debugVideoRegisters") :
                                                    QStringLiteral("debugHardwareState"));
    setupTable(table_, {trText("Устройство"), trText("Параметр"), trText("Значение")});
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table_, 1);
    setSnapshot({});
}

void DebugStateInspector::setSnapshot(const DebugInspectionSnapshot& snapshot) {
    if (!snapshot.available()) {
        status_->setText(emptyCaption(snapshot));
        table_->setRowCount(0);
        return;
    }
    if (!(snapshot.parts & part_)) return;
    status_->setText(stateCaption(snapshot));
    const auto count = std::min(snapshot.info.value_count, KR_INSPECT_MAX_VALUES);
    int registers = 0;
    if (part_ == KR_INSPECT_VIDEO) {
        const auto domains = std::min(snapshot.info.domain_count, KR_INSPECT_MAX_DOMAINS);
        for (quint32 i = 0; i < domains; ++i) {
            const auto& domain = snapshot.info.domains[i];
            if (domain.id == KR_MEM_VDP_REGS) {
                registers = int(std::min(domain.size, quint32(sizeof(snapshot.info.vdp_regs))));
                break;
            }
        }
    }
    int rows = registers;
    for (quint32 i = 0; i < count; ++i)
        if (snapshot.info.values[i].part & part_) ++rows;
    table_->setRowCount(rows);
    for (int reg = 0; reg < registers; ++reg) {
        cell(table_, reg, 0, QStringLiteral("VDP"));
        cell(table_, reg, 1, QStringLiteral("R%1").arg(hexNumber(reg, 2)));
        cell(table_, reg, 2, QStringLiteral("0x%1").arg(hexNumber(snapshot.info.vdp_regs[reg], 2)));
    }
    int row = registers;
    for (quint32 i = 0; i < count; ++i) {
        const auto& value = snapshot.info.values[i];
        if (!(value.part & part_)) continue;
        cell(table_, row, 0, Localization::text(QString::fromUtf8(value.device)));
        cell(table_, row, 1, Localization::text(QString::fromUtf8(value.name)));
        cell(table_, row++, 2, value.digits ? QStringLiteral("0x%1").arg(hexNumber(value.value, std::min(value.digits, 16u)))
                                          : QString::number(value.value));
    }
    if (!rows) status_->setText(trText("Для этого устройства нет данных"));
}

// Decode address-ordered physical table bytes without raster history or the
// VDP sprite cache. All coordinates here are relative to the captured tables.
namespace {
quint8 byteAt(const QByteArray& bytes, quint32 address) {
    return address < bytes.size() ? quint8(bytes.at(address)) : 0;
}

quint16 wordAt(const QByteArray& bytes, quint32 address, bool bigEndian = true) {
    if (quint64(address) + 2 > quint64(bytes.size())) return 0;
    const char* p = bytes.constData() + address;
    return bigEndian ? qFromBigEndian<quint16>(p) : qFromLittleEndian<quint16>(p);
}

quint32 field(const DebugInspectionSnapshot& snapshot, const char* device, const char* name,
              quint32 fallback) {
    const auto count = std::min(snapshot.info.value_count, KR_INSPECT_MAX_VALUES);
    for (quint32 i = 0; i < count; ++i) {
        const auto& value = snapshot.info.values[i];
        if (QByteArrayView(value.device) == device && QByteArrayView(value.name) == name)
            return quint32(value.value);
    }
    return fallback;
}

QRgb packedColor(int color, int bits) {
    const int mask = (1 << bits) - 1;
    return qRgb((color & mask) * 255 / mask, ((color >> bits) & mask) * 255 / mask,
                ((color >> (2 * bits)) & mask) * 255 / mask);
}

QString memoryLink(quint32 domain, quint32 offset, const QString& label) {
    return QStringLiteral("<a href=\"%1/%2\">%3 0x%4</a>")
        .arg(domain).arg(offset).arg(label.toHtmlEscaped(), hexNumber(offset, 4));
}

struct Video {
    explicit Video(const DebugInspectionSnapshot& s)
        : snapshot(s), vram(s.blocks.value(KR_MEM_VRAM)), cram(s.blocks.value(KR_MEM_CRAM)),
          reg(s.info.vdp_regs), format(s.info.video_format), hardware(s.info.hardware) {}

    bool mode5() const { return format == KR_VIDEO_MD; }
    bool tms() const { return format == KR_VIDEO_SG; }
    bool h40() const { return reg[12] & 1; }
    bool firecore() const { return (hardware & KR_HW_FIRECORE) && (reg[0x18] & 0x80); }
    bool interlace() const { return mode5() && (reg[12] & 6) == 6; }
    int tmsMode() const { return ((reg[0] & 2) | (reg[1] & 0x18)) >> 1; }
    int tileHeight() const { return interlace() ? 16 : 8; }
    int tileWidth() const { return tms() && (tmsMode() & 8) ? 6 : 8; }
    int tileBytes() const { return tms() ? 8 : (firecore() ? 48 : 4 * tileHeight()); }
    int bankColors() const { return firecore() ? 64 : 16; }
    int banks() const { return firecore() ? 8 : (mode5() ? 4 : 2); }
    int activeLines() const { return int(field(snapshot, "VDP", "Active lines", snapshot.info.height)); }
    quint32 mask() const { return firecore() ? 0x1ffff : mode5() ? 0xffff : 0x3fff; }

    int colorBytes() const {
        if (tms()) return 0;
        if (firecore()) return 4;
        return mode5() || cram.size() == 64 ? 2 : 1;
    }

    bool decodable() const {
        if (!mode5() && !tms() && format != KR_VIDEO_SMS) return false;
        return !vram.isEmpty() && (tms() || !cram.isEmpty())
            && !(firecore() && interlace()) && !(tms() && tmsMode() >= 12);
    }

    int tileCount() const {
        if (tms()) return 256;
        return std::min(int(vram.size() / tileBytes()), mode5() ? 2048 : 512);
    }

    quint8 byte(quint32 address) const { return byteAt(vram, address & mask()); }
    quint16 word(quint32 address) const { return wordAt(vram, address & mask(), mode5()); }

    QRgb color(int index) const {
        if (tms()) {
            // Nominal TMS9918 colors before output filtering.
            static constexpr std::array<QRgb, 16> colors{
                0xff000000, 0xff000000, 0xff21c842, 0xff5edc78, 0xff5455ed, 0xff7d76fc,
                0xffd4524d, 0xff42ebf5, 0xfffc5554, 0xffff7978, 0xffd4c154, 0xffe6ce80,
                0xff21b03b, 0xffc95bba, 0xffcccccc, 0xffffffff
            };
            return colors[index & 15];
        }
        const quint32 addr = index * colorBytes();
        if (firecore()) return qRgb(byteAt(cram, addr + 1), byteAt(cram, addr), byteAt(cram, addr + 3));
        if (mode5()) return packedColor(wordAt(cram, addr), 3);
        if (colorBytes() == 2) return packedColor(wordAt(cram, addr, false), 4);
        return packedColor(byteAt(cram, addr), 2);
    }

    quint32 patternAddress(int name, int mapRow) const {
        if (!tms()) return quint32(name * tileBytes()) & mask();
        if (tmsMode() & 1) {
            const quint32 tableMask = ((reg[4] & 7) << 11) | 0x7ff |
                                      ((hardware & KR_HW_SG) ? 0 : 0x1800);
            return (0x2000 + ((mapRow & 0x18) << 8) + (name << 3)) & tableMask;
        }
        return ((reg[4] & 7) << 11) + (name << 3);
    }

    int patternPixel(quint32 address, int x, int y) const {
        if (firecore()) {
            const int bit = x * 6;
            const quint32 source = address + y * 6 + bit / 8;
            // The packed 6-bit stream uses the low byte of each guest word first.
            const unsigned bytes = byte(source ^ 1) | (unsigned(byte((source + 1) ^ 1)) << 8);
            return (bytes >> (bit & 7)) & 63;
        }
        if (mode5()) {
            const auto value = byte(address + y * 4 + x / 2);
            return (x & 1) ? value & 15 : value >> 4;
        }
        int color = 0;
        for (int plane = 0; plane < 4; ++plane)
            color |= ((byte(address + y * 4 + plane) >> (7 - x)) & 1) << plane;
        return color;
    }

    int tmsPixel(int name, int mapRow, int x, int y) const {
        const auto address = patternAddress(name, mapRow);
        const int mode = tmsMode();
        if (mode & 4) {
            const auto value = byte(address + ((mapRow & 3) << 1) + y / 4);
            return x < 4 ? value >> 4 : value & 15;
        }
        int colors = reg[7];
        if (mode == 0) {
            colors = byte((reg[3] << 6) + name / 8);
        } else if (mode == 1) {
            const int colorMask = (reg[3] << 6) | 0x3f |
                                  ((hardware & KR_HW_SG) ? 0 : 0x1fc0);
            colors = byte((0x2000 + ((mapRow & 0x18) << 8) + (name << 3) + y) & colorMask);
        }
        const bool foreground = byte(address + y) & (0x80 >> x);
        return foreground ? colors >> 4 : colors & 15;
    }

    const DebugInspectionSnapshot& snapshot;
    const QByteArray vram;
    const QByteArray cram;
    const uint8_t* reg;
    const quint32 format;
    const quint32 hardware;
};

struct MapLayout {
    quint32 base = 0;
    quint32 rowMask = 0;
    quint32 addressMask = ~quint32(0);
    int columns = 32;
    int rows = 24;
    int stride = 0;
    int palette = 0;
    bool words = true;

    bool operator==(const MapLayout& other) const {
        return base == other.base && rowMask == other.rowMask && addressMask == other.addressMask &&
            columns == other.columns && rows == other.rows && stride == other.stride &&
            palette == other.palette && words == other.words;
    }

    quint32 address(int x, int y) const {
        const quint32 row = quint32(y * stride);
        return (base + (rowMask ? row & rowMask : row) + quint32(x * (words ? 2 : 1))) & addressMask;
    }
};

MapLayout mapLayout(const Video& video, int plane) {
    const auto* r = video.reg;
    MapLayout map;
    if (video.tms()) {
        map.columns = video.tileWidth() == 6 ? 40 : 32;
        map.stride = map.columns;
        map.words = false;
        map.base = (r[2] << 10) & 0x3c00;
    } else if (!video.mode5()) {
        const bool extended = video.activeLines() > 192;
        const auto mask = field(video.snapshot, "VDP", "Name table mask",
                                0x3fff ^ ((~r[2] & 0xe) << 10));
        map.base = extended ? mask & 0x3700 : 0x3800;
        map.addressMask = extended ? 0x3fff : mask;
        map.rows = extended ? 32 : 28;
        map.stride = 64;
    } else if (plane == 2) {
        const quint32 fallback = (r[3] << 10) & (video.h40() ? 0xf000 : 0xf800);
        map.base = field(video.snapshot, "VDP", "Window table", fallback);
        map.columns = video.h40() ? 64 : 32;
        map.rows = 32;
        map.stride = map.columns * 2;
        if (video.firecore()) map.palette = (r[0x18] & 4) ? 4 : 0;
    } else {
        constexpr std::array<int, 4> widths{32, 64, 32, 128};
        constexpr std::array<int, 4> heights{32, 64, 96, 128};
        map.columns = widths[r[16] & 3];
        map.rows = heights[(r[16] >> 4) & 3];
        map.stride = (r[16] & 3) == 2 ? 1 : map.columns * 2;
        map.rowMask = 0x1fc0;
        if (plane == 1) {
            map.base = field(video.snapshot, "VDP", "Plane B table", (r[4] << 13) & 0xe000);
        } else {
            map.base = field(video.snapshot, "VDP", "Plane A table", (r[2] << 10) & 0xe000);
        }
        if (video.firecore()) map.palette = (r[0x18] & (1 << plane)) ? 4 : 0;
    }
    return map;
}

struct Tile {
    int index = 0;
    int palette = 0;
    bool horizontal = false;
    bool vertical = false;
    bool priority = false;
};

Tile tileFromMap(const Video& video, const MapLayout& map, quint32 address) {
    if (!map.words) return {video.byte(address)};
    const quint16 entry = video.word(address);
    if (video.mode5()) {
        return {entry & (video.interlace() ? 0x3ff : 0x7ff), map.palette + ((entry >> 13) & 3),
                bool(entry & 0x0800), bool(entry & 0x1000), bool(entry & 0x8000)};
    }
    return {entry & 511, (entry >> 11) & 1, bool(entry & 0x200), bool(entry & 0x400),
            bool(entry & 0x1000)};
}

void drawTile(QImage& image, const Video& video, const Tile& tile, int x, int y, int mapRow = 0) {
    const int h = video.tileHeight();
    const int w = video.tileWidth();
    const auto address = video.patternAddress(tile.index, mapRow);
    for (int row = 0; row < h; ++row) {
        const int sy = tile.vertical ? h - 1 - row : row;
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y + row)) + x;
        for (int col = 0; col < w; ++col) {
            const int sx = tile.horizontal ? w - 1 - col : col;
            int index;
            if (video.tms()) {
                index = video.tmsPixel(tile.index, mapRow, sx, sy);
                if (!index) index = video.reg[7] & 15;
            } else {
                index = tile.palette * video.bankColors() + video.patternPixel(address, sx, sy);
            }
            line[col] = video.color(index);
        }
    }
}
} // namespace

namespace {
struct Sprite {
    Tile tile;
    quint32 entry = 0;
    quint32 pattern = 0;
    int index = 0;
    int x = 0;
    int y = 0;
    int columns = 1;
    int rows = 1;
    int zoom = 1;
};

QList<Sprite> spriteList(const Video& video) {
    const auto* r = video.reg;
    QList<Sprite> sprites;
    if (video.mode5()) {
        const int limit = video.h40() ? 80 : 64;
        const quint32 base = field(video.snapshot, "VDP", "Sprite table",
                                   (r[5] << 9) & (video.h40() ? 0xfc00 : 0xfe00));
        const int yOrigin = video.interlace() ? 256 : 128;
        std::array<bool, 128> visited{};
        for (int index = 0; index < limit && !visited[index];) {
            visited[index] = true;
            const quint32 address = (base + index * 8) & video.mask();
            const auto sizeAndLink = video.word(address + 2);
            Sprite sprite;
            sprite.index = index;
            sprite.entry = address;
            sprite.tile = tileFromMap(video, MapLayout{}, address + 4);
            if (video.firecore() && (r[0x18] & 8)) sprite.tile.palette += 4;
            sprite.pattern = video.patternAddress(sprite.tile.index, 0);
            sprite.x = (video.word(address + 6) & 511) - 128;
            sprite.y = (video.word(address) & (video.interlace() ? 1023 : 511)) - yOrigin;
            sprite.columns = ((sizeAndLink >> 10) & 3) + 1;
            sprite.rows = ((sizeAndLink >> 8) & 3) + 1;
            sprites.append(sprite);
            index = sizeAndLink & 127;
            if (index == 0) break;
        }
    } else {
        const bool tms = video.tms();
        if (tms && (video.tmsMode() & 8)) return sprites;
        const quint32 base = (r[5] << 7) & (tms ? 0x3f80 : 0x3f00);
        const quint32 smsMask = field(video.snapshot, "VDP", "Sprite table mask", 0x3fff);
        const bool doubleHeight = r[1] & 2;
        const int active = tms ? 192 : video.activeLines();
        for (int i = 0; i < (tms ? 32 : 64); ++i) {
            Sprite sprite;
            sprite.index = i;
            sprite.entry = base + (tms ? i * 4 : i);
            const int y = video.byte(sprite.entry);
            if (y == 208 && active <= 192) break;
            sprite.y = y + 1 - (y > active + 16 ? 256 : 0);
            sprite.rows = doubleHeight ? 2 : 1;
            sprite.zoom = ((r[1] & 1) && !(video.hardware & KR_HW_MD)) ? 2 : 1;
            if (tms) {
                const int color = video.byte(sprite.entry + 3);
                sprite.x = video.byte(sprite.entry + 1) - ((color & 0x80) ? 32 : 0);
                sprite.tile.index = video.byte(sprite.entry + 2) & (doubleHeight ? 0xfc : 0xff);
                sprite.tile.palette = color & 15;
                sprite.columns = sprite.rows;
                sprite.pattern = ((r[6] << 11) & 0x3800) + sprite.tile.index * 8;
            } else {
                const quint32 xTable = 0x80 + 2 * i;
                sprite.x = video.byte(base + (xTable & smsMask)) - ((r[0] & 8) ? 8 : 0);
                sprite.tile.index = (video.byte(base + ((xTable + 1) & smsMask)) &
                                      (doubleHeight ? 0xfe : 0xff)) | ((r[6] & 4) << 6);
                sprite.tile.palette = 1;
                sprite.pattern = video.patternAddress(sprite.tile.index, 0);
            }
            sprites.append(sprite);
        }
    }
    return sprites;
}

QImage spritePreview(const Video& video, const Sprite& sprite) {
    const int tileHeight = video.tileHeight();
    QImage image(sprite.columns * 8, sprite.rows * tileHeight, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        const int sy = sprite.tile.vertical ? image.height() - 1 - y : y;
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int sx = sprite.tile.horizontal ? image.width() - 1 - x : x;
            const int pattern = (sx / 8) * sprite.rows + sy / tileHeight;
            if (video.tms()) {
                const bool set = video.byte(sprite.pattern + pattern * 8 + sy % 8) & (0x80 >> (sx % 8));
                line[x] = set && sprite.tile.palette ? video.color(sprite.tile.palette) : 0;
            } else {
                const quint32 address = (sprite.pattern + pattern * video.tileBytes()) & video.mask();
                const int index = video.patternPixel(address, sx % 8, sy % tileHeight);
                line[x] = index ? video.color(sprite.tile.palette * video.bankColors() + index) : 0;
            }
        }
    }
    return image;
}

quint32 fbValue(const DebugInspectionSnapshot& snapshot, const char* name, quint32 fallback = 0) {
    return field(snapshot, "32X VDP", name, fallback);
}

using FrameLine = std::array<quint16, 320>;
FrameLine framebufferLine(const QByteArray& page, const QByteArray& colors,
                          int y, int mode, bool pixelShift) {
    FrameLine line{};
    const quint32 start = wordAt(page, y * 2);
    const int shift = pixelShift && (start & 0xff) != 0xff ? 1 : 0;
    if (mode == 3) {
        int filled = 0;
        quint32 offset = start;
        int skip = shift;
        while (filled < int(line.size())) {
            const auto run = wordAt(page, (offset++ & 0xffff) * 2);
            const int length = std::min((run >> 8) + 1 - skip, int(line.size()) - filled);
            std::fill_n(line.begin() + filled, length, wordAt(colors, (run & 255) * 2));
            filled += length;
            skip = 0;
        }
    } else if (mode == 2) {
        for (unsigned x = 0; x < line.size(); ++x)
            line[x] = wordAt(page, ((start + x + shift) & 0xffff) * 2);
    } else if (mode == 1) {
        for (unsigned x = 0; x < line.size(); ++x) {
            const auto index = byteAt(page, (start * 2 + x + shift) & 0x1ffff);
            line[x] = wordAt(colors, index * 2);
        }
    }
    return line;
}

QString tileFlags(const Tile& tile) {
    QStringList flags;
    if (tile.horizontal) flags << QStringLiteral("H");
    if (tile.vertical) flags << QStringLiteral("V");
    if (tile.priority) flags << QStringLiteral("P");
    return flags.isEmpty() ? QStringLiteral("—") : flags.join(QLatin1Char(' '));
}

void setOptions(QComboBox* combo, const QStringList& options) {
    combo->setVisible(options.size() > 1);
    bool same = combo->count() == options.size();
    for (int i = 0; same && i < combo->count(); ++i) same = combo->itemText(i) == options[i];
    if (same) return;
    const int selected = combo->currentIndex();
    const QSignalBlocker block(combo);
    combo->clear();
    combo->addItems(options);
    if (selected >= 0 && selected < combo->count()) combo->setCurrentIndex(selected);
}

QString modeName(const Video& video) {
    if (video.format == KR_VIDEO_FIRECORE_BITMAP) return QStringLiteral("Firecore Bitmap / YUV");
    if (video.firecore()) return QStringLiteral("Firecore 6 bpp");
    if (video.mode5()) return QStringLiteral("Mode 5");
    if (video.format == KR_VIDEO_SMS) return QStringLiteral("Mode 4");
    switch (video.tmsMode()) {
    case 0: return QStringLiteral("Graphics I");
    case 1: return QStringLiteral("Graphics II");
    case 4: return QStringLiteral("Multicolor");
    case 5: return QStringLiteral("Multicolor + Graphics II");
    case 8: return QStringLiteral("Text");
    case 9: return QStringLiteral("Text + Graphics II");
    default: return QStringLiteral("VDP");
    }
}
} // namespace

class DebugVideoImage final : public QWidget {
public:
    explicit DebugVideoImage(QWidget* parent = nullptr) : QWidget(parent) {}

    void setImage(QImage image, QSize grid, int scale) {
        image_ = std::move(image);
        grid_ = grid;
        scale_ = scale;
        setFixedSize(image_.isNull() ? QSize(0, 0) : image_.size() * scale_);
        update();
    }

    void select(QPoint point) {
        if (point == point_) return;
        point_ = point;
        update();
    }

    std::function<void(QPoint)> onClick;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.drawImage(rect(), image_);
        if (grid_.isEmpty() || point_.x() < 0 || point_.y() < 0) return;
        const QSize cell = grid_ * scale_;
        const QRect selection(QPoint(point_.x() * cell.width(), point_.y() * cell.height()), cell);
        painter.setPen(Qt::black);
        painter.drawRect(selection.adjusted(0, 0, -1, -1));
        painter.setPen(Qt::white);
        painter.drawRect(selection.adjusted(1, 1, -2, -2));
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !onClick || grid_.isEmpty()) return;
        const QPoint point = event->position().toPoint();
        onClick(QPoint(point.x() / (grid_.width() * scale_), point.y() / (grid_.height() * scale_)));
    }

private:
    QImage image_;
    QPoint point_{-1, -1};
    QSize grid_;
    int scale_ = 1;
};

DebugVideoInspector::DebugVideoInspector(View view, QWidget* parent)
    : QWidget(parent), view_(view) {
    auto* layout = new QVBoxLayout(this);
    auto* heading = new QHBoxLayout;
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    heading->addWidget(status_, 1);
    selection_ = new QComboBox(this);
    heading->addWidget(selection_);
    palette_ = new QComboBox(this);
    palette_->setAccessibleName(trText("Палитра"));
    heading->addWidget(palette_);
    layout->addLayout(heading);
    if (view == Sprites) {
        sprites_ = new QTableWidget(this);
        sprites_->setObjectName(QStringLiteral("debugVideoSpriteList"));
        setupTable(sprites_, {trText("Запись"), QStringLiteral("X"), QStringLiteral("Y"),
                             trText("Размер"), trText("Тайл"), trText("Палитра"), trText("Флаги")});
        sprites_->setSelectionBehavior(QAbstractItemView::SelectRows);
        sprites_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        sprites_->setToolTip(trText("Порядок строк соответствует порядку обхода таблицы в снимке. H и V обозначают отражение, P обозначает приоритет."));
        connect(sprites_, &QTableWidget::currentCellChanged, this, [this] { updateDetail(); });
        layout->addWidget(sprites_, 1);
    }
    auto* scroll = new QScrollArea(this);
    scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    image_ = new DebugVideoImage(scroll);
    image_->setObjectName(QStringLiteral("debugVideoSnapshotImage"));
    scroll->setWidget(image_);
    if (view == Sprites) scroll->setFixedHeight(116);
    layout->addWidget(scroll, view == Sprites ? 0 : 1);
    image_->onClick = [this](QPoint point) {
        point_ = point;
        image_->select(point_);
        updateDetail();
    };

    detail_ = new QLabel(this);
    detail_->setObjectName(QStringLiteral("debugVideoSelection"));
    detail_->setTextFormat(Qt::RichText);
    detail_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    detail_->setWordWrap(true);
    layout->addWidget(detail_);
    connect(detail_, &QLabel::linkActivated, this, [this](const QString& link) {
        const auto pieces = link.split(QLatin1Char('/'));
        if (pieces.size() == 2 && snapshot_.available())
            emit memoryRequested(pieces[0].toUInt(), pieces[1].toUInt());
    });
    connect(selection_, &QComboBox::currentIndexChanged, this, [this] { redraw(); });
    connect(palette_, &QComboBox::currentIndexChanged, this, [this] { redraw(); });
    redraw();
}

namespace {
bool sameVideoPreview(DebugVideoInspector::View view, int option,
                      const DebugInspectionSnapshot& previous, const DebugInspectionSnapshot& next) {
    if (!previous.available() || !next.available() || previous.generation != next.generation ||
        previous.instance != next.instance || previous.info.hardware != next.info.hardware ||
        previous.info.video_format != next.info.video_format) return false;

    const Video before(previous);
    const Video after(next);
    if (before.firecore() != after.firecore() || before.interlace() != after.interlace() ||
        (before.tms() && before.tmsMode() != after.tmsMode())) return false;

    const auto sameBlock = [&](quint32 domain) { return previous.blocks.value(domain) == next.blocks.value(domain); };
    if (view == DebugVideoInspector::Framebuffer) {
        for (const char* field : {"Mode", "Height", "Displayed framebuffer", "Pixel shift"})
            if (fbValue(previous, field) != fbValue(next, field)) return false;
        const auto domain = option == 0 ? KR_MEM_32X_FB0 : KR_MEM_32X_FB1;
        const int mode = fbValue(next, "Mode");
        return sameBlock(domain) && ((mode != 1 && mode != 3) || sameBlock(KR_MEM_32X_CRAM));
    }
    if (view == DebugVideoInspector::Palette)
        return option == 1 ? sameBlock(KR_MEM_32X_CRAM) : before.tms() || before.cram == after.cram;
    if (before.vram != after.vram || (!before.tms() && before.cram != after.cram)) return false;

    const auto* a = before.reg;
    const auto* b = after.reg;
    if (view == DebugVideoInspector::Sprites) {
        if (before.mode5()) {
            return a[5] == b[5] && before.h40() == after.h40()
                && (!before.firecore() || (a[0x18] & 8) == (b[0x18] & 8))
                && field(previous, "VDP", "Sprite table", 0) == field(next, "VDP", "Sprite table", 0);
        }
        return a[0] == b[0] && a[1] == b[1] && a[5] == b[5] && a[6] == b[6]
            && before.activeLines() == after.activeLines()
            && field(previous, "VDP", "Sprite table mask", 0) == field(next, "VDP", "Sprite table mask", 0);
    }
    if (before.tms() && (a[3] != b[3] || a[4] != b[4] || a[7] != b[7])) return false;
    return view != DebugVideoInspector::Map || mapLayout(before, option) == mapLayout(after, option);
}
} // namespace

void DebugVideoInspector::setSnapshot(const DebugInspectionSnapshot& snapshot) {
    if (snapshot.available() && !(snapshot.parts & KR_INSPECT_VIDEO)) return;
    const bool same = sameVideoPreview(view_, qMax(0, selection_->currentIndex()), snapshot_, snapshot);
    snapshot_ = snapshot;
    if (same)
        status_->setText(QStringLiteral("%1 · %2").arg(stateCaption(snapshot_), modeCaption_));
    else
        redraw();
}

void DebugVideoInspector::redraw() {
    const QSignalBlocker tableBlock(sprites_);
    const Video video(snapshot_);
    if (!snapshot_.available()) {
        status_->setText(emptyCaption(snapshot_));
        if (sprites_) sprites_->setRowCount(0);
        selection_->hide();
        palette_->hide();
        image_->setImage({}, {}, 1);
        detail_->clear();
        return;
    }
    QString caption = modeName(video);
    QStringList options;
    QStringList bankOptions;
    QImage preview;
    QSize grid;
    int zoom = 1;
    const bool has32x = video.hardware & KR_HW_32X;
    if (view_ == Palette && has32x) {
        options = {QStringLiteral("VDP"), QStringLiteral("32X")};
    } else if (view_ == Framebuffer && has32x) {
        const int displaying = fbValue(snapshot_, "Displayed framebuffer") & 1;
        for (int page = 0; page < 2; ++page) {
            QString label = trText("Буфер %1").arg(page);
            if (displaying == page) label += QStringLiteral(" (%1)").arg(trText("активный"));
            options << label;
        }
    } else if (view_ == Map && video.mode5()) {
        options = {trText("Плоскость A"), trText("Плоскость B"), trText("Окно")};
    } else if (view_ == Tiles && video.tms() && (video.tmsMode() & 1)) {
        for (int block = 0; block < 3; ++block) options << trText("Секция %1").arg(block + 1);
    }
    if (view_ == Tiles && !video.tms() && video.decodable()) {
        for (int bank = 0; bank < video.banks(); ++bank) bankOptions << trText("Палитра %1").arg(bank);
    }
    setOptions(selection_, options);
    setOptions(palette_, bankOptions);
    const int option = qMax(0, selection_->currentIndex());
    if (view_ == Palette) {
        const auto colors = snapshot_.blocks.value(KR_MEM_32X_CRAM);
        const int count = option == 1 ? colors.size() / 2 :
            video.tms() ? 16 : video.cram.size() / qMax(1, video.colorBytes());
        if (video.tms()) caption += QStringLiteral(" · ") + trText("Фиксированная палитра TMS");
        if (count > 0) {
            grid = QSize(20, 16);
            preview = QImage(16 * grid.width(), ((count + 15) / 16) * grid.height(), QImage::Format_RGB32);
            preview.fill(Qt::black);
            QPainter painter(&preview);
            for (int i = 0; i < count; ++i) {
                const auto color = option == 1 ? packedColor(wordAt(colors, i * 2), 5) : video.color(i);
                const QRect rect((i % 16) * grid.width(), (i / 16) * grid.height(), grid.width(), grid.height());
                painter.fillRect(rect.adjusted(0, 0, -1, -1), QColor::fromRgb(color));
            }
        }
    } else if (view_ == Framebuffer) {
        const int mode = fbValue(snapshot_, "Mode");
        const auto page = snapshot_.blocks.value(option == 0 ? KR_MEM_32X_FB0 : KR_MEM_32X_FB1);
        const auto colors = snapshot_.blocks.value(KR_MEM_32X_CRAM);
        const bool shift = fbValue(snapshot_, "Pixel shift") != 0;
        if (page.size() == 0x20000 && mode >= 1 && mode <= 3 && (mode == 2 || colors.size() == 512)) {
            const int lines = std::clamp(int(fbValue(snapshot_, "Height", 224)), 1, 256);
            const std::array<QString, 3> formats{ QStringLiteral("Packed pixel"),
                                                 QStringLiteral("Direct color"), QStringLiteral("RLE") };
            caption = QStringLiteral("32X %1").arg(formats[mode - 1]);
            grid = QSize(1, 1);
            preview = QImage(320, lines, QImage::Format_RGB32);
            for (int y = 0; y < lines; ++y) {
                const FrameLine row = framebufferLine(page, colors, y, mode, shift);
                auto* line = reinterpret_cast<QRgb*>(preview.scanLine(y));
                for (int x = 0; x < preview.width(); ++x) line[x] = packedColor(row[x], 5);
            }
        } else if (!mode) {
            caption = trText("Вывод 32X отключён");
        }
    } else if (video.decodable()) {
        grid = QSize(video.tileWidth(), video.tileHeight());
        if (view_ == Tiles) {
            constexpr int columns = 16;
            const int count = video.tileCount();
            zoom = 2;
            preview = QImage(columns * grid.width(), ((count + columns - 1) / columns) * grid.height(),
                             QImage::Format_RGB32);
            preview.fill(Qt::black);
            const int bank = qMax(0, palette_->currentIndex());
            for (int i = 0; i < count; ++i) {
                const int mapRow = video.tms() ? option * 8 : 0;
                drawTile(preview, video, Tile{i, bank}, (i % columns) * grid.width(),
                         (i / columns) * grid.height(), mapRow);
            }
        } else if (view_ == Map) {
            const MapLayout map = mapLayout(video, option);
            preview = QImage(map.columns * grid.width(), map.rows * grid.height(), QImage::Format_RGB32);
            for (int y = 0; y < map.rows; ++y) {
                for (int x = 0; x < map.columns; ++x) {
                    const auto tile = tileFromMap(video, map, map.address(x, y));
                    drawTile(preview, video, tile, x * grid.width(), y * grid.height(), y);
                }
            }
        } else if (view_ == Sprites) {
            const auto list = spriteList(video);
            sprites_->setRowCount(list.size());
            int row = 0;
            int selectedRow = 0;
            for (const auto& sprite : list) {
                if (sprite.index == selectedSprite_) selectedRow = row;
                cell(sprites_, row, 0, QString::number(sprite.index));
                cell(sprites_, row, 1, QString::number(sprite.x));
                cell(sprites_, row, 2, QString::number(sprite.y));
                cell(sprites_, row, 3, QStringLiteral("%1×%2").arg(sprite.columns * 8 * sprite.zoom)
                                               .arg(sprite.rows * video.tileHeight() * sprite.zoom));
                cell(sprites_, row, 4, hexNumber(sprite.tile.index, 3));
                cell(sprites_, row, 5, QString::number(sprite.tile.palette));
                cell(sprites_, row, 6, tileFlags(sprite.tile));
                ++row;
            }
            if (!list.isEmpty()) sprites_->setCurrentCell(selectedRow, 0);
            if (list.isEmpty()) caption += QStringLiteral(" · ") + trText("Пустая таблица спрайтов");
        }
    }
    if (view_ != Palette && view_ != Framebuffer && !video.decodable()) {
        if (sprites_) sprites_->setRowCount(0);
        caption += QStringLiteral(" · ") + trText("Просмотр этого формата недоступен");
    }
    modeCaption_ = caption;
    status_->setText(QStringLiteral("%1 · %2").arg(stateCaption(snapshot_), modeCaption_));
    if (!preview.isNull() && !grid.isEmpty() &&
        (point_.x() < 0 || point_.y() < 0 || point_.x() * grid.width() >= preview.width() ||
         point_.y() * grid.height() >= preview.height()))
        point_ = QPoint(0, 0);
    image_->setImage(std::move(preview), grid, zoom);
    image_->select(point_);
    updateDetail();
}

void DebugVideoInspector::updateDetail() {
    if (!snapshot_.available()) return;
    QStringList details;
    const Video video(snapshot_);
    const int option = qMax(0, selection_->currentIndex());
    const bool valid = point_.x() >= 0 && point_.y() >= 0;
    const auto paletteLink = [&](int bank) {
        return memoryLink(KR_MEM_CRAM, bank * video.bankColors() * video.colorBytes(), QStringLiteral("CRAM"));
    };
    if (view_ == Framebuffer) {
        const quint32 domain = option == 0 ? KR_MEM_32X_FB0 : KR_MEM_32X_FB1;
        const auto page = snapshot_.blocks.value(domain);
        if (!page.isEmpty()) {
            const quint32 address = valid ? point_.y() * 2 : 0;
            details << memoryLink(domain, address, trText("Таблица строк"));
            if (valid && fbValue(snapshot_, "Mode"))
                details << memoryLink(domain, wordAt(page, address) * 2, trText("Данные строки"));
        }
    } else if (view_ == Palette && valid) {
        const int index = point_.y() * 16 + point_.x();
        const auto colors = snapshot_.blocks.value(KR_MEM_32X_CRAM);
        const bool is32x = option == 1;
        const int stride = is32x ? 2 : video.colorBytes();
        const int count = is32x ? colors.size() / stride :
            video.tms() ? 16 : video.cram.size() / qMax(1, stride);
        if (index < count) {
            const auto word = wordAt(colors, index * 2);
            const auto rgb = is32x ? packedColor(word, 5) : video.color(index);
            details << QStringLiteral("%1 · #%2").arg(trText("Цвет %1").arg(index),
                                                         hexNumber(rgb & 0xffffff, 6));
            if (stride) details << memoryLink(is32x ? KR_MEM_32X_CRAM : KR_MEM_CRAM,
                                              index * stride, QStringLiteral("CRAM"));
            if (is32x && (word & 0x8000)) details << QStringLiteral("P");
        }
    } else if (!video.decodable()) {
        detail_->clear();
        return;
    } else if (view_ == Tiles && valid) {
        const int index = point_.y() * 16 + point_.x();
        if (index < video.tileCount()) {
            details << trText("Тайл %1").arg(hexNumber(index, 3));
            details << memoryLink(KR_MEM_VRAM, video.patternAddress(index, option * 8), QStringLiteral("VRAM"));
            if (!video.tms()) details << paletteLink(qMax(0, palette_->currentIndex()));
        }
    } else if (view_ == Map && valid) {
        const MapLayout map = mapLayout(video, option);
        const auto address = map.address(point_.x(), point_.y()) & video.mask();
        const Tile tile = tileFromMap(video, map, address);
        details << QStringLiteral("(%1, %2)").arg(point_.x()).arg(point_.y());
        details << memoryLink(KR_MEM_VRAM, address, trText("Запись"));
        details << memoryLink(KR_MEM_VRAM, video.patternAddress(tile.index, point_.y()),
                              trText("Тайл %1").arg(hexNumber(tile.index, 3)));
        if (!video.tms()) details << paletteLink(tile.palette) << tileFlags(tile);
    } else if (view_ == Sprites) {
        const auto list = spriteList(video);
        const int row = sprites_->currentRow();
        if (row >= 0 && row < list.size()) {
            const Sprite& sprite = list.at(row);
            selectedSprite_ = sprite.index;
            details << memoryLink(KR_MEM_VRAM, sprite.entry, trText("Запись"));
            details << memoryLink(KR_MEM_VRAM, sprite.pattern, trText("Тайл %1").arg(hexNumber(sprite.tile.index, 3)));
            if (!video.tms()) details << paletteLink(sprite.tile.palette);
            image_->setImage(spritePreview(video, sprite), {}, 2);
        } else {
            image_->setImage({}, {}, 1);
        }
    }
    detail_->setText(details.join(QStringLiteral(" · ")));
}
