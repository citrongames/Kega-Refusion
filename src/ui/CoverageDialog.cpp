#include "CoverageDialog.h"
#include "i18n/Localization.h"

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

namespace {
constexpr quint32 rowBytes = 16;
constexpr quint32 pageBytes = 256;

QString text(const char* source) {
    return Localization::text(QString::fromUtf8(source));
}

QString hex(quint32 value, int digits = 0) {
    return QString::number(value, 16).toUpper().rightJustified(digits, QLatin1Char('0'));
}

QString console(quint32 instance) {
    return instance ? text("Консоль B") : text("Консоль A");
}

QString sourceName(const DebugCoverageSource& source) {
    const QString name = Localization::text(QString::fromUtf8(source.native.name));
    return source.native.flags & KR_COV_SOURCE_PARTIAL
        ? text("%1 (частично)").arg(name) : name;
}

template<class T>
T* named(T* widget, const char* name) {
    widget->setObjectName(QString::fromLatin1(name));
    return widget;
}

QPushButton* button(const char* label, const char* name, QWidget* parent) {
    auto* widget = named(new QPushButton(text(label), parent), name);
    widget->setAutoDefault(false);
    return widget;
}

QLabel* label(QWidget* parent, const char* name) {
    auto* widget = named(new QLabel(parent), name);
    widget->setWordWrap(true);
    widget->setTextFormat(Qt::PlainText);
    return widget;
}

void setChoices(QComboBox* box, const QVector<QPair<quint32, QString>>& choices, quint32 selected) {
    bool changed = box->count() != choices.size();
    for (int i = 0; !changed && i < box->count(); ++i) {
        const auto& choice = choices[i];
        changed = box->itemData(i).toUInt() != choice.first || box->itemText(i) != choice.second;
    }
    const QSignalBlocker block(box);
    if (changed) {
        box->clear();
        for (const auto& choice : choices) box->addItem(choice.second, choice.first);
    }
    const int index = box->findData(selected);
    if (box->currentIndex() != index) box->setCurrentIndex(index);
}

unsigned byteFlags(char byte) {
    return static_cast<unsigned char>(byte) & 3u;
}

QString coverageName(unsigned flags) {
    switch (flags) {
    case KR_COV_EXEC: return text("Код");
    case KR_COV_READ: return text("Чтение данных");
    case KR_COV_EXEC | KR_COV_READ: return text("Код и данные");
    default: return text("Не наблюдалось");
    }
}
}

class CoveragePageModel final : public QAbstractTableModel {
public:
    explicit CoveragePageModel(QObject* parent) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : (bytes_.size() + rowBytes - 1) / rowBytes;
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 1 + rowBytes;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        return section == 0 ? text("Смещение") : hex(section - 1, 1);
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) return {};
        if (index.column() == 0)
            return role == Qt::DisplayRole ? QVariant(hex(offset_ + index.row() * rowBytes, 8)) : QVariant{};
        const int relative = byteIndex(index);
        if (relative < 0) return {};
        const unsigned flags = byteFlags(bytes_[relative]);
        switch (role) {
        case Qt::DisplayRole: {
            static const std::array<QString, 4> marks{
                QStringLiteral("U"), QStringLiteral("C"), QStringLiteral("R"), QStringLiteral("B")
            };
            return marks[flags];
        }
        case Qt::BackgroundRole: {
            static const std::array<QColor, 4> colors{
                QColor(), QColor(179, 220, 255), QColor(188, 231, 187), QColor(236, 201, 249)
            };
            return flags ? QVariant(colors[flags]) : QVariant{};
        }
        case Qt::ForegroundRole: return flags ? QVariant(QColor(24, 28, 37)) : QVariant{};
        case Qt::TextAlignmentRole: return int(Qt::AlignCenter);
        case Qt::ToolTipRole:
            return QStringLiteral("0x%1 · %2").arg(hex(offset_ + relative), coverageName(flags));
        default: return {};
        }
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        if (!index.isValid()) return Qt::NoItemFlags;
        return byteIndex(index) >= 0 ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::ItemIsEnabled;
    }

    int byteIndex(const QModelIndex& index) const {
        if (!index.isValid() || index.column() < 1) return -1;
        const int relative = index.row() * rowBytes + index.column() - 1;
        return relative < bytes_.size() ? relative : -1;
    }

    QModelIndex byteCell(quint32 relative) const {
        return relative < quint32(bytes_.size()) ? index(relative / rowBytes, 1 + relative % rowBytes) : QModelIndex{};
    }

    void setPage(quint32 offset, const QByteArray& bytes) {
        if (offset == offset_ && bytes == bytes_) return;
        if (offset != offset_ || bytes.size() != bytes_.size()) {
            beginResetModel();
            offset_ = offset;
            bytes_ = bytes;
            endResetModel();
            return;
        }
        int first = 0;
        while (bytes[first] == bytes_[first]) ++first;
        int last = bytes.size() - 1;
        while (bytes[last] == bytes_[last]) --last;
        bytes_ = bytes;
        const int firstRow = first / rowBytes;
        const int lastRow = last / rowBytes;
        if (firstRow == lastRow)
            emit dataChanged(byteCell(first), byteCell(last));
        else
            emit dataChanged(index(firstRow, 1), index(lastRow, rowBytes));
    }

private:
    QByteArray bytes_;
    quint32 offset_ = 0;
};

CoverageDialog::CoverageDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint) {
    setObjectName(QStringLiteral("coverageDialog"));
    setWindowTitle(text("Покрытие кода и данных"));
    setWindowModality(Qt::NonModal);
    resize(850, 535);

    auto* outer = new QVBoxLayout(this);
    auto* config = new QHBoxLayout;
    instance_ = named(new QComboBox(this), "coverageConsole");
    instance_->setAccessibleName(text("Консоль"));
    instance_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    config->addWidget(instance_);
    cpu_ = named(new QComboBox(this), "coverageCpu");
    cpu_->setAccessibleName(text("Процессор"));
    cpu_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    config->addWidget(cpu_);
    execution_ = named(new QCheckBox(text("Код"), this), "coverageExecution");
    reads_ = named(new QCheckBox(text("Чтение данных"), this), "coverageReads");
    config->addWidget(execution_);
    config->addWidget(reads_);
    config->addStretch();
    outer->addLayout(config);

    auto* actions = new QHBoxLayout;
    start_ = button("Начать", "coverageStart", this);
    stop_ = button("Остановить", "coverageStop", this);
    clear_ = button("Очистить покрытие", "coverageClear", this);
    export_ = button("Экспорт покрытия…", "coverageExport", this);
    for (auto* action : {start_, stop_, clear_, export_}) actions->addWidget(action);
    actions->addStretch();
    outer->addLayout(actions);

    status_ = label(this, "coverageStatus");
    summary_ = label(this, "coverageSummary");
    summary_->setToolTip(text("Счётчики кода и данных включают байты, попавшие в обе категории."));
    feedback_ = label(this, "coverageFeedback");
    feedback_->hide();
    outer->addWidget(status_);
    outer->addWidget(summary_);
    outer->addWidget(feedback_);

    auto* navigation = new QHBoxLayout;
    source_ = named(new QComboBox(this), "coverageSource");
    source_->setAccessibleName(text("Физический источник"));
    source_->setPlaceholderText(text("Физический источник"));
    source_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    navigation->addWidget(source_, 1);
    previous_ = button("Назад", "coveragePrevious", this);
    next_ = button("Далее", "coverageNext", this);
    offset_ = named(new QLineEdit(this), "coverageOffset");
    offset_->setAccessibleName(text("Смещение в физическом источнике"));
    offset_->setPlaceholderText(QStringLiteral("0x00000000"));
    offset_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    offset_->setFixedWidth(offset_->fontMetrics().horizontalAdvance(QStringLiteral("0x00000000")) + 22);
    offset_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("(?i)(?:0x)?[0-9a-f]{1,8}")), offset_));
    openMemory_ = button("Открыть в памяти", "coverageOpenMemory", this);
    for (QWidget* control : std::array<QWidget*, 4>{previous_, offset_, next_, openMemory_})
        navigation->addWidget(control);
    outer->addLayout(navigation);

    table_ = named(new QTableView(this), "coveragePage");
    table_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->hide();
    table_->setAlternatingRowColors(true);
    table_->installEventFilter(this);
    model_ = new CoveragePageModel(table_);
    table_->setModel(model_);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    updateColumns();
    outer->addWidget(table_, 1);

    auto* legend = label(this, "coverageLegend");
    legend->setText(text("C — код · R — чтение данных · B — код и данные · U — не наблюдалось"));
    outer->addWidget(legend);

    connect(instance_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) return;
        instanceId_ = instance_->itemData(index).toUInt();
        cpuId_ = 0;
        updateControls();
        send(DebugCoverageCommand::Refresh);
    });
    connect(cpu_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) return;
        cpuId_ = cpu_->itemData(index).toUInt();
        updateControls();
    });
    connect(execution_, &QCheckBox::toggled, this, [this](bool checked) {
        wantExecution_ = checked;
        updateControls();
    });
    connect(reads_, &QCheckBox::toggled, this, [this](bool checked) {
        wantReads_ = checked;
        updateControls();
    });
    connect(start_, &QPushButton::clicked, this, [this] { send(DebugCoverageCommand::Start); });
    connect(stop_, &QPushButton::clicked, this, [this] { send(DebugCoverageCommand::Stop); });
    connect(clear_, &QPushButton::clicked, this, [this] { send(DebugCoverageCommand::Clear); });
    connect(export_, &QPushButton::clicked, this, &CoverageDialog::exportCapture);
    connect(source_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) return;
        sourceId_ = source_->itemData(index).toUInt();
        pageOffset_ = selectedOffset_ = 0;
        updatePage();
        updateControls();
        send(DebugCoverageCommand::Refresh);
    });
    connect(previous_, &QPushButton::clicked, this, [this] { changeOffset(pageOffset_ - pageBytes); });
    connect(next_, &QPushButton::clicked, this, [this] { changeOffset(pageOffset_ + pageBytes); });
    connect(offset_, &QLineEdit::returnPressed, this, [this] {
        bool ok = false;
        const quint32 value = offset_->text().toUInt(&ok, 16);
        if (ok) {
            offset_->setModified(false);
            changeOffset(value);
        }
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& index) {
        const int byte = model_->byteIndex(index);
        if (byte < 0) return;
        selectedOffset_ = pageOffset_ + byte;
        updateControls();
    });
    connect(openMemory_, &QPushButton::clicked, this, &CoverageDialog::navigate);
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const int byte = model_->byteIndex(index);
        if (byte >= 0) {
            selectedOffset_ = pageOffset_ + byte;
            navigate();
        }
    });
    updateControls();
}

const DebugCoverageSource* CoverageDialog::selectedSource() const {
    const auto found = std::find_if(snapshot_.sources.cbegin(), snapshot_.sources.cend(),
                                    [this](const auto& source) { return source.native.id == sourceId_; });
    return found == snapshot_.sources.cend() ? nullptr : &*found;
}

DebugCoverageConfig CoverageDialog::configuration() const {
    DebugCoverageConfig config;
    config.instance = instanceId_;
    config.cpu = cpuId_;
    const quint32 mask = cpuId_ ? cpuId_ : ~quint32(0);
    const bool matched = snapshot_.available && snapshot_.instance == instanceId_;
    config.execution = wantExecution_ && matched && (snapshot_.execCpus & mask);
    config.reads = wantReads_ && matched && (snapshot_.readCpus & mask);
    return config;
}

DebugCoverageCommand CoverageDialog::refreshCommand() const {
    DebugCoverageCommand command;
    command.instance = instanceId_;
    command.source = sourceId_;
    command.offset = pageOffset_;
    command.captureId = snapshot_.captureId;
    return command;
}

void CoverageDialog::setSnapshot(const DebugCoverageSnapshot& snapshot) {
    if (!isVisible()) return;
    const bool captureChanged = snapshot.captureId != snapshot_.captureId;
    const bool availabilityRestored = !snapshot_.available && snapshot.available;
    snapshot_ = snapshot;
    commandPending_ = false;

    QVector<QPair<quint32, QString>> consoles;
    for (const auto instance : snapshot.instances) consoles.append({instance, console(instance)});
    const bool instanceChanged = !snapshot.instances.isEmpty() && !snapshot.instances.contains(instanceId_);
    if (instanceChanged) {
        instanceId_ = snapshot.instances.first();
        cpuId_ = 0;
    }
    setChoices(instance_, consoles, instanceId_);

    QVector<QPair<quint32, QString>> cpus{{0u, text("Все процессоры")}};
    if (snapshot.instance == instanceId_) {
        for (const auto& target : snapshot.targets)
            cpus.append({target.cpu, Localization::text(QString::fromUtf8(target.name))});
        if (std::none_of(cpus.cbegin(), cpus.cend(), [this](const auto& cpu) { return cpu.first == cpuId_; }))
            cpuId_ = 0;
    }
    setChoices(cpu_, cpus, cpuId_);

    QVector<QPair<quint32, QString>> sources;
    for (const auto& source : snapshot.sources)
        sources.append({source.native.id, sourceName(source)});
    bool requestPage = instanceChanged && snapshot.instance != instanceId_;
    if (snapshot.hasCapture && !sources.isEmpty()) {
        if (captureChanged || !selectedSource()) {
            sourceId_ = snapshot.source ? snapshot.source : sources.first().first;
            pageOffset_ = selectedOffset_ = snapshot.offset;
            requestPage |= snapshot.source != sourceId_;
        } else if (selectedSource()->native.size) {
            const quint32 last = selectedSource()->native.size - 1;
            const quint32 lastPage = last - last % pageBytes;
            if (pageOffset_ > lastPage) {
                pageOffset_ = lastPage;
                selectedOffset_ = std::min(selectedOffset_, last);
                requestPage |= snapshot.offset != pageOffset_;
            }
        }
    }
    setChoices(source_, sources, sourceId_);
    if (!snapshot.status.isEmpty() || availabilityRestored) showFeedback(snapshot.status);
    updatePage();
    updateControls();
    if (requestPage) send(DebugCoverageCommand::Refresh);
}

void CoverageDialog::changeOffset(quint32 address) {
    const auto* source = selectedSource();
    if (!source || !source->native.size) return;
    selectedOffset_ = std::min(address, source->native.size - 1);
    const auto previousPage = pageOffset_;
    pageOffset_ = selectedOffset_ - selectedOffset_ % pageBytes;
    updatePage();
    updateControls();
    if (previousPage != pageOffset_) send(DebugCoverageCommand::Refresh);
}

void CoverageDialog::updatePage() {
    const bool matches = selectedSource() && snapshot_.source == sourceId_ && snapshot_.offset == pageOffset_;
    const QSignalBlocker block(table_->selectionModel());
    model_->setPage(pageOffset_, matches ? snapshot_.page : QByteArray{});
    const QModelIndex expected = model_->byteCell(selectedOffset_ - pageOffset_);
    if (table_->currentIndex() != expected) {
        table_->setCurrentIndex(expected);
        if (expected.isValid()) table_->scrollTo(expected);
    }
}

bool CoverageDialog::canNavigate() const {
    if (!snapshot_.canNavigate || !snapshot_.inspectDomain || snapshot_.source != sourceId_ ||
        snapshot_.offset != pageOffset_) return false;
    const quint32 relative = selectedOffset_ - pageOffset_;
    return relative < quint32(snapshot_.page.size()) && snapshot_.inspectOffset <= UINT32_MAX - relative;
}

void CoverageDialog::navigate() {
    if (canNavigate())
        emit navigateRequested(snapshot_.capturedInstance, snapshot_.inspectDomain,
                               snapshot_.inspectOffset + (selectedOffset_ - pageOffset_));
}

void CoverageDialog::updateControls() {
    const bool settings = !snapshot_.active && !snapshot_.writing && !commandPending_;
    const bool captureSettings = settings && snapshot_.available && snapshot_.instance == instanceId_;
    const quint32 mask = cpuId_ ? cpuId_ : ~quint32(0);
    instance_->setEnabled(settings && instance_->count() > 1);
    cpu_->setEnabled(captureSettings && cpu_->count() > 1);
    const QSignalBlocker executionBlock(execution_);
    const QSignalBlocker readsBlock(reads_);
    const auto config = configuration();
    execution_->setChecked(config.execution);
    reads_->setChecked(config.reads);
    execution_->setEnabled(captureSettings && (snapshot_.execCpus & mask));
    reads_->setEnabled(captureSettings && (snapshot_.readCpus & mask));
    start_->setEnabled(captureSettings && (config.execution || config.reads));
    start_->setText(snapshot_.hasCapture ? text("Начать заново") : text("Начать"));
    stop_->setEnabled(snapshot_.active && !commandPending_);
    clear_->setEnabled(snapshot_.hasCapture && !commandPending_);
    export_->setEnabled(snapshot_.hasCapture && !snapshot_.writing && !commandPending_);

    const auto* source = selectedSource();
    source_->setEnabled(source != nullptr);
    offset_->setEnabled(source != nullptr);
    previous_->setEnabled(source && pageOffset_ >= pageBytes);
    next_->setEnabled(source && quint64(pageOffset_) + pageBytes < source->native.size);
    openMemory_->setEnabled(canNavigate());
    if (!offset_->hasFocus() || !offset_->isModified()) {
        const QString address = source ? QStringLiteral("0x%1").arg(hex(selectedOffset_, 8)) : QString();
        if (offset_->text() != address) offset_->setText(address);
    }

    QString activity = snapshot_.active ? text("Сбор покрытия идёт")
        : snapshot_.hasCapture ? text("Сбор покрытия остановлен")
        : snapshot_.available ? text("Сбор покрытия готов к запуску")
        : text("Покрытие недоступно");
    if (snapshot_.hasCapture) {
        activity += QStringLiteral(" · %1 · %2").arg(snapshot_.game, console(snapshot_.capturedInstance));
        activity += QStringLiteral(" · ") + text("Карта %1 МиБ")
            .arg(QLocale().toString(double(snapshot_.allocatedBytes) / (1024 * 1024), 'f', 2));
    }
    if (snapshot_.writing) activity += QStringLiteral(" · ") + text("Экспорт файла");
    if (status_->text() != activity) status_->setText(activity);

    const QLocale locale;
    QString totals;
    if (source) {
        const quint64 known = source->code + source->data - std::min({source->code, source->data, source->both});
        const quint64 unseen = source->native.size - std::min(quint64(source->native.size), known);
        totals = text("Байты: код %1 · данные %2 · оба %3 · не наблюдалось %4")
            .arg(locale.toString(source->code), locale.toString(source->data),
                 locale.toString(source->both), locale.toString(unseen));
    }
    if (snapshot_.unknownCode || snapshot_.unknownData) {
        if (!totals.isEmpty()) totals += QLatin1Char('\n');
        totals += text("Неизвестный источник: код %1 байт · чтение %2 байт")
            .arg(locale.toString(snapshot_.unknownCode), locale.toString(snapshot_.unknownData));
    }
    if (summary_->text() != totals) summary_->setText(totals);
    summary_->setVisible(!totals.isEmpty());
}

void CoverageDialog::send(DebugCoverageCommand::Kind kind, const QString& path) {
    auto command = refreshCommand();
    command.kind = kind;
    command.path = path;
    if (kind == DebugCoverageCommand::Start) command.config = configuration();
    if (kind != DebugCoverageCommand::Refresh && isVisible()) {
        commandPending_ = true;
        showFeedback({});
        updateControls();
    }
    emit commandRequested(command);
}

void CoverageDialog::exportCapture() {
    QString path = QFileDialog::getSaveFileName(this, text("Экспорт покрытия"),
        {}, text("Покрытие Kega (*.kcdl);;Все файлы (*)"), nullptr, QFileDialog::DontConfirmOverwrite);
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".kcdl");
    if (QFileInfo::exists(path)) {
        showFeedback(text("Файл уже существует. Выберите новое имя."));
        return;
    }
    send(DebugCoverageCommand::Export, path);
}

void CoverageDialog::showFeedback(const QString& message) {
    feedback_->setText(message);
    feedback_->setVisible(!message.isEmpty());
}

void CoverageDialog::updateColumns() {
    const QFontMetrics metrics(table_->font());
    const int offsetWidth = std::max(metrics.horizontalAdvance(text("Смещение")),
                                     metrics.horizontalAdvance(QStringLiteral("00000000")));
    table_->setColumnWidth(0, offsetWidth + 20);
    table_->horizontalHeader()->setMinimumSectionSize(metrics.horizontalAdvance(QStringLiteral("00")) + 14);
    table_->verticalHeader()->setDefaultSectionSize(metrics.height() + 5);
}

bool CoverageDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_ && model_ && (event->type() == QEvent::FontChange ||
                                      event->type() == QEvent::StyleChange))
        updateColumns();
    return QDialog::eventFilter(watched, event);
}

void CoverageDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    send(DebugCoverageCommand::Refresh);
}

void CoverageDialog::hideEvent(QHideEvent* event) {
    QDialog::hideEvent(event);
    commandPending_ = false;
    snapshot_.active = false;
    snapshot_.canNavigate = false;
    snapshot_.page.clear();
    updatePage();
    updateControls();
    send(DebugCoverageCommand::Stop);
}
