#include "TraceDialog.h"
#include "i18n/Localization.h"

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHash>
#include <QHeaderView>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr int recentRows = 256;
constexpr std::array<quint32, 7> traceFilters{
    KR_TRACE_INSTRUCTION, KR_TRACE_VIDEO, KR_TRACE_DMA, KR_TRACE_IRQ,
    KR_TRACE_DISC, KR_TRACE_LINK, KR_TRACE_MAPPER
};

QString text(const char* source) {
    return Localization::text(QString::fromUtf8(source));
}

QString hex(quint64 number, int width = 0) {
    return QString::number(number, 16).toUpper().rightJustified(width, QLatin1Char('0'));
}

QString cpuName(const kr_debug_target& target, bool linked) {
    const QString name = Localization::text(QString::fromUtf8(target.name));
    if (!linked) return name;
    return QStringLiteral("%1 · %2").arg(target.cpu >> 8 ? text("Консоль B") : text("Консоль A"), name);
}

QString deviceName(quint32 device) {
    switch (device) {
    case KR_TRACE_VDP: return QStringLiteral("VDP");
    case KR_TRACE_CD: return QStringLiteral("Sega CD");
    case KR_TRACE_32X: return QStringLiteral("32X");
    case KR_TRACE_MEGALD: return QStringLiteral("MegaLD");
    case KR_TRACE_CABLE: return QStringLiteral("Link Cable");
    case KR_TRACE_CART: return text("Картридж");
    default: return QStringLiteral("—");
    }
}

QString eventName(quint32 kind) {
    switch (kind) {
    case KR_TRACE_EXEC: return text("Исполнение");
    case KR_TRACE_VDP_WRITE: return text("Запись VDP");
    case KR_TRACE_DMA_START: return text("Начало DMA");
    case KR_TRACE_DMA_END: return text("Конец DMA");
    case KR_TRACE_IRQ_ACCEPT: return text("Принятое прерывание");
    case KR_TRACE_DISC_COMMAND: return text("Команда диска");
    case KR_TRACE_LINK_EDGE: return text("Сигнал Link Cable");
    case KR_TRACE_BANK_CHANGE: return text("Смена банка");
    case KR_TRACE_DISCONTINUITY: return text("Разрыв трассы");
    default: return QString::number(kind);
    }
}

QString boundaryName(quint32 event) {
    switch (event) {
    case KR_TRACE_RESET: return text("Сброс");
    case KR_TRACE_POWER: return text("Перезапуск");
    case KR_TRACE_RESTORE: return text("Восстановление состояния");
    case KR_TRACE_REGISTER_EDIT: return text("Изменение регистра");
    case KR_TRACE_MEMORY_EDIT: return text("Изменение памяти");
    case KR_TRACE_TOPOLOGY: return text("Смена оборудования");
    case KR_TRACE_UNLOAD: return text("Закрытие игры");
    default: return QStringLiteral("—");
    }
}

quint32 encodedCpu(const DebugTraceRecord& row) {
    return (row.instance << 8) | row.native.cpu;
}

QString opcode(const DebugTraceRow& row) {
    const auto& native = row.record.native;
    const quint32 fetched = native.cpu == KR_DEBUG_Z80 ? 1 : 2;
    const quint32 decoded = row.instructionBytes ? row.instructionBytes : fetched;
    const quint32 length = std::min({quint32(native.valid_bytes), decoded, quint32(sizeof(native.bytes))});
    QString bytes;
    for (quint32 i = 0; i < length; ++i) {
        if (i) bytes += QLatin1Char(' ');
        bytes += hex(native.bytes[i], 2);
    }
    return bytes;
}

QPushButton* button(const QString& title, const char* name, QWidget* parent) {
    auto* result = new QPushButton(title, parent);
    result->setAutoDefault(false);
    result->setObjectName(QString::fromLatin1(name));
    return result;
}
} // namespace

class TraceTableModel final : public QAbstractTableModel {
public:
    explicit TraceTableModel(QObject* parent) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : rows_.size(); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 7; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (section) {
        case 0: return text("Кадр");
        case 1: return QStringLiteral("MCLK");
        case 2: return text("Процессор / устройство");
        case 3: return text("Событие");
        case 4: return text("Адрес");
        case 5: return text("Байты / значение");
        case 6: return text("Инструкция / детали");
        default: return {};
        }
    }

    QVariant data(const QModelIndex& index, int role) const override {
        const auto* row = at(index.row());
        if (!index.isValid() || !row) return {};
        const auto& record = row->record;
        const auto& event = record.native;
        if (role == Qt::ToolTipRole) {
            QString detail = text("Порядок %1 · эпоха %2 · локальные такты %3")
                .arg(record.sequence).arg(record.epoch).arg(event.local_ticks);
            if (event.flags & KR_TRACE_TIME_QUANTIZED) detail += QStringLiteral("\n") + text("Показано квантованное время.");
            if (event.flags & KR_TRACE_PEEK_TAIL) detail += QStringLiteral("\n") + text("Дополнительные байты взяты из снимка памяти.");
            if (event.flags & KR_TRACE_BYTES_TRUNCATED) detail += QStringLiteral("\n") + text("Часть байтов инструкции отсутствует.");
            return detail;
        }
        if (role != Qt::DisplayRole) return {};
        const bool instruction = event.kind == KR_TRACE_EXEC;
        const bool boundary = event.kind == KR_TRACE_DISCONTINUITY;
        const auto target = labels_.constFind(encodedCpu(record));
        switch (index.column()) {
        case 0: return QString::number(record.frame);
        case 1:
            return boundary ? QStringLiteral("—")
                : (event.flags & KR_TRACE_TIME_QUANTIZED ? QStringLiteral("~") : QString()) + QString::number(event.mclk);
        case 2:
            if (boundary) return QStringLiteral("—");
            if (target != labels_.cend()) {
                if (instruction || !event.device) return target->name;
                return QStringLiteral("%1 · %2").arg(target->name, deviceName(event.device));
            }
            {
                const QString device = deviceName(event.device);
                return linked_ ? QStringLiteral("%1 · %2")
                    .arg(record.instance ? text("Консоль B") : text("Консоль A"), device) : device;
            }
        case 3: return eventName(event.kind);
        case 4:
            if (boundary) return QStringLiteral("—");
            return hex(event.address, instruction && target != labels_.cend() ? target->digits : 0);
        case 5:
            if (boundary) return QStringLiteral("—");
            return instruction ? opcode(*row) : QStringLiteral("0x%1").arg(hex(event.value, std::min(event.width, quint8(4)) * 2));
        case 6:
            if (boundary) return boundaryName(event.value);
            if (instruction) return row->instruction;
            return event.aux ? QStringLiteral("aux 0x%1").arg(hex(event.aux)) : QString();
        default: return {};
        }
    }

    const DebugTraceRow* at(int row) const {
        return row >= 0 && row < rows_.size() ? &rows_[row] : nullptr;
    }

    int find(quint64 sequence) const {
        for (int row = 0; row < rows_.size(); ++row)
            if (rows_[row].record.sequence == sequence) return row;
        return -1;
    }

    bool setSnapshot(const DebugTraceSnapshot& snapshot) {
        const auto count = std::min(snapshot.rows.size(), qsizetype(recentRows));
        const auto first = count ? snapshot.rows[snapshot.rows.size() - count].record.sequence : 0;
        const auto last = count ? snapshot.rows.last().record.sequence : 0;
        const auto& targets = snapshot.recordTargets.isEmpty() ? snapshot.targets : snapshot.recordTargets;
        const bool linked = std::any_of(targets.cbegin(), targets.cend(),
                                        [](const kr_debug_target& target) { return target.cpu >> 8; })
            || std::any_of(snapshot.rows.cbegin(), snapshot.rows.cend(),
                           [](const DebugTraceRow& row) { return row.record.instance != 0; });
        QHash<quint32, Label> labels;
        for (const auto& target : targets)
            labels.insert(target.cpu, {cpuName(target, linked), (int(std::clamp(target.address_bits, 1u, 32u)) + 3) / 4});
        if (capture_ == snapshot.captureId && epoch_ == snapshot.epoch && rows_.size() == count
            && (!count || (first == rows_.first().record.sequence && last == rows_.last().record.sequence))
            && labels_ == labels && linked_ == linked) return false;

        beginResetModel();
        rows_ = snapshot.rows.size() <= recentRows ? snapshot.rows : snapshot.rows.mid(snapshot.rows.size() - recentRows);
        labels_ = std::move(labels);
        linked_ = linked;
        capture_ = snapshot.captureId;
        epoch_ = snapshot.epoch;
        endResetModel();
        return true;
    }

private:
    struct Label {
        QString name;
        int digits = 0;
        bool operator==(const Label& other) const { return name == other.name && digits == other.digits; }
    };
    QVector<DebugTraceRow> rows_;
    QHash<quint32, Label> labels_;
    quint64 capture_ = 0;
    quint32 epoch_ = 0;
    bool linked_ = false;
};

TraceDialog::TraceDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint) {
    setObjectName(QStringLiteral("traceDialog"));
    setWindowTitle(text("Трассировка"));
    setWindowModality(Qt::NonModal);
    resize(940, 505);

    auto* outer = new QVBoxLayout(this);
    auto* options = new QGridLayout;
    cpu_ = new QComboBox(this);
    cpu_->setObjectName(QStringLiteral("traceCpu"));
    cpu_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    cpu_->setAccessibleName(text("Процессор"));
    cpu_->setToolTip(text("Фильтр процессора действует на инструкции."));
    auto* cpuLabel = new QLabel(text("Процессор"), this);
    cpuLabel->setBuddy(cpu_);
    options->addWidget(cpuLabel, 0, 0);
    options->addWidget(cpu_, 0, 1);

    memory_ = new QSpinBox(this);
    memory_->setObjectName(QStringLiteral("traceMemoryMiB"));
    memory_->setRange(1, 64);
    memory_->setValue(DebugTraceConfig{}.memoryMiB);
    memory_->setSuffix(text(" МиБ"));
    auto* memoryLabel = new QLabel(text("Размер буфера"), this);
    memoryLabel->setBuddy(memory_);
    options->addWidget(memoryLabel, 0, 2);
    options->addWidget(memory_, 0, 3);
    options->setColumnStretch(4, 1);

    const std::array<QString, 7> labels{
        text("Инструкции"), QStringLiteral("VDP"), QStringLiteral("DMA"), QStringLiteral("IRQ"),
        text("Диск"), QStringLiteral("Link Cable"), text("Банки")
    };
    auto* filterRow = new QHBoxLayout;
    for (size_t i = 0; i < filters_.size(); ++i) {
        auto* filter = new QCheckBox(labels[i], this);
        filter->setObjectName(QStringLiteral("traceFilter%1").arg(traceFilters[i]));
        filters_[i] = filter;
        filterRow->addWidget(filter);
        connect(filter, &QCheckBox::toggled, this, [this, i](bool enabled) {
            if (enabled) filterMask_ |= traceFilters[i];
            else filterMask_ &= ~traceFilters[i];
            updateControls();
        });
    }
    filterRow->addStretch();
    options->addLayout(filterRow, 1, 0, 1, 5);

    captureFile_ = new QCheckBox(text("Записывать в файл"), this);
    captureFile_->setObjectName(QStringLiteral("traceCaptureFile"));
    file_ = new QLineEdit(this);
    file_->setObjectName(QStringLiteral("traceFilePath"));
    file_->setReadOnly(true);
    file_->setPlaceholderText(text("Путь к новому файлу TSV"));
    file_->setAccessibleName(file_->placeholderText());
    filePick_ = button(text("Выбрать файл…"), "traceFilePick", this);
    options->addWidget(captureFile_, 2, 0);
    options->addWidget(file_, 2, 1, 1, 3);
    options->addWidget(filePick_, 2, 4, Qt::AlignLeft);
    outer->addLayout(options);
    connect(captureFile_, &QCheckBox::toggled, this, [this] { updateControls(); });
    connect(filePick_, &QPushButton::clicked, this, [this] {
        const QString path = chooseNewFile(text("Файл трассы"), file_->text());
        if (!path.isEmpty()) {
            file_->setText(path);
            file_->setToolTip(path);
            showFeedback({});
            updateControls();
        }
    });

    auto* actions = new QHBoxLayout;
    start_ = button(text("Начать"), "traceStart", this);
    stop_ = button(text("Остановить"), "traceStop", this);
    clear_ = button(text("Очистить буфер"), "traceClear", this);
    export_ = button(text("Экспорт буфера…"), "traceExport", this);
    for (auto* action : {start_, stop_, clear_, export_}) actions->addWidget(action);
    actions->addStretch();
    outer->addLayout(actions);
    connect(start_, &QPushButton::clicked, this, &TraceDialog::startCapture);
    connect(stop_, &QPushButton::clicked, this, [this] { send(DebugTraceCommand::Stop); });
    connect(clear_, &QPushButton::clicked, this, [this] { send(DebugTraceCommand::Clear); });
    connect(export_, &QPushButton::clicked, this, [this] {
        const QString path = chooseNewFile(text("Экспорт буфера трассы"));
        if (path.isEmpty()) return;
        DebugTraceCommand command;
        command.kind = DebugTraceCommand::Export;
        command.path = path;
        commandPending_ = true;
        showFeedback({});
        updateControls();
        emit commandRequested(command);
    });

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("traceStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    outer->addWidget(status_);
    feedback_ = new QLabel(this);
    feedback_->setObjectName(QStringLiteral("traceFeedback"));
    feedback_->setTextFormat(Qt::PlainText);
    feedback_->setWordWrap(true);
    feedback_->hide();
    outer->addWidget(feedback_);

    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("traceEvents"));
    table_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(table_->fontMetrics().height() + 7);
    model_ = new TraceTableModel(this);
    table_->setModel(model_);
    table_->horizontalHeader()->setStretchLastSection(true);
    const std::array<int, 6> widths{65, 92, 205, 145, 80, 130};
    for (int col = 0; col < int(widths.size()); ++col) table_->setColumnWidth(col, widths[col]);
    outer->addWidget(table_, 1);
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto* row = model_->at(index.row());
        if (snapshot_.canNavigate && row && row->record.native.kind == KR_TRACE_EXEC)
            emit navigateRequested(encodedCpu(row->record), row->record.native.address);
    });
    updateControls();
}

void TraceDialog::updateControls() {
    const bool settings = snapshot_.available && !snapshot_.active && !snapshot_.writing && !commandPending_;
    const auto filters = filterMask_ & snapshot_.filters;
    for (size_t i = 0; i < filters_.size(); ++i) {
        const QSignalBlocker block(filters_[i]);
        const bool supported = snapshot_.filters & traceFilters[i];
        filters_[i]->setVisible(supported);
        filters_[i]->setChecked(filters & traceFilters[i]);
        filters_[i]->setEnabled(settings && supported);
    }
    cpu_->setEnabled(settings && (filters & KR_TRACE_INSTRUCTION) && cpu_->count() > 1);
    memory_->setEnabled(settings);
    captureFile_->setEnabled(settings);
    const bool toFile = captureFile_->isChecked();
    file_->setEnabled(settings && toFile);
    filePick_->setEnabled(settings && toFile);
    start_->setEnabled(settings && filters && (!toFile || !file_->text().isEmpty()));
    stop_->setEnabled(snapshot_.active && !commandPending_);
    clear_->setEnabled(hasRecords() && !commandPending_);
    export_->setEnabled(hasRecords() && !snapshot_.writing && !commandPending_);

    QString activity = snapshot_.active ? text("Трассировка идёт") :
        snapshot_.writing ? text("Завершение записи файла") :
        snapshot_.available ? text("Трассировка остановлена") : text("Трассировка недоступна");
    if (snapshot_.active && snapshot_.writing) activity += QStringLiteral(" · ") + text("Запись файла");
    if (snapshot_.capacity) {
        activity += QStringLiteral(" · ") + text("В буфере %1 из %2 событий")
            .arg(std::min(snapshot_.total, quint64(snapshot_.capacity))).arg(snapshot_.total);
    }
    if (snapshot_.overwritten)
        activity += QStringLiteral(" · ") + text("Вытеснено из буфера %1").arg(snapshot_.overwritten);
    status_->setText(activity);
    status_->setToolTip(snapshot_.filePath);
}

void TraceDialog::setSnapshot(const DebugTraceSnapshot& snapshot) {
    if (!isVisible()) return;
    const bool sameCapture = snapshot.captureId == snapshot_.captureId;
    const auto* selected = model_->at(table_->currentIndex().row());
    const auto* top = model_->at(table_->rowAt(0));
    const quint64 selectedSequence = selected ? selected->record.sequence : 0;
    const quint64 topSequence = top ? top->record.sequence : 0;
    const bool atLatest = table_->verticalScrollBar()->value() == table_->verticalScrollBar()->maximum();
    if (model_->setSnapshot(snapshot)) {
        const int selectedRow = sameCapture && selectedSequence ? model_->find(selectedSequence) : -1;
        if (selectedRow >= 0) table_->setCurrentIndex(model_->index(selectedRow, 0));
        const int visibleRow = sameCapture && topSequence ? model_->find(topSequence) : -1;
        if (atLatest || !sameCapture) table_->scrollToBottom();
        else if (visibleRow >= 0) table_->scrollTo(model_->index(visibleRow, 0), QAbstractItemView::PositionAtTop);
    }

    const auto previousCpu = cpu_->currentData().toUInt();
    const bool linked = std::any_of(snapshot.targets.cbegin(), snapshot.targets.cend(),
                                    [](const kr_debug_target& target) { return target.cpu >> 8; });
    bool targetsChanged = cpu_->count() != snapshot.targets.size() + 1;
    for (int i = 0; !targetsChanged && i < snapshot.targets.size(); ++i) {
        const auto& cpu = snapshot.targets[i];
        targetsChanged = cpu_->itemData(i + 1).toUInt() != cpu.cpu || cpu_->itemText(i + 1) != cpuName(cpu, linked);
    }
    if (targetsChanged) {
        const QSignalBlocker block(cpu_);
        cpu_->clear();
        cpu_->addItem(text("Все процессоры"), 0u);
        for (const auto& target : snapshot.targets)
            cpu_->addItem(cpuName(target, linked), target.cpu);
        const int selectedIndex = cpu_->findData(previousCpu);
        cpu_->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    }
    const bool availabilityRestored = !snapshot_.available && snapshot.available;
    snapshot_ = snapshot;
    snapshot_.rows.clear(); // The presentation model retains just its visible window.
    commandPending_ = false;
    if (!snapshot.status.isEmpty() || availabilityRestored) showFeedback(snapshot.status);
    updateControls();
}

void TraceDialog::startCapture() {
    DebugTraceCommand command;
    command.kind = DebugTraceCommand::Start;
    command.config.cpu = cpu_->currentData().toUInt();
    command.config.filters = filterMask_ & snapshot_.filters;
    command.config.memoryMiB = memory_->value();
    if (captureFile_->isChecked()) {
        command.config.filePath = file_->text();
        if (QFileInfo::exists(command.config.filePath)) {
            showFeedback(text("Файл уже существует. Выберите новое имя."));
            return;
        }
    }
    commandPending_ = true;
    showFeedback({});
    updateControls();
    emit commandRequested(command);
}

void TraceDialog::send(DebugTraceCommand::Kind kind) {
    DebugTraceCommand command;
    command.kind = kind;
    if (kind != DebugTraceCommand::Refresh && isVisible()) {
        commandPending_ = true;
        showFeedback({});
        updateControls();
    }
    emit commandRequested(command);
}

QString TraceDialog::chooseNewFile(const QString& title, const QString& initial) {
    QString path = QFileDialog::getSaveFileName(this, title, initial, text("Трасса TSV (*.tsv);;Все файлы (*)"),
        nullptr, QFileDialog::DontConfirmOverwrite);
    if (path.isEmpty()) return {};
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".tsv");
    if (QFileInfo::exists(path)) {
        showFeedback(text("Файл уже существует. Выберите новое имя."));
        return {};
    }
    return path;
}

void TraceDialog::showFeedback(const QString& message) {
    feedback_->setText(message);
    feedback_->setVisible(!message.isEmpty());
}

void TraceDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    send(DebugTraceCommand::Refresh);
}

void TraceDialog::hideEvent(QHideEvent* event) {
    QDialog::hideEvent(event);
    commandPending_ = false;
    snapshot_.active = false;
    snapshot_.canNavigate = false;
    snapshot_.rows.clear();
    model_->setSnapshot({});
    updateControls();
    send(DebugTraceCommand::Stop);
}
