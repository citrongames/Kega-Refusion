#include "DebugWatchDialog.h"
#include "i18n/Localization.h"

#include <QAbstractTableModel>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFont>
#include <QFormLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace {
constexpr int nameLimit = 128;
constexpr int changeHighlightMs = 1000;
enum Column { Name, Address, Value, ColumnCount };

QString text(const char* source) {
    return Localization::text(QString::fromUtf8(source));
}

QString hex(quint64 value, int digits = 0) {
    return QString::number(value, 16).toUpper().rightJustified(digits, QLatin1Char('0'));
}

QString addressText(quint32 offset, quint32 base = 0) {
    return QStringLiteral("0x%1").arg(hex(quint64(base) + offset));
}

QString consoleName(quint32 instance) {
    return instance ? text("Консоль B") : text("Консоль A");
}

QString domainName(const DebugWatchDomain& domain) {
    return Localization::text(QString::fromUtf8(domain.native.name));
}

const DebugWatchDomain* findDomain(const QVector<DebugWatchDomain>& domains, quint32 instance, quint32 domain) {
    const auto found = std::find_if(domains.cbegin(), domains.cend(), [&](const auto& item) {
        return item.instance == instance && item.native.id == domain;
    });
    return found == domains.cend() ? nullptr : &*found;
}

bool validEntry(const DebugWatchEntry& entry) {
    return entry.instance <= 1 && entry.domain &&
        (entry.width == 1 || entry.width == 2 || entry.width == 4) &&
        entry.format >= DebugWatchEntry::Hex && entry.format <= DebugWatchEntry::Binary &&
        entry.order >= DebugWatchEntry::Native && entry.order <= DebugWatchEntry::Little &&
        quint64(entry.offset) + entry.width <= quint64(1) + std::numeric_limits<quint32>::max();
}

bool sameLocation(const DebugWatchEntry& a, const DebugWatchEntry& b) {
    return a.instance == b.instance && a.domain == b.domain && a.offset == b.offset && a.width == b.width;
}

QString formatValue(const QByteArray& bytes, bool nativeBig, const DebugWatchEntry& entry) {
    if (bytes.isEmpty()) return QStringLiteral("—");
    const bool big = entry.order == DebugWatchEntry::Big || (entry.order == DebugWatchEntry::Native && nativeBig);
    quint32 value = 0;
    const int size = bytes.size();
    for (int i = 0; i < size; ++i)
        value = (value << 8) | quint8(bytes.at(big ? i : size - 1 - i));
    switch (entry.format) {
    case DebugWatchEntry::Hex:
        return QStringLiteral("0x%1").arg(hex(value, size * 2));
    case DebugWatchEntry::Signed: {
        const quint64 range = quint64(1) << (size * 8);
        return QString::number(value >= range / 2 ? qint64(value) - qint64(range) : qint64(value));
    }
    case DebugWatchEntry::Binary:
        return QStringLiteral("0b%1").arg(QString::number(value, 2).rightJustified(size * 8, QLatin1Char('0')));
    case DebugWatchEntry::Unsigned:
        return QString::number(value);
    }
    return {};
}

QPushButton* makeButton(QWidget* parent, const char* source, const char* name) {
    auto* button = new QPushButton(text(source), parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setAutoDefault(false);
    return button;
}
}

class DebugWatchModel final : public QAbstractTableModel {
public:
    explicit DebugWatchModel(QObject* parent) : QAbstractTableModel(parent) { clock_.start(); }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : rows_.size(); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : ColumnCount; }
    int projectCount() const { return projectCount_; }
    bool isProjectRow(int row) const { return row >= 0 && row < projectCount_; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (section) {
        case Name: return text("Имя");
        case Address: return text("Адрес");
        case Value: return text("Значение");
        default: return {};
        }
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) return {};
        const auto& row = rows_.at(index.row());
        if (index.column() == Value && row.highlight) {
            if (role == Qt::BackgroundRole) return QColor(233, 190, 80);
            if (role == Qt::ForegroundRole) return QColor(Qt::black);
            if (role == Qt::FontRole) {
                QFont emphasis;
                emphasis.setBold(true);
                return emphasis;
            }
        }
        if (role == Qt::ToolTipRole && index.column() == Address) {
            const auto domain = row.domain.id ? text(row.domain.name) :
                text("Область %1 (недоступна)").arg(row.entry.domain);
            return QStringLiteral("%1 · %2\n%3: %4").arg(consoleName(row.entry.instance), domain,
                text("Смещение (hex)"), addressText(row.entry.offset));
        }
        if (role == Qt::ToolTipRole && index.column() == Value && row.changes) {
            return QStringLiteral("%1: %2\n%3: %4\n%5: %6").arg(
                text("Значение"), formatValue(row.current, row.bigEndian, row.entry),
                text("Предыдущее"), formatValue(row.previous, row.bigEndian, row.entry),
                text("Изменения"), QString::number(row.changes));
        }
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
        switch (index.column()) {
        case Name:
            if (!isProjectRow(index.row())) return text("%1 (Lua)").arg(row.entry.name);
            return row.entry.name.isEmpty() ? QStringLiteral("—") : row.entry.name;
        case Address:
            return (row.domain.id ? QString() : QStringLiteral("+")) +
                addressText(row.entry.offset, row.domain.address_base);
        case Value: return formatValue(row.current, row.bigEndian, row.entry);
        default: return {};
        }
    }

    const DebugWatchEntry* entry(int row) const {
        return row >= 0 && row < rows_.size() ? &rows_.at(row).entry : nullptr;
    }

    QVector<DebugWatchEntry> projectEntries() const { return entries(0, projectCount_); }
    QVector<DebugWatchEntry> scriptEntries() const { return entries(projectCount_, rows_.size()); }

    void replaceProjectEntries(const QVector<DebugWatchEntry>& entries) {
        beginResetModel();
        rows_.clear();
        rows_.reserve(entries.size());
        for (const auto& entry : entries) rows_.append(Row(entry));
        projectCount_ = rows_.size();
        endResetModel();
    }

    void replaceScriptEntries(const QVector<DebugWatchEntry>& declarations) {
        if (rows_.size() > projectCount_) {
            beginRemoveRows({}, projectCount_, rows_.size() - 1);
            rows_.erase(rows_.begin() + projectCount_, rows_.end());
            endRemoveRows();
        }
        if (declarations.isEmpty()) return;

        // Saved projects accept any nonzero ID. Remap a conflicting temporary ID
        // so each requested value still has a unique ID within this snapshot.
        QSet<quint32> ids;
        for (const auto& row : rows_) ids.insert(row.entry.id);
        quint32 spare = std::numeric_limits<quint32>::max();
        beginInsertRows({}, projectCount_, projectCount_ + declarations.size() - 1);
        for (auto entry : declarations) {
            if (!entry.id || ids.contains(entry.id)) {
                while (ids.contains(spare)) --spare;
                entry.id = spare--;
            }
            ids.insert(entry.id);
            rows_.append(Row(entry));
        }
        endInsertRows();
    }

    void append(const DebugWatchEntry& entry) {
        beginInsertRows({}, projectCount_, projectCount_);
        rows_.insert(projectCount_, Row(entry));
        ++projectCount_;
        endInsertRows();
    }

    void replace(int position, const DebugWatchEntry& entry) {
        auto& row = rows_[position];
        if (!sameLocation(row.entry, entry)) row = Row(entry);
        else row.entry = entry;
        emit dataChanged(index(position, 0), index(position, ColumnCount - 1));
    }

    void erase(int row) {
        beginRemoveRows({}, row, row);
        rows_.removeAt(row);
        --projectCount_;
        endRemoveRows();
    }

    void applySamples(const QVector<DebugWatchDomain>& domains,
                      const QVector<DebugWatchSample>& values, bool reset) {
        QHash<quint32, const DebugWatchSample*> samples;
        samples.reserve(values.size());
        for (const auto& value : values) samples.insert(value.id, &value);
        const qint64 now = clock_.elapsed();
        for (int i = 0; i < rows_.size(); ++i) {
            auto& row = rows_[i];
            const auto* domain = findDomain(domains, row.entry.instance, row.entry.domain);
            const auto* sample = samples.value(row.entry.id, nullptr);
            const kr_inspect_domain native = domain ? domain->native : kr_inspect_domain{};
            const bool valid = domain && sample && sample->bytes.size() == row.entry.width &&
                quint64(row.entry.offset) + row.entry.width <= native.size;
            const bool changedDomain = row.domain.id != native.id || row.domain.size != native.size ||
                row.domain.address_base != native.address_base || qstrcmp(row.domain.name, native.name) != 0;
            row.domain = native;
            const bool changedOrder = valid && row.bigEndian != sample->bigEndian;
            const bool newBaseline = reset || changedDomain || !valid || changedOrder;
            const QByteArray next = valid ? sample->bytes : QByteArray{};
            const bool different = row.current != next;
            const bool hadHighlight = row.highlight;
            const bool hadHistory = row.changes != 0;
            if (newBaseline) {
                row.previous.clear();
                row.changes = 0;
                row.highlightUntil = 0;
            } else if (!row.current.isEmpty() && different) {
                row.previous = row.current;
                if (row.changes < std::numeric_limits<quint64>::max()) ++row.changes;
                row.highlightUntil = now + changeHighlightMs;
            }
            row.highlight = now < row.highlightUntil;
            if (valid) row.bigEndian = sample->bigEndian;
            row.current = next;
            if (different || changedOrder || (newBaseline && hadHistory) || hadHighlight != row.highlight)
                emit dataChanged(index(i, changedDomain ? Address : Value), index(i, Value));
            else if (changedDomain)
                emit dataChanged(index(i, Address), index(i, Address));
        }
    }

    void clearHistory() {
        for (int i = 0; i < rows_.size(); ++i) {
            auto& row = rows_[i];
            if (!row.changes) continue;
            row.previous.clear();
            row.changes = 0;
            row.highlightUntil = 0;
            row.highlight = false;
            emit dataChanged(index(i, Value), index(i, Value));
        }
    }

private:
    struct Row {
        explicit Row(const DebugWatchEntry& item) : entry(item) {}
        DebugWatchEntry entry;
        QByteArray current;
        QByteArray previous;
        kr_inspect_domain domain{};
        quint64 changes = 0;
        qint64 highlightUntil = 0;
        bool bigEndian = false;
        bool highlight = false;
    };
    QVector<DebugWatchEntry> entries(int first, int end) const {
        QVector<DebugWatchEntry> result;
        result.reserve(end - first);
        for (int i = first; i < end; ++i) result.append(rows_.at(i).entry);
        return result;
    }

    QVector<Row> rows_;
    int projectCount_ = 0;
    QElapsedTimer clock_;
};

DebugWatchDialog::DebugWatchDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint) {
    setObjectName(QStringLiteral("debugWatchDialog"));
    setWindowTitle(text("Наблюдение за памятью"));
    setWindowModality(Qt::NonModal);
    resize(470, 320);

    auto* outer = new QVBoxLayout(this);
    auto* actions = new QHBoxLayout;
    add_ = makeButton(this, "Добавить…", "debugWatchAdd");
    edit_ = makeButton(this, "Изменить…", "debugWatchEdit");
    remove_ = makeButton(this, "Удалить", "debugWatchRemove");
    memory_ = makeButton(this, "Открыть в памяти", "debugWatchMemory");
    for (auto* button : {add_, edit_, remove_, memory_}) actions->addWidget(button);
    actions->addStretch();
    outer->addLayout(actions);

    status_ = new QLabel(text("Нет данных снимка"), this);
    status_->setObjectName(QStringLiteral("debugWatchStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    outer->addWidget(status_);
    feedback_ = new QLabel(this);
    feedback_->setObjectName(QStringLiteral("debugWatchFeedback"));
    feedback_->setTextFormat(Qt::PlainText);
    feedback_->setWordWrap(true);
    feedback_->hide();
    outer->addWidget(feedback_);

    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("debugWatchTable"));
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(table_->fontMetrics().height() + 8);
    model_ = new DebugWatchModel(table_);
    table_->setModel(model_);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(Name, QHeaderView::Stretch);
    table_->setColumnWidth(Address, 110);
    table_->setColumnWidth(Value, 135);
    outer->addWidget(table_, 1);

    connect(add_, &QPushButton::clicked, this, [this] { editWatch(-1); });
    connect(edit_, &QPushButton::clicked, this, [this] { editWatch(table_->currentIndex().row()); });
    connect(remove_, &QPushButton::clicked, this, &DebugWatchDialog::removeWatch);
    connect(memory_, &QPushButton::clicked, this, [this] { navigate(table_->currentIndex().row()); });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) { editWatch(index.row()); });
    connect(table_, &QTableView::customContextMenuRequested, this, &DebugWatchDialog::showTableMenu);
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { updateActions(); });
    updateActions();
}

QVector<DebugWatchEntry> DebugWatchDialog::entries() const {
    return model_->projectEntries();
}

QVector<DebugWatchEntry> DebugWatchDialog::scriptEntries() const {
    return model_->scriptEntries();
}

quint32 DebugWatchDialog::unusedId() {
    const auto current = entries();
    const auto temporary = scriptEntries();
    const auto usesId = [this](const auto& entry) { return entry.id == nextId_; };
    while (!nextId_ || std::any_of(current.cbegin(), current.cend(), usesId) ||
           std::any_of(temporary.cbegin(), temporary.cend(), usesId)) ++nextId_;
    return nextId_++;
}

void DebugWatchDialog::setEntries(const QVector<DebugWatchEntry>& entries) {
    QVector<DebugWatchEntry> valid;
    QSet<quint32> ids;
    valid.reserve(std::min(entries.size(), qsizetype(DebugWatchLimit)));
    // Keep valid saved IDs before allocating replacements for missing or duplicate ones.
    for (const auto& entry : entries) {
        if (!validEntry(entry)) continue;
        auto item = entry;
        item.name = item.name.left(nameLimit);
        if (!item.id || ids.contains(item.id)) item.id = 0;
        else ids.insert(item.id);
        valid.append(std::move(item));
        if (valid.size() == DebugWatchLimit) break;
    }
    for (auto& entry : valid) {
        if (entry.id) continue;
        while (!nextId_ || ids.contains(nextId_)) ++nextId_;
        entry.id = nextId_++;
        ids.insert(entry.id);
    }
    model_->replaceProjectEntries(valid);
    model_->replaceScriptEntries(scriptDeclarations_);
    model_->applySamples(domains_, {}, true);
    showFeedback({});
    updateActions();
    emit entriesChanged();
    emit inspectionChanged();
}

void DebugWatchDialog::setScriptEntries(const QVector<DebugWatchEntry>& entries) {
    if (entries.size() == scriptDeclarations_.size() &&
        std::equal(entries.cbegin(), entries.cend(), scriptDeclarations_.cbegin(),
            [](const auto& a, const auto& b) {
                return a.id == b.id && sameLocation(a, b) && a.name == b.name &&
                       a.format == b.format && a.order == b.order;
            })) return;
    scriptDeclarations_ = entries;
    model_->replaceScriptEntries(scriptDeclarations_);
    updateActions();
    if (isVisible()) emit inspectionChanged();
}

void DebugWatchDialog::addWatch(const DebugWatchEntry& entry) {
    if (model_->projectCount() >= DebugWatchLimit) {
        showFeedback(text("Достигнут предел в %1 наблюдений.").arg(DebugWatchLimit));
        return;
    }
    if (!validEntry(entry)) return;
    auto item = entry;
    item.id = unusedId();
    item.name = item.name.left(nameLimit);
    model_->append(item);
    table_->selectRow(model_->projectCount() - 1);
    table_->scrollTo(table_->currentIndex());
    showFeedback({});
    updateActions();
    emit entriesChanged();
    emit inspectionChanged();
}

void DebugWatchDialog::setSnapshot(const DebugInspectionSnapshot& snapshot) {
    if (!isVisible()) return;
    const bool sameContext = gameKey_ == snapshot.gameKey && generation_ == snapshot.generation;
    gameKey_ = snapshot.gameKey;
    generation_ = snapshot.generation;
    if (snapshot.available()) {
        domains_ = snapshot.watchDomains;
        activeInstance_ = snapshot.instance;
        activeDomain_ = snapshot.memoryDomain;
        model_->applySamples(domains_, snapshot.watchValues, !sameContext);
    } else {
        resetSamples();
    }
    const QString message = snapshot.available() ? QString() :
        snapshot.status.isEmpty() ? text("Нет данных снимка") : snapshot.status;
    if (status_->text() != message) {
        status_->setText(message);
        status_->setVisible(!message.isEmpty());
    }
    updateActions();
}

void DebugWatchDialog::editWatch(int row) {
    const auto* existing = model_->entry(row);
    const bool adding = !existing;
    if (adding && (domains_.isEmpty() || model_->projectCount() >= DebugWatchLimit)) return;
    if (existing && !model_->isProjectRow(row)) return;
    DebugWatchEntry original = existing ? *existing : DebugWatchEntry{};
    if (adding) {
        original.instance = activeInstance_;
        original.domain = activeDomain_;
    }
    const auto catalog = domains_;
    const auto gameKey = gameKey_;
    const auto generation = generation_;
    QDialog editor(this, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    editor.setObjectName(QStringLiteral("debugWatchEditor"));
    editor.setWindowTitle(adding ? text("Добавить наблюдение") : text("Изменить наблюдение"));
    auto* layout = new QVBoxLayout(&editor);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(original.name, &editor);
    name->setObjectName(QStringLiteral("debugWatchEditorName"));
    name->setMaxLength(nameLimit);
    name->setMinimumWidth(180);
    form->addRow(text("Имя"), name);
    auto* address = new QLineEdit(&editor);
    address->setObjectName(QStringLiteral("debugWatchEditorOffset"));
    address->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("(?:0[xX])?[0-9a-fA-F]{1,8}")), address));
    form->addRow(text("Адрес (hex)"), address);
    auto* width = new QComboBox(&editor);
    width->setObjectName(QStringLiteral("debugWatchEditorWidth"));
    for (const quint32 value : {1u, 2u, 4u})
        width->addItem(text("%1 бит").arg(value * 8), value);
    width->setCurrentIndex(width->findData(original.width));
    form->addRow(text("Размер"), width);
    layout->addLayout(form);

    auto* advancedToggle = new QToolButton(&editor);
    advancedToggle->setObjectName(QStringLiteral("debugWatchEditorAdvanced"));
    advancedToggle->setText(text("Дополнительные параметры"));
    advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedToggle->setArrowType(Qt::RightArrow);
    advancedToggle->setCheckable(true);
    layout->addWidget(advancedToggle, 0, Qt::AlignLeft);
    auto* advanced = new QWidget(&editor);
    auto* advancedForm = new QFormLayout(advanced);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    auto* instance = new QComboBox(advanced);
    instance->setObjectName(QStringLiteral("debugWatchEditorConsole"));
    for (const quint32 value : {0u, 1u}) {
        if ((!adding && value == original.instance) ||
            std::any_of(catalog.cbegin(), catalog.cend(), [value](const auto& domain) { return domain.instance == value; }))
            instance->addItem(consoleName(value), value);
    }
    const int selectedConsole = instance->findData(original.instance);
    instance->setCurrentIndex(selectedConsole >= 0 ? selectedConsole : 0);
    advancedForm->addRow(text("Консоль"), instance);
    auto* domain = new QComboBox(advanced);
    domain->setObjectName(QStringLiteral("debugWatchEditorDomain"));
    const auto updateAddress = [&] {
        const auto* region = findDomain(catalog, instance->currentData().toUInt(), domain->currentData().toUInt());
        const quint32 base = region ? region->native.address_base : 0;
        address->setPlaceholderText(addressText(0, base));
        if (!adding && !address->isModified()) address->setText(addressText(original.offset, base));
    };
    const auto selectDomains = [&] {
        const auto console = instance->currentData().toUInt();
        const QSignalBlocker blocker(domain);
        domain->clear();
        for (const auto& item : catalog)
            if (item.instance == console) domain->addItem(domainName(item), item.native.id);
        if (!adding && console == original.instance && domain->findData(original.domain) < 0)
            domain->addItem(text("Область %1 (недоступна)").arg(original.domain), original.domain);
        int selection = domain->findData(original.domain);
        if (selection < 0) selection = domain->findData(quint32(KR_MEM_RAM));
        if (selection < 0) selection = domain->findData(quint32(KR_MEM_ZRAM));
        domain->setCurrentIndex(selection >= 0 ? selection : 0);
        updateAddress();
    };
    selectDomains();
    connect(instance, &QComboBox::currentIndexChanged, &editor, selectDomains);
    connect(domain, &QComboBox::currentIndexChanged, &editor, updateAddress);
    advancedForm->addRow(text("Область памяти"), domain);
    auto* format = new QComboBox(advanced);
    format->setObjectName(QStringLiteral("debugWatchEditorFormat"));
    format->addItem(QStringLiteral("Hex"), int(DebugWatchEntry::Hex));
    format->addItem(text("Без знака"), int(DebugWatchEntry::Unsigned));
    format->addItem(text("Со знаком"), int(DebugWatchEntry::Signed));
    format->addItem(text("Двоичный"), int(DebugWatchEntry::Binary));
    format->setCurrentIndex(format->findData(int(original.format)));
    advancedForm->addRow(text("Формат"), format);
    auto* order = new QComboBox(advanced);
    order->setObjectName(QStringLiteral("debugWatchEditorOrder"));
    order->addItem(text("Порядок области"), int(DebugWatchEntry::Native));
    order->addItem(QStringLiteral("Big-endian"), int(DebugWatchEntry::Big));
    order->addItem(QStringLiteral("Little-endian"), int(DebugWatchEntry::Little));
    order->setCurrentIndex(order->findData(int(original.order)));
    advancedForm->addRow(text("Порядок байтов"), order);
    advanced->hide();
    layout->addWidget(advanced);
    connect(advancedToggle, &QToolButton::toggled, &editor, [advancedToggle, advanced](bool shown) {
        advanced->setVisible(shown);
        advancedToggle->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
    });

    auto* error = new QLabel(&editor);
    error->setTextFormat(Qt::PlainText);
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
    layout->addWidget(buttons);
    DebugWatchEntry updated = original;
    connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &editor, [&] {
        if (gameKey_ != gameKey || generation_ != generation) {
            error->setText(text("Контекст памяти изменился. Откройте редактор заново."));
            error->show();
            return;
        }
        bool ok = false;
        const quint32 absolute = address->text().toUInt(&ok, 16);
        updated.instance = instance->currentData().toUInt();
        updated.domain = domain->currentData().toUInt();
        updated.width = width->currentData().toUInt();
        updated.format = DebugWatchEntry::Format(format->currentData().toInt());
        updated.order = DebugWatchEntry::Order(order->currentData().toInt());
        updated.name = name->text().trimmed();
        const auto* region = findDomain(domains_, updated.instance, updated.domain);
        const quint32 base = region ? region->native.address_base : 0;
        updated.offset = absolute >= base ? absolute - base : absolute;
        const bool available = region && quint64(updated.offset) + updated.width <= region->native.size;
        if (!ok || !validEntry(updated) || (!available && (adding || !sameLocation(original, updated)))) {
            error->setText(text("Значение выходит за границы доступной области памяти."));
            error->show();
            return;
        }
        editor.accept();
    });
    if (adding) address->setFocus();
    else name->setFocus();
    if (editor.exec() != QDialog::Accepted) return;

    if (adding) {
        addWatch(updated);
        return;
    }
    const auto* current = model_->entry(row);
    if (!current || current->id != original.id) return;
    model_->replace(row, updated);
    showFeedback({});
    updateActions();
    emit entriesChanged();
    emit inspectionChanged();
}

void DebugWatchDialog::removeWatch() {
    const int row = table_->currentIndex().row();
    if (!model_->isProjectRow(row)) return;
    model_->erase(row);
    if (model_->rowCount()) table_->selectRow(std::min(row, model_->rowCount() - 1));
    showFeedback({});
    updateActions();
    emit entriesChanged();
    emit inspectionChanged();
}

bool DebugWatchDialog::canNavigate(int row) const {
    const auto* entry = model_->entry(row);
    if (!entry) return false;
    const auto* domain = findDomain(domains_, entry->instance, entry->domain);
    return domain && entry->offset < domain->native.size;
}

void DebugWatchDialog::navigate(int row) {
    if (!canNavigate(row)) return;
    const auto* entry = model_->entry(row);
    emit navigateRequested(entry->instance, entry->domain, entry->offset);
}

void DebugWatchDialog::showTableMenu(const QPoint& point) {
    const QModelIndex index = table_->indexAt(point);
    if (!index.isValid()) return;
    table_->selectRow(index.row());
    QMenu menu(this);
    const QPersistentModelIndex selected(index);
    auto* open = menu.addAction(text("Открыть в памяти"), this, [this, selected] {
        if (selected.isValid()) navigate(selected.row());
    });
    open->setEnabled(canNavigate(index.row()));
    const bool editable = model_->isProjectRow(index.row());
    auto* edit = menu.addAction(text("Изменить…"), this, [this, selected] {
        if (selected.isValid()) editWatch(selected.row());
    });
    edit->setEnabled(editable);
    auto* remove = menu.addAction(text("Удалить"), this, [this, selected] {
        if (!selected.isValid()) return;
        table_->selectRow(selected.row());
        removeWatch();
    });
    remove->setEnabled(editable);
    menu.addSeparator();
    menu.addAction(text("Сбросить историю"), this, [this] { model_->clearHistory(); });
    menu.exec(table_->viewport()->mapToGlobal(point));
}

void DebugWatchDialog::updateActions() {
    const int row = table_->currentIndex().row();
    add_->setEnabled(!domains_.isEmpty() && model_->projectCount() < DebugWatchLimit);
    edit_->setEnabled(model_->isProjectRow(row));
    remove_->setEnabled(model_->isProjectRow(row));
    memory_->setEnabled(canNavigate(row));
}

void DebugWatchDialog::resetSamples() {
    domains_.clear();
    model_->applySamples({}, {}, true);
}

void DebugWatchDialog::showFeedback(const QString& message) {
    feedback_->setText(message);
    feedback_->setVisible(!message.isEmpty());
}

void DebugWatchDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    emit inspectionChanged();
}

void DebugWatchDialog::hideEvent(QHideEvent* event) {
    QDialog::hideEvent(event);
    resetSamples();
    status_->setText(text("Нет данных снимка"));
    status_->show();
    showFeedback({});
    updateActions();
    emit inspectionChanged();
}
