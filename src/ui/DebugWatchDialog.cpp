#include "DebugWatchDialog.h"
#include "i18n/Localization.h"

#include <QAbstractTableModel>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
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
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace {
constexpr int nameLimit = 128;
enum Column { Name, Offset, Domain, Console, Value, Previous, Changes, ColumnCount };

QString text(const char* source) {
    return Localization::text(QString::fromUtf8(source));
}

QString hex(quint32 value, int digits = 0) {
    return QString::number(value, 16).toUpper().rightJustified(digits, QLatin1Char('0'));
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
    explicit DebugWatchModel(QObject* parent) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : rows_.size(); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : ColumnCount; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (section) {
        case Name: return text("Имя");
        case Offset: return text("Смещение");
        case Domain: return text("Область памяти");
        case Console: return text("Консоль");
        case Value: return text("Значение");
        case Previous: return text("Предыдущее");
        case Changes: return text("Изменения");
        default: return {};
        }
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) return {};
        const auto& row = rows_.at(index.row());
        if (index.column() == Value && row.highlight) {
            if (role == Qt::BackgroundRole) return QColor(233, 190, 80);
            if (role == Qt::ForegroundRole) return QColor(Qt::black);
        }
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
        switch (index.column()) {
        case Name: return row.entry.name.isEmpty() ? QStringLiteral("—") : row.entry.name;
        case Offset: return QStringLiteral("0x%1").arg(hex(row.entry.offset));
        case Domain: return row.domainName.isEmpty() ? text("Область %1").arg(row.entry.domain) : row.domainName;
        case Console: return consoleName(row.entry.instance);
        case Value: return formatValue(row.current, row.bigEndian, row.entry);
        case Previous: return formatValue(row.previous, row.bigEndian, row.entry);
        case Changes: return row.current.isEmpty() ? QStringLiteral("—") : QString::number(row.changes);
        default: return {};
        }
    }

    const DebugWatchEntry* entry(int row) const {
        return row >= 0 && row < rows_.size() ? &rows_.at(row).entry : nullptr;
    }

    QVector<DebugWatchEntry> entries() const {
        QVector<DebugWatchEntry> result;
        result.reserve(rows_.size());
        for (const auto& row : rows_) result.append(row.entry);
        return result;
    }

    void replaceAll(const QVector<DebugWatchEntry>& entries) {
        beginResetModel();
        rows_.clear();
        rows_.reserve(entries.size());
        for (const auto& entry : entries) rows_.append(Row(entry));
        endResetModel();
    }

    void append(const DebugWatchEntry& entry) {
        const int row = rows_.size();
        beginInsertRows({}, row, row);
        rows_.append(Row(entry));
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
        endRemoveRows();
    }

    void applySamples(const QVector<DebugWatchDomain>& domains,
                      const QVector<DebugWatchSample>& values, bool reset) {
        QHash<quint32, const DebugWatchSample*> samples;
        samples.reserve(values.size());
        for (const auto& value : values) samples.insert(value.id, &value);
        for (int i = 0; i < rows_.size(); ++i) {
            auto& row = rows_[i];
            const auto* domain = findDomain(domains, row.entry.instance, row.entry.domain);
            const auto* sample = samples.value(row.entry.id, nullptr);
            const bool valid = domain && sample && sample->bytes.size() == row.entry.width &&
                quint64(row.entry.offset) + row.entry.width <= domain->native.size;
            const auto name = domain ? domainName(*domain) : QString();
            const auto size = domain ? domain->native.size : 0;
            const bool changedDomain = row.domainName != name;
            if (changedDomain) row.domainName = name;
            const bool newBaseline = reset || changedDomain || row.domainSize != size || !valid ||
                (sample && row.bigEndian != sample->bigEndian);
            row.domainSize = size;
            if (changedDomain) emit dataChanged(index(i, Domain), index(i, Domain));
            const QByteArray next = valid ? sample->bytes : QByteArray{};
            const bool different = row.current != next;
            const bool hadHighlight = row.highlight;
            const bool hadHistory = row.changes != 0;
            row.highlight = !newBaseline && !row.current.isEmpty() && different;
            if (newBaseline) {
                row.previous.clear();
                row.changes = 0;
            } else if (row.highlight) {
                row.previous = row.current;
                if (row.changes < std::numeric_limits<quint64>::max()) ++row.changes;
            }
            const bool changedOrder = valid && row.bigEndian != sample->bigEndian;
            if (valid) row.bigEndian = sample->bigEndian;
            row.current = next;
            if (row.highlight || (newBaseline && hadHistory) || different || changedOrder)
                emit dataChanged(index(i, Value), index(i, Changes));
            else if (hadHighlight != row.highlight)
                emit dataChanged(index(i, Value), index(i, Value), {Qt::BackgroundRole, Qt::ForegroundRole});
        }
    }

    void clearHistory() {
        for (int i = 0; i < rows_.size(); ++i) {
            auto& row = rows_[i];
            if (!row.changes && !row.highlight) continue;
            row.previous.clear();
            row.changes = 0;
            row.highlight = false;
            emit dataChanged(index(i, Value), index(i, Changes));
        }
    }

private:
    struct Row {
        explicit Row(const DebugWatchEntry& item) : entry(item) {}
        DebugWatchEntry entry;
        QByteArray current;
        QByteArray previous;
        QString domainName;
        quint32 domainSize = 0;
        quint64 changes = 0;
        bool bigEndian = false;
        bool highlight = false;
    };
    QVector<Row> rows_;
};

DebugWatchDialog::DebugWatchDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint) {
    setObjectName(QStringLiteral("debugWatchDialog"));
    setWindowTitle(text("Наблюдение за памятью"));
    setWindowModality(Qt::NonModal);
    resize(900, 420);

    auto* outer = new QVBoxLayout(this);
    auto* actions = new QHBoxLayout;
    add_ = makeButton(this, "Добавить…", "debugWatchAdd");
    edit_ = makeButton(this, "Изменить…", "debugWatchEdit");
    remove_ = makeButton(this, "Удалить", "debugWatchRemove");
    memory_ = makeButton(this, "Открыть в памяти", "debugWatchMemory");
    clearHistory_ = makeButton(this, "Сбросить историю", "debugWatchClearHistory");
    clearHistory_->setToolTip(text("Очистить предыдущие значения и счётчики, сохранив список наблюдений."));
    for (auto* button : {add_, edit_, remove_, memory_, clearHistory_}) actions->addWidget(button);
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
    table_->horizontalHeader()->setSectionResizeMode(Domain, QHeaderView::Stretch);
    table_->setColumnWidth(Offset, 106);
    table_->setColumnWidth(Console, 102);
    table_->setColumnWidth(Value, 125);
    table_->setColumnWidth(Previous, 125);
    table_->setColumnWidth(Changes, 90);
    outer->addWidget(table_, 1);

    connect(add_, &QPushButton::clicked, this, [this] { editWatch(-1); });
    connect(edit_, &QPushButton::clicked, this, [this] { editWatch(table_->currentIndex().row()); });
    connect(remove_, &QPushButton::clicked, this, &DebugWatchDialog::removeWatch);
    connect(memory_, &QPushButton::clicked, this, [this] { navigate(table_->currentIndex().row()); });
    connect(clearHistory_, &QPushButton::clicked, this, [this] { model_->clearHistory(); });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) { editWatch(index.row()); });
    connect(table_, &QTableView::customContextMenuRequested, this, &DebugWatchDialog::showTableMenu);
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { updateActions(); });
    updateActions();
}

QVector<DebugWatchEntry> DebugWatchDialog::entries() const {
    return model_->entries();
}

quint32 DebugWatchDialog::unusedId() {
    const auto current = entries();
    while (!nextId_ || std::any_of(current.cbegin(), current.cend(), [this](const auto& entry) {
        return entry.id == nextId_;
    })) ++nextId_;
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
    model_->replaceAll(valid);
    model_->applySamples(domains_, {}, true);
    showFeedback({});
    updateActions();
    emit entriesChanged();
    emit inspectionChanged();
}

void DebugWatchDialog::addWatch(const DebugWatchEntry& entry) {
    if (model_->rowCount() >= DebugWatchLimit) {
        showFeedback(text("Достигнут предел в %1 наблюдений.").arg(DebugWatchLimit));
        return;
    }
    if (!validEntry(entry)) return;
    auto item = entry;
    item.id = unusedId();
    item.name = item.name.left(nameLimit);
    model_->append(item);
    table_->selectRow(model_->rowCount() - 1);
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
        model_->applySamples(domains_, snapshot.watchValues, !sameContext);
    } else {
        resetSamples();
    }
    const QString message = snapshot.available() ? text("Изменения между полученными снимками")
        : snapshot.status.isEmpty() ? text("Нет данных снимка") : snapshot.status;
    if (status_->text() != message) status_->setText(message);
    updateActions();
}

void DebugWatchDialog::editWatch(int row) {
    const auto* existing = model_->entry(row);
    const bool adding = !existing;
    if (adding && (domains_.isEmpty() || model_->rowCount() >= DebugWatchLimit)) return;
    const DebugWatchEntry original = existing ? *existing : DebugWatchEntry{};
    const auto catalog = domains_;
    const auto gameKey = gameKey_;
    const auto generation = generation_;
    QDialog editor(this, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    editor.setObjectName(QStringLiteral("debugWatchEditor"));
    editor.setWindowTitle(adding ? text("Добавить наблюдение") : text("Изменить наблюдение"));
    auto* layout = new QVBoxLayout(&editor);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(original.name, &editor);
    name->setObjectName(QStringLiteral("debugWatchEditorName"));
    name->setMaxLength(nameLimit);
    form->addRow(text("Имя"), name);
    auto* instance = new QComboBox(&editor);
    instance->setObjectName(QStringLiteral("debugWatchEditorConsole"));
    for (const quint32 value : {0u, 1u}) {
        if ((!adding && value == original.instance) ||
            std::any_of(catalog.cbegin(), catalog.cend(), [value](const auto& domain) { return domain.instance == value; }))
            instance->addItem(consoleName(value), value);
    }
    const int selectedConsole = instance->findData(original.instance);
    instance->setCurrentIndex(selectedConsole >= 0 ? selectedConsole : 0);
    form->addRow(text("Консоль"), instance);
    auto* domain = new QComboBox(&editor);
    domain->setObjectName(QStringLiteral("debugWatchEditorDomain"));
    const auto selectDomains = [&] {
        domain->clear();
        const auto console = instance->currentData().toUInt();
        for (const auto& item : catalog)
            if (item.instance == console) domain->addItem(domainName(item), item.native.id);
        if (!adding && console == original.instance && domain->findData(original.domain) < 0)
            domain->addItem(text("Область %1 (недоступна)").arg(original.domain), original.domain);
        int selection = domain->findData(original.domain);
        if (selection < 0) selection = domain->findData(quint32(KR_MEM_ZRAM));
        domain->setCurrentIndex(selection >= 0 ? selection : 0);
    };
    selectDomains();
    connect(instance, &QComboBox::currentIndexChanged, &editor, selectDomains);
    form->addRow(text("Область памяти"), domain);

    auto* offset = new QLineEdit(hex(original.offset), &editor);
    offset->setObjectName(QStringLiteral("debugWatchEditorOffset"));
    offset->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("(?:0[xX])?[0-9a-fA-F]{1,8}")), offset));
    form->addRow(text("Смещение (hex)"), offset);
    auto* width = new QComboBox(&editor);
    width->setObjectName(QStringLiteral("debugWatchEditorWidth"));
    for (const quint32 value : {1u, 2u, 4u})
        width->addItem(text("%1 бит").arg(value * 8), value);
    width->setCurrentIndex(width->findData(original.width));
    form->addRow(text("Размер"), width);
    auto* format = new QComboBox(&editor);
    format->setObjectName(QStringLiteral("debugWatchEditorFormat"));
    format->addItem(QStringLiteral("Hex"), int(DebugWatchEntry::Hex));
    format->addItem(text("Без знака"), int(DebugWatchEntry::Unsigned));
    format->addItem(text("Со знаком"), int(DebugWatchEntry::Signed));
    format->addItem(text("Двоичный"), int(DebugWatchEntry::Binary));
    format->setCurrentIndex(format->findData(int(original.format)));
    form->addRow(text("Формат"), format);
    auto* order = new QComboBox(&editor);
    order->setObjectName(QStringLiteral("debugWatchEditorOrder"));
    order->addItem(text("Порядок области"), int(DebugWatchEntry::Native));
    order->addItem(QStringLiteral("Big-endian"), int(DebugWatchEntry::Big));
    order->addItem(QStringLiteral("Little-endian"), int(DebugWatchEntry::Little));
    order->setCurrentIndex(order->findData(int(original.order)));
    form->addRow(text("Порядок байтов"), order);
    layout->addLayout(form);

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
        updated.offset = offset->text().toUInt(&ok, 16);
        updated.instance = instance->currentData().toUInt();
        updated.domain = domain->currentData().toUInt();
        updated.width = width->currentData().toUInt();
        updated.format = DebugWatchEntry::Format(format->currentData().toInt());
        updated.order = DebugWatchEntry::Order(order->currentData().toInt());
        updated.name = name->text().trimmed();
        const auto* region = findDomain(domains_, updated.instance, updated.domain);
        const bool available = region && quint64(updated.offset) + updated.width <= region->native.size;
        if (!ok || !validEntry(updated) || (!available && (adding || !sameLocation(original, updated)))) {
            error->setText(text("Значение выходит за границы доступной области памяти."));
            error->show();
            return;
        }
        editor.accept();
    });
    name->setFocus();
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
    if (!model_->entry(row)) return;
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
    auto* open = menu.addAction(text("Открыть в памяти"), this, [this, row = index.row()] { navigate(row); });
    open->setEnabled(canNavigate(index.row()));
    menu.addAction(text("Изменить…"), this, [this, row = index.row()] { editWatch(row); });
    menu.addAction(text("Удалить"), this, &DebugWatchDialog::removeWatch);
    menu.exec(table_->viewport()->mapToGlobal(point));
}

void DebugWatchDialog::updateActions() {
    const int row = table_->currentIndex().row();
    add_->setEnabled(!domains_.isEmpty() && model_->rowCount() < DebugWatchLimit);
    edit_->setEnabled(model_->entry(row) != nullptr);
    remove_->setEnabled(model_->entry(row) != nullptr);
    memory_->setEnabled(canNavigate(row));
    clearHistory_->setEnabled(model_->rowCount() != 0);
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
    showFeedback({});
    updateActions();
    emit inspectionChanged();
}
