#include "CpuDebuggerDialog.h"
#include "i18n/Localization.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringDecoder>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr qsizetype maxBreakpoints = 128;
constexpr qsizetype maxSymbols = 4096;
constexpr qsizetype maxSymbolFile = 512 * 1024;
constexpr std::array<quint32, 3> eventBits{KR_DEBUG_EXEC, KR_DEBUG_READ, KR_DEBUG_WRITE};
constexpr std::array<DebuggerCommand::Kind, 5> executionActions{
    DebuggerCommand::Pause, DebuggerCommand::Continue, DebuggerCommand::StepInto,
    DebuggerCommand::StepOver, DebuggerCommand::StepOut
};

QString text(const char* source) {
    return Localization::text(QString::fromUtf8(source));
}

quint32 bitMask(quint32 bits) {
    return bits >= 32 ? ~quint32(0) : (quint32(1) << bits) - 1;
}

const kr_debug_target* findTarget(const DebuggerSnapshot& snapshot, quint32 cpu) {
    for (const auto& target : snapshot.targets)
        if (target.cpu == cpu) return &target;
    return nullptr;
}

QString hex(quint64 value, int digits = 0) {
    return QString::number(value, 16).toUpper().rightJustified(digits, QLatin1Char('0'));
}

QString eventNames(quint32 events) {
    QStringList names;
    if (events & KR_DEBUG_EXEC) names << text("Исполнение");
    if (events & KR_DEBUG_READ) names << text("Чтение");
    if (events & KR_DEBUG_WRITE) names << text("Запись данных");
    return names.join(QStringLiteral(" / "));
}

bool readHex(QString input, quint32 mask, quint32& address) {
    input = input.trimmed();
    if (input.startsWith(QLatin1Char('$'))) input.remove(0, 1);
    else if (input.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) input.remove(0, 2);
    static const QRegularExpression digits(QStringLiteral("^[0-9a-fA-F]{1,8}$"));
    if (!digits.match(input).hasMatch()) return false;
    bool ok = false;
    const auto parsed = input.toUInt(&ok, 16);
    if (!ok || parsed > mask) return false;
    address = parsed;
    return true;
}

template<typename T>
T* named(T* widget, const char* name) {
    widget->setObjectName(QString::fromLatin1(name));
    return widget;
}

QPushButton* button(const QString& title, QWidget* parent, const char* name) {
    auto* widget = named(new QPushButton(title, parent), name);
    widget->setAutoDefault(false);
    return widget;
}

void setupTable(QTableWidget* table, const QStringList& columns) {
    table->setColumnCount(columns.size());
    table->setHorizontalHeaderLabels(columns);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 6);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
}

QTableWidgetItem* cell(QTableWidget* table, int row, int column, const QString& value) {
    auto* item = table->item(row, column);
    if (!item) {
        item = new QTableWidgetItem;
        table->setItem(row, column, item);
    }
    if (item->text() != value) item->setText(value);
    return item;
}

const kr_debug_cpu* findCpu(const DebuggerSnapshot& snapshot, quint32 id) {
    for (const auto& cpu : snapshot.cpus)
        if (cpu.cpu == id) return &cpu;
    return nullptr;
}

const DebuggerBreakpoint* findBreakpoint(const QVector<DebuggerBreakpoint>& breakpoints, quint32 id) {
    for (const auto& breakpoint : breakpoints)
        if (breakpoint.id == id) return &breakpoint;
    return nullptr;
}

quint32 newBreakpointId(const QVector<DebuggerBreakpoint>& breakpoints) {
    quint32 id = 0;
    for (const auto& breakpoint : breakpoints) id = std::max(id, breakpoint.id);
    do { ++id; } while (id == 0 || findBreakpoint(breakpoints, id));
    return id;
}

QString codeBytes(const kr_debug_instruction& instruction) {
    QString result;
    const auto count = std::min(instruction.length, quint32(sizeof(instruction.bytes)));
    for (quint32 i = 0; i < count; ++i) {
        if (i) result += QLatin1Char(' ');
        result += hex(instruction.bytes[i], 2);
    }
    return result;
}

struct FlagBit {
    const char* label = nullptr;
    quint32 bit = 0;
};

using FlagBits = std::array<FlagBit, 8>;

const FlagBits& flagBits(quint32 kind) {
    static constexpr FlagBits m68k{{
        {"T", 15}, {"S", 13}, {"X", 4}, {"N", 3}, {"Z", 2}, {"V", 1}, {"C", 0}
    }};
    static constexpr FlagBits z80{{
        {"S", 7}, {"Z", 6}, {"Y", 5}, {"H", 4}, {"X", 3}, {"P/V", 2}, {"N", 1}, {"C", 0}
    }};
    static constexpr FlagBits sh2{{{"T", 0}, {"S", 1}, {"Q", 8}, {"M", 9}}};
    static constexpr FlagBits ssp{{{"N", 15}, {"V", 14}, {"Z", 13}, {"L", 12}}};
    static constexpr FlagBits none{};
    switch (kind) {
    case KR_DEBUG_FLAGS_M68K: return m68k;
    case KR_DEBUG_FLAGS_Z80: return z80;
    case KR_DEBUG_FLAGS_SH2: return sh2;
    case KR_DEBUG_FLAGS_SSP1601: return ssp;
    default: return none;
    }
}

const kr_debug_reg* findRegister(const kr_debug_cpu& cpu, QByteArrayView name) {
    const auto count = std::min(cpu.count, KR_DEBUG_MAX_REGS);
    for (quint32 i = 0; i < count; ++i)
        if (QByteArrayView(cpu.regs[i].name) == name) return &cpu.regs[i];
    return nullptr;
}

const kr_debug_reg* flagRegister(const kr_debug_cpu& cpu, quint32 kind) {
    switch (kind) {
    case KR_DEBUG_FLAGS_M68K:
    case KR_DEBUG_FLAGS_SH2:
        return findRegister(cpu, "SR");
    case KR_DEBUG_FLAGS_Z80:
        if (const auto* af = findRegister(cpu, "AF")) return af;
        return findRegister(cpu, "F");
    case KR_DEBUG_FLAGS_SSP1601:
        return findRegister(cpu, "ST");
    default:
        return nullptr;
    }
}
} // namespace

CpuDebuggerDialog::CpuDebuggerDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                      Qt::WindowCloseButtonHint) {
    setObjectName(QStringLiteral("cpuDebuggerDialog"));
    setWindowTitle(text("Отладчик процессоров"));
    setWindowModality(Qt::NonModal);
    resize(850, 540);

    auto* outer = new QVBoxLayout(this);
    auto* controls = new QHBoxLayout;
    cpuLabel_ = new QLabel(QStringLiteral("CPU"), this);
    cpu_ = named(new QComboBox(this), "debugCpuSelector");
    cpu_->setAccessibleName(text("Процессор"));
    cpuLabel_->setBuddy(cpu_);
    controls->addWidget(cpuLabel_);
    controls->addWidget(cpu_);
    const std::array<QString, 5> actionLabels{
        text("Пауза"), text("Продолжить"), text("Шаг внутрь"), text("Шаг без захода"), text("Шаг из подпрограммы")
    };
    const std::array<const char*, 5> actionNames{
        "debugCpuPause", "debugCpuContinue", "debugCpuStepInto", "debugCpuStepOver", "debugCpuStepOut"
    };
    for (size_t i = 0; i < execution_.size(); ++i) {
        execution_[i] = button(actionLabels[i], this, actionNames[i]);
        controls->addWidget(execution_[i]);
        connect(execution_[i], &QPushButton::clicked, this, [this, i] { send(executionActions[i]); });
    }
    controls->addStretch();
    link_ = named(new QCheckBox(QStringLiteral("Link Cable"), this), "debugCpuLinkCable");
    link_->setToolTip(text("Подключить две локальные консоли Link Cable"));
    connect(link_, &QCheckBox::clicked, this, [this](bool enabled) {
        const QSignalBlocker block(link_);
        link_->setChecked(snapshot_.linkEnabled);
        auto request = refreshCommand();
        request.kind = DebuggerCommand::SetLink;
        request.linkEnabled = enabled;
        emit command(request);
    });
    controls->addWidget(link_);
    outer->addLayout(controls);
    status_ = named(new QLabel(this), "debugCpuStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setToolTip(text("Исполнение останавливается перед выборкой команды. Чтение и запись останавливают машину после обращения к данным."));
    outer->addWidget(status_);
    feedback_ = named(new QLabel(this), "debugCpuFeedback");
    feedback_->setTextFormat(Qt::PlainText);
    feedback_->setWordWrap(true);
    feedback_->hide();
    outer->addWidget(feedback_);

    pages_ = named(new QTabWidget(this), "debugCpuTabs");
    outer->addWidget(pages_, 1);
    auto* codePage = new QWidget(pages_);
    auto* codeLayout = new QVBoxLayout(codePage);
    auto* navigation = new QHBoxLayout;
    address_ = named(new QLineEdit(codePage), "debugCpuAddress");
    address_->setPlaceholderText(text("Адрес (hex) или метка"));
    address_->setAccessibleName(address_->placeholderText());
    address_->setMaxLength(96);
    go_ = button(text("Перейти"), codePage, "debugCpuNavigate");
    follow_ = button(QStringLiteral("PC"), codePage, "debugCpuFollowPc");
    follow_->setCheckable(true);
    follow_->setToolTip(text("Следить за счётчиком команд"));
    import_ = button(text("Импорт меток…"), codePage, "debugCpuImportSymbols");
    import_->setToolTip(text("Текстовый файл UTF-8. Каждая строка содержит HEX ИМЯ и необязательный комментарий после # или ;."));
    bookmarks_ = named(new QComboBox(codePage), "debugCpuBookmarks");
    bookmarks_->setAccessibleName(text("Закладки"));
    navigation->addWidget(address_, 1);
    navigation->addWidget(go_);
    navigation->addWidget(follow_);
    navigation->addWidget(bookmarks_);
    navigation->addWidget(import_);
    codeLayout->addLayout(navigation);

    auto* split = new QSplitter(Qt::Horizontal, codePage);
    code_ = named(new QTableWidget(split), "debugCpuDisassembly");
    code_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    code_->setContextMenuPolicy(Qt::CustomContextMenu);
    setupTable(code_, {text("Адрес"), text("Байты"), text("Инструкция"), text("Метка / комментарий")});
    code_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    code_->setColumnWidth(2, 170);
    split->addWidget(code_);
    stateTabs_ = new QTabWidget(split);
    auto* registerPage = new QWidget(stateTabs_);
    auto* registerLayout = new QVBoxLayout(registerPage);
    registerLayout->setContentsMargins(0, 0, 0, 0);
    registers_ = named(new QTableWidget(registerPage), "debugCpuRegisters");
    setupTable(registers_, {text("Регистр"), text("Значение")});
    registerLayout->addWidget(registers_, 1);
    flags_ = named(new QGroupBox(text("Флаги"), registerPage), "debugCpuFlags");
    auto* flagLayout = new QGridLayout(flags_);
    for (size_t i = 0; i < flagBits_.size(); ++i) {
        auto* flag = new QCheckBox(flags_);
        flag->setObjectName(QStringLiteral("debugCpuFlag%1").arg(i));
        flagBits_[i] = flag;
        flagLayout->addWidget(flag, int(i) / 4, int(i) % 4);
        connect(flag, &QCheckBox::clicked, this, [this, i](bool checked) { editFlag(i, checked); });
    }
    registerLayout->addWidget(flags_);
    connect(registers_, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        if (column == 1) editRegister(row);
    });
    stateTabs_->addTab(registerPage, text("Регистры"));
    stack_ = named(new QPlainTextEdit(stateTabs_), "debugCpuStack");
    stack_->setReadOnly(true);
    stack_->setLineWrapMode(QPlainTextEdit::NoWrap);
    stack_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    stateTabs_->addTab(stack_, text("Стек"));
    split->addWidget(stateTabs_);
    split->setSizes({510, 280});
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 1);
    codeLayout->addWidget(split, 1);
    pages_->addTab(codePage, text("Дизассемблер"));

    auto* breakPage = new QWidget(pages_);
    auto* breakLayout = new QVBoxLayout(breakPage);
    breakpoints_ = named(new QTableWidget(breakPage), "debugCpuBreakpoints");
    setupTable(breakpoints_, {text("Вкл."), text("Процессор"), text("События"),
                              text("Диапазон (hex)"), text("Условие"), text("Пропустить"),
                              text("Срабатывания"), text("Однократная")});
    breakLayout->addWidget(breakpoints_, 1);

    breakpointEditor_ = new QWidget(breakPage);
    auto* editor = new QGridLayout(breakpointEditor_);
    editor->setContentsMargins(0, 0, 0, 0);
    breakCpu_ = named(new QComboBox(breakpointEditor_), "debugBreakCpu");
    breakCpu_->setAccessibleName(text("Процессор"));
    breakEnabled_ = named(new QCheckBox(text("Включена"), breakpointEditor_), "debugBreakEnabled");
    breakOnce_ = named(new QCheckBox(text("Однократная"), breakpointEditor_), "debugBreakOneShot");
    auto* typeRow = new QHBoxLayout;
    typeRow->addWidget(breakCpu_);
    typeRow->addWidget(breakEnabled_);
    typeRow->addWidget(breakOnce_);
    for (int i = 0; i < int(breakEvents_.size()); ++i) {
        breakEvents_[i] = new QCheckBox(eventNames(eventBits[i]), breakpointEditor_);
        breakEvents_[i]->setObjectName(QStringLiteral("debugBreakEvent%1").arg(eventBits[i]));
        typeRow->addWidget(breakEvents_[i]);
    }
    typeRow->addStretch();
    editor->addLayout(typeRow, 0, 0, 1, 4);

    breakFirst_ = named(new QLineEdit(breakpointEditor_), "debugBreakFirst");
    breakLast_ = named(new QLineEdit(breakpointEditor_), "debugBreakLast");
    breakFirst_->setPlaceholderText(text("Начало"));
    breakLast_->setPlaceholderText(text("Конец"));
    for (auto* input : {breakFirst_, breakLast_}) {
        input->setAccessibleName(input->placeholderText());
        input->setMaxLength(10);
        input->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("(?:0[xX]|\\$)?[0-9a-fA-F]{1,8}")), input));
    }
    auto* range = new QLabel(text("Диапазон (hex)"), breakpointEditor_);
    range->setBuddy(breakFirst_);
    editor->addWidget(range, 1, 0);
    auto* rangeRow = new QHBoxLayout;
    rangeRow->addWidget(breakFirst_);
    rangeRow->addWidget(new QLabel(QStringLiteral("–"), breakpointEditor_));
    rangeRow->addWidget(breakLast_);
    editor->addLayout(rangeRow, 1, 1);
    breakIgnore_ = named(new QLineEdit(breakpointEditor_), "debugBreakIgnoreHits");
    breakIgnore_->setMaxLength(20);
    breakIgnore_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{1,20}")), breakIgnore_));
    auto* ignore = new QLabel(text("Пропустить срабатывания"), breakpointEditor_);
    ignore->setBuddy(breakIgnore_);
    editor->addWidget(ignore, 1, 2);
    editor->addWidget(breakIgnore_, 1, 3);

    breakCondition_ = named(new QLineEdit(breakpointEditor_), "debugBreakCondition");
    breakCondition_->setMaxLength(160);
    breakCondition_->setPlaceholderText(QStringLiteral("ADDR >= $C000   /   VALUE & $FF == 5"));
    breakCondition_->setToolTip(text("Регистр, ADDR, VALUE, SIZE или HIT, затем необязательная маска & и сравнение с числом. Префиксы $ и 0x задают hex."));
    auto* condition = new QLabel(text("Условие"), breakpointEditor_);
    condition->setBuddy(breakCondition_);
    editor->addWidget(condition, 2, 0);
    editor->addWidget(breakCondition_, 2, 1, 1, 3);
    auto* edits = new QHBoxLayout;
    breakNew_ = button(text("Новая"), breakpointEditor_, "debugBreakNew");
    breakSave_ = button(text("Добавить"), breakpointEditor_, "debugBreakSave");
    breakRemove_ = button(text("Удалить"), breakpointEditor_, "debugBreakRemove");
    edits->addWidget(breakNew_);
    edits->addWidget(breakSave_);
    edits->addWidget(breakRemove_);
    edits->addStretch();
    editor->addLayout(edits, 3, 0, 1, 4);
    breakLayout->addWidget(breakpointEditor_);
    pages_->addTab(breakPage, text("Точки останова"));

    connect(cpu_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) return;
        cpuId_ = cpu_->itemData(index).toUInt();
        updateControls();
        updateCode();
        updateState();
        updateBookmarks();
        followProgramCounter();
    });
    connect(go_, &QPushButton::clicked, this, &CpuDebuggerDialog::goToInput);
    connect(address_, &QLineEdit::returnPressed, this, &CpuDebuggerDialog::goToInput);
    connect(follow_, &QPushButton::clicked, this, &CpuDebuggerDialog::followProgramCounter);
    connect(import_, &QPushButton::clicked, this, &CpuDebuggerDialog::importSymbols);
    connect(bookmarks_, &QComboBox::activated, this, [this](int index) {
        if (index > 0) navigateTo(cpuId_, bookmarks_->itemData(index).toUInt());
        bookmarks_->setCurrentIndex(0);
    });
    connect(code_, &QTableWidget::cellDoubleClicked, this, [this](int row) { toggleExecuteBreakpoint(row); });
    connect(code_, &QTableWidget::customContextMenuRequested, this, &CpuDebuggerDialog::showCodeMenu);
    connect(breakpoints_, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row < 0 || !breakpoints_->item(row, 0)) return;
        const auto id = breakpoints_->item(row, 0)->data(Qt::UserRole).toUInt();
        editBreakpoint(findBreakpoint(snapshot_.breakpoints, id));
    });
    connect(breakNew_, &QPushButton::clicked, this, [this] {
        const QSignalBlocker block(breakpoints_);
        breakpoints_->setCurrentCell(-1, -1);
        editBreakpoint(nullptr);
        breakFirst_->setFocus(Qt::OtherFocusReason);
    });
    connect(breakCpu_, &QComboBox::currentIndexChanged, this, [this] { updateBreakEventAvailability(); });
    connect(breakSave_, &QPushButton::clicked, this, &CpuDebuggerDialog::saveBreakpoint);
    connect(breakRemove_, &QPushButton::clicked, this, &CpuDebuggerDialog::removeBreakpoint);
    editBreakpoint(nullptr);
    updateControls();
    updateState();
    updateBookmarks();
}

DebuggerCommand CpuDebuggerDialog::refreshCommand() const {
    DebuggerCommand result;
    result.cpu = cpuId_;
    result.address = addressValue_;
    result.followPc = followPc_;
    return result;
}

void CpuDebuggerDialog::send(DebuggerCommand::Kind action) {
    DebuggerCommand request = refreshCommand();
    request.kind = action;
    emit command(request);
}

void CpuDebuggerDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    send(DebuggerCommand::Attach);
}

void CpuDebuggerDialog::hideEvent(QHideEvent* event) {
    QDialog::hideEvent(event);
    send(DebuggerCommand::Detach);
    setSnapshot({});
}

void CpuDebuggerDialog::navigateTo(quint32 cpu, quint32 address) {
    if (address > addressMask(cpu) || (snapshot_.attached && !findTarget(snapshot_, cpu))) return;
    cpuId_ = cpu;
    addressValue_ = address;
    followPc_ = false;
    address_->setText(hex(address, addressDigits(cpu)));
    updateCpuChoices();
    updateControls();
    updateBookmarks();
    follow_->setChecked(false);
    feedback_->hide();
    if (isVisible()) send(DebuggerCommand::Navigate);
}

void CpuDebuggerDialog::followProgramCounter() {
    followPc_ = true;
    follow_->setChecked(true);
    feedback_->hide();
    send(DebuggerCommand::Navigate);
}

void CpuDebuggerDialog::goToInput() {
    const QString query = address_->text().trimmed();
    if (query.isEmpty()) return;
    quint32 target = 0;
    if (readHex(query, addressMask(cpuId_), target)) {
        navigateTo(cpuId_, target);
        return;
    }
    const auto symbols = symbols_.constFind(cpuId_);
    bool found = false;
    if (symbols != symbols_.cend()) {
        for (auto symbol = symbols->cbegin(); symbol != symbols->cend(); ++symbol) {
            if (symbol->label != query) continue;
            if (found) {
                feedback_->setText(text("Метка встречается по нескольким адресам."));
                feedback_->show();
                return;
            }
            found = true;
            target = symbol.key();
        }
    }
    if (found) {
        navigateTo(cpuId_, target);
    } else {
        feedback_->setText(text("Введите адрес в шестнадцатеричном виде или имя метки."));
        feedback_->show();
    }
}

void CpuDebuggerDialog::setSnapshot(const DebuggerSnapshot& snapshot) {
    if (!isVisible() && snapshot.attached) return;
    const bool changedGeneration = snapshot.generation && generation_ && snapshot.generation != generation_;
    const bool changedGame = !snapshot.gameKey.isEmpty() && !gameKey_.isEmpty() && snapshot.gameKey != gameKey_;
    if (changedGame) symbols_.clear();
    if (!snapshot.gameKey.isEmpty()) gameKey_ = snapshot.gameKey;
    if (changedGeneration || changedGame) {
        feedback_->hide();
        followPc_ = true;
    }
    if (snapshot.generation) generation_ = snapshot.generation;
    const bool newTrap = snapshot.stopped && snapshot.event &&
        (!snapshot_.stopped || snapshot.event != snapshot_.event ||
         snapshot.eventCpu != snapshot_.eventCpu || snapshot.address != snapshot_.address);
    const quint32 previousCpu = cpuId_;
    const bool wasAttached = snapshot_.attached;
    if (snapshot.attached && !snapshot.linkEnabled) {
        for (auto symbol = symbols_.begin(); symbol != symbols_.end();) {
            if (symbol.key() >> 8) symbol = symbols_.erase(symbol);
            else ++symbol;
        }
    }
    snapshot_ = snapshot;
    if (snapshot_.attached && !snapshot_.targets.isEmpty()) {
        const quint32 stopCpu = snapshot_.eventCpu ? snapshot_.eventCpu : snapshot_.selectedCpu;
        if (newTrap && findTarget(snapshot_, stopCpu)) {
            cpuId_ = stopCpu;
            followPc_ = true;
        } else if (!findTarget(snapshot_, cpuId_) && findTarget(snapshot_, snapshot_.selectedCpu)) {
            cpuId_ = snapshot_.selectedCpu;
            followPc_ = true;
        } else if (!findTarget(snapshot_, cpuId_)) {
            cpuId_ = snapshot_.targets.first().cpu;
            followPc_ = true;
        }
    }
    updateCpuChoices();
    if (changedGeneration || changedGame || !snapshot_.attached) editBreakpoint(nullptr);
    updateControls();
    updateCode();
    updateState();
    updateBreakpoints();
    if (changedGame || previousCpu != cpuId_ || wasAttached != snapshot_.attached) updateBookmarks();
}

QString CpuDebuggerDialog::cpuName(quint32 cpu) const {
    const auto* target = findTarget(snapshot_, cpu);
    if (!target) return QStringLiteral("—");
    const QString name = Localization::text(QString::fromUtf8(target->name));
    if (!snapshot_.linkEnabled) return name;
    const QString console = cpu >> 8 ? text("Консоль B") : text("Консоль A");
    return QStringLiteral("%1 · %2").arg(console, name);
}

int CpuDebuggerDialog::addressDigits(quint32 cpu) const {
    const auto* target = findTarget(snapshot_, cpu);
    return target ? (std::clamp(target->address_bits, 1u, 32u) + 3) / 4 : 8;
}

quint32 CpuDebuggerDialog::addressMask(quint32 cpu) const {
    const auto* target = findTarget(snapshot_, cpu);
    return bitMask(target ? target->address_bits : 32);
}

void CpuDebuggerDialog::updateControls() {
    const auto* target = snapshot_.attached ? findTarget(snapshot_, cpuId_) : nullptr;
    const bool connected = target != nullptr;
    const quint32 capabilities = target ? target->capabilities : 0;
    execution_[0]->setEnabled(connected && !snapshot_.stopped && (capabilities & KR_DEBUG_CAP_EXEC));
    execution_[1]->setEnabled(connected && snapshot_.stopped);
    const std::array<quint32, 3> steps{KR_DEBUG_CAP_STEP, KR_DEBUG_CAP_OVER, KR_DEBUG_CAP_OUT};
    for (size_t i = 0; i < steps.size(); ++i)
        execution_[i + 2]->setEnabled(connected && snapshot_.stopped && (capabilities & steps[i]));
    stateTabs_->setTabVisible(stateTabs_->indexOf(stack_), capabilities & KR_DEBUG_CAP_STACK);
    const QSignalBlocker linkBlock(link_);
    link_->setVisible(snapshot_.linkSupported || snapshot_.linkEnabled);
    link_->setChecked(snapshot_.linkEnabled);
    link_->setEnabled(snapshot_.linkSupported);
    const QString addressPrompt = target && target->address_shift
        ? text("Адрес слова (hex) или метка") : text("Адрес (hex) или метка");
    address_->setPlaceholderText(addressPrompt);
    address_->setAccessibleName(addressPrompt);
    for (auto* control : {go_, follow_, import_}) control->setEnabled(connected);
    address_->setEnabled(connected);
    follow_->setChecked(followPc_);
    bookmarks_->setEnabled(connected);
    breakpointEditor_->setEnabled(connected);
    const bool editing = findBreakpoint(snapshot_.breakpoints, editingBreakpoint_) != nullptr;
    breakRemove_->setEnabled(connected && editing);
    breakSave_->setText(editing ? text("Применить") : text("Добавить"));
    updateBreakEventAvailability();

    if (!snapshot_.status.isEmpty()) {
        status_->setText(snapshot_.status);
    } else if (!connected) {
        status_->setText(text("Нет данных отладчика"));
    } else {
        QString state;
        const quint32 stopCpu = snapshot_.eventCpu ? snapshot_.eventCpu : snapshot_.selectedCpu;
        const QString address = QStringLiteral("0x%1").arg(hex(snapshot_.address, addressDigits(stopCpu)));
        if (!snapshot_.stopped) {
            state = text("Выполняется");
        } else if (snapshot_.event == KR_DEBUG_EXEC) {
            state = text("Остановлено перед исполнением по %1").arg(address);
        } else if (snapshot_.event & (KR_DEBUG_READ | KR_DEBUG_WRITE)) {
            const QString value = QStringLiteral("0x%1")
                .arg(hex(snapshot_.value, std::min(snapshot_.accessSize, 4u) * 2));
            state = snapshot_.event == KR_DEBUG_READ
                ? text("Остановлено после чтения по %1 (%2)").arg(address, value)
                : text("Остановлено после записи по %1 (%2)").arg(address, value);
        } else {
            state = text("Остановлено");
        }
        const QString frame = text("Кадр %1").arg(snapshot_.frame);
        const QString unit = cpuName(snapshot_.stopped && snapshot_.event ? stopCpu : cpuId_);
        status_->setText(QStringLiteral("%1 · %2 · %3").arg(unit, state, frame)
            + (snapshot_.partialFrame ? QStringLiteral(" · ") + text("Незавершённый кадр") : QString()));
    }
}

void CpuDebuggerDialog::updateCpuChoices() {
    const QSignalBlocker selectBlock(cpu_);
    const QSignalBlocker editBlock(breakCpu_);
    const auto previousBreakCpu = breakCpu_->currentData().toUInt();
    bool changed = cpu_->count() != snapshot_.targets.size();
    for (int i = 0; !changed && i < cpu_->count(); ++i) {
        const auto id = snapshot_.targets[i].cpu;
        changed = cpu_->itemData(i).toUInt() != id || cpu_->itemText(i) != cpuName(id);
    }
    if (changed) {
        cpu_->clear();
        breakCpu_->clear();
        for (const auto& target : snapshot_.targets) {
            const QString name = cpuName(target.cpu);
            cpu_->addItem(name, target.cpu);
            breakCpu_->addItem(name, target.cpu);
        }
    }
    cpu_->setCurrentIndex(cpu_->findData(cpuId_));
    const int edited = breakCpu_->findData(previousBreakCpu);
    breakCpu_->setCurrentIndex(edited >= 0 ? edited : breakCpu_->findData(cpuId_));
    const bool multiple = cpu_->count() > 1 || snapshot_.linkEnabled;
    cpu_->setVisible(multiple);
    cpuLabel_->setVisible(multiple);
}

void CpuDebuggerDialog::updateBreakEventAvailability() {
    const auto* target = findTarget(snapshot_, breakCpu_->currentData().toUInt());
    const quint32 events = target ? target->capabilities & 7u : 0;
    for (size_t i = 0; i < breakEvents_.size(); ++i) {
        breakEvents_[i]->setEnabled(events & eventBits[i]);
        if (!(events & eventBits[i])) breakEvents_[i]->setChecked(false);
    }
    breakSave_->setEnabled(snapshot_.attached && events != 0);
}

void CpuDebuggerDialog::updateCode() {
    if (!snapshot_.attached || cpuId_ != snapshot_.selectedCpu) {
        code_->setRowCount(0);
        return;
    }
    const auto* state = findCpu(snapshot_, cpuId_);
    const auto codeSymbols = symbols_.constFind(cpuId_);
    code_->setUpdatesEnabled(false);
    code_->setRowCount(snapshot_.instructions.size());
    int row = 0;
    for (const auto& instruction : snapshot_.instructions) {
        const bool current = state && state->pc == instruction.address;
        const bool breakpoint = std::any_of(snapshot_.breakpoints.cbegin(), snapshot_.breakpoints.cend(),
            [&](const DebuggerBreakpoint& bp) {
                return bp.enabled && bp.cpu == cpuId_ && (bp.events & KR_DEBUG_EXEC) &&
                       instruction.address >= bp.first && instruction.address <= bp.last;
            });
        QString address;
        if (breakpoint) address += QStringLiteral("● ");
        if (current) address += QStringLiteral("▶ ");
        address += hex(instruction.address, addressDigits(cpuId_));
        auto* addressCell = cell(code_, row, 0, address);
        if (addressCell->data(Qt::UserRole).toUInt() != instruction.address)
            addressCell->setData(Qt::UserRole, instruction.address);
        cell(code_, row, 1, instruction.valid ? codeBytes(instruction) : QStringLiteral("??"));
        cell(code_, row, 2, instruction.valid ? QString::fromLatin1(instruction.text) : text("Команда недоступна"));
        QString annotation;
        if (codeSymbols != symbols_.cend()) {
            const auto symbol = codeSymbols->constFind(instruction.address);
            if (symbol != codeSymbols->cend()) {
                if (symbol->bookmarked) annotation = QStringLiteral("★ ");
                annotation += symbol->label;
                if (!symbol->comment.isEmpty()) {
                    if (!symbol->label.isEmpty()) annotation += QStringLiteral(" · ");
                    annotation += symbol->comment;
                }
            }
        }
        cell(code_, row++, 3, annotation);
    }
    if (followPc_ && state && !address_->hasFocus())
        address_->setText(hex(state->pc, addressDigits(cpuId_)));
    code_->setUpdatesEnabled(true);
}

void CpuDebuggerDialog::updateState() {
    const auto* state = snapshot_.attached ? findCpu(snapshot_, cpuId_) : nullptr;
    const auto* target = findTarget(snapshot_, cpuId_);
    updateFlags(state, target);
    if (!state) {
        registers_->setRowCount(0);
        stack_->clear();
        return;
    }
    const auto count = std::min(state->count, KR_DEBUG_MAX_REGS);
    registers_->setRowCount(count + 1);
    registers_->setUpdatesEnabled(false);
    for (quint32 i = 0; i < count; ++i) {
        const auto& reg = state->regs[i];
        const QString name = QString::fromLatin1(reg.name);
        cell(registers_, i, 0, name);
        auto* item = cell(registers_, i, 1, reg.digits
                          ? QStringLiteral("0x%1").arg(hex(reg.value, std::min(reg.digits, 8u)))
                          : QString::number(reg.value));
        const QString tip = canEditRegister(name) ? text("Двойной щелчок для изменения регистра") : QString();
        if (item->toolTip() != tip) item->setToolTip(tip);
    }
    cell(registers_, count, 0, text("Такты в кадре"));
    cell(registers_, count, 1, QString::number(state->cycles))->setToolTip({});
    registers_->setUpdatesEnabled(true);

    QString dump;
    if (snapshot_.selectedCpu == cpuId_ && target && (target->capabilities & KR_DEBUG_CAP_STACK)) {
        const auto shift = std::min(target->address_shift, 31u);
        for (qsizetype i = 0; i < snapshot_.stack.size(); ++i) {
            if (i && i % 8 == 0) dump += QLatin1Char('\n');
            if (i % 8 == 0)
                dump += hex((snapshot_.stackAddress + (quint32(i) >> shift)) & addressMask(cpuId_), addressDigits(cpuId_));
            dump += QLatin1Char(' ');
            dump += i < snapshot_.stackValid.size() && snapshot_.stackValid[i]
                ? hex(quint8(snapshot_.stack[i]), 2) : QStringLiteral("??");
        }
    }
    if (stack_->toPlainText() != dump) stack_->setPlainText(dump);
}

void CpuDebuggerDialog::updateFlags(const kr_debug_cpu* state, const kr_debug_target* target) {
    const auto* reg = state && target ? flagRegister(*state, target->flags_kind) : nullptr;
    flags_->setVisible(reg != nullptr);
    if (!reg) return;
    const auto& bits = flagBits(target->flags_kind);
    const QString regName = QString::fromLatin1(reg->name);
    const bool editable = canEditRegister(regName);
    for (size_t i = 0; i < flagBits_.size(); ++i) {
        auto* box = flagBits_[i];
        const QSignalBlocker blocker(box);
        box->setVisible(bits[i].label != nullptr);
        if (!bits[i].label) continue;
        box->setEnabled(editable);
        box->setText(QString::fromLatin1(bits[i].label));
        box->setChecked(reg->value & (quint32(1) << bits[i].bit));
    }
    QString title = text("Флаги");
    if (target->flags_kind == KR_DEBUG_FLAGS_M68K)
        title += QStringLiteral(" · IPL %1").arg((reg->value >> 8) & 7);
    if (target->flags_kind == KR_DEBUG_FLAGS_SH2)
        title += QStringLiteral(" · I %1").arg((reg->value >> 4) & 15);
    if (flags_->title() != title) flags_->setTitle(title);
}

bool CpuDebuggerDialog::canEditRegister(const QString& name) const {
    return snapshot_.attached && snapshot_.canEdit && snapshot_.selectedCpu == cpuId_ &&
           snapshot_.writableRegisters.contains(name);
}

void CpuDebuggerDialog::editRegister(int row) {
    const auto* state = findCpu(snapshot_, cpuId_);
    if (!state || row < 0 || quint32(row) >= std::min(state->count, KR_DEBUG_MAX_REGS)) return;
    const auto& reg = state->regs[row];
    const QString name = QString::fromLatin1(reg.name);
    if (!canEditRegister(name)) return;
    const quint32 cpu = cpuId_;
    const quint64 context = generation_;
    const quint32 width = std::min(reg.digits, 8u);
    const quint32 limit = width ? bitMask(width * 4) : ~quint32(0);
    bool ok = false;
    const QString value = QInputDialog::getText(this, text("Изменить регистр"),
        text("Новое значение %1 (hex)").arg(name), QLineEdit::Normal, hex(reg.value, width), &ok);
    if (!ok || context != generation_ || cpu != cpuId_ || !canEditRegister(name)) return;
    quint32 parsed = 0;
    if (!readHex(value, limit, parsed)) {
        feedback_->setText(text("Введите шестнадцатеричное значение в пределах разрядности регистра."));
        feedback_->show();
        return;
    }
    writeRegister(name, parsed, ~quint32(0));
}

void CpuDebuggerDialog::editFlag(size_t index, bool checked) {
    const auto* state = findCpu(snapshot_, cpuId_);
    const auto* target = findTarget(snapshot_, cpuId_);
    const auto* reg = state && target ? flagRegister(*state, target->flags_kind) : nullptr;
    if (!reg || index >= flagBits_.size()) return;
    const auto& bit = flagBits(target->flags_kind)[index];
    const QString name = QString::fromLatin1(reg->name);
    if (!bit.label || !canEditRegister(name)) {
        updateFlags(state, target);
        return;
    }
    const quint32 mask = quint32(1) << bit.bit;
    writeRegister(name, checked ? mask : 0u, mask);
}

void CpuDebuggerDialog::writeRegister(const QString& name, quint32 value, quint32 mask) {
    feedback_->hide();
    auto request = refreshCommand();
    request.kind = DebuggerCommand::WriteRegister;
    request.registerName = name;
    request.registerValue = value;
    request.registerMask = mask;
    emit command(request);
}

void CpuDebuggerDialog::updateBreakpoints() {
    const QSignalBlocker block(breakpoints_);
    breakpoints_->setUpdatesEnabled(false);
    const auto& list = snapshot_.breakpoints;
    breakpoints_->setRowCount(snapshot_.attached ? list.size() : 0);
    int selectedRow = -1;
    for (int row = 0; snapshot_.attached && row < list.size(); ++row) {
        const auto& bp = list.at(row);
        auto* enabled = cell(breakpoints_, row, 0, bp.enabled ? QStringLiteral("✓") : QStringLiteral("—"));
        if (enabled->data(Qt::UserRole).toUInt() != bp.id) enabled->setData(Qt::UserRole, bp.id);
        if (bp.id == editingBreakpoint_) selectedRow = row;
        cell(breakpoints_, row, 1, cpuName(bp.cpu));
        cell(breakpoints_, row, 2, eventNames(bp.events));
        const int digits = addressDigits(bp.cpu);
        QString address = hex(bp.first, digits);
        if (bp.first != bp.last) address += QStringLiteral("–%1").arg(hex(bp.last, digits));
        cell(breakpoints_, row, 3, address);
        cell(breakpoints_, row, 4, bp.condition);
        cell(breakpoints_, row, 5, QString::number(bp.ignoreHits));
        cell(breakpoints_, row, 6, QString::number(bp.hits));
        cell(breakpoints_, row, 7, bp.oneShot ? QStringLiteral("✓") : QStringLiteral("—"));
    }
    if (selectedRow != breakpoints_->currentRow()) breakpoints_->setCurrentCell(selectedRow, selectedRow < 0 ? -1 : 0);
    breakpoints_->setUpdatesEnabled(true);
}

void CpuDebuggerDialog::editBreakpoint(const DebuggerBreakpoint* bp) {
    editingBreakpoint_ = bp ? bp->id : 0;
    breakEnabled_->setChecked(!bp || bp->enabled);
    breakFirst_->setText(bp ? hex(bp->first, addressDigits(bp->cpu)) : QString());
    breakLast_->setText(bp && bp->last != bp->first ? hex(bp->last, addressDigits(bp->cpu)) : QString());
    breakCondition_->setText(bp ? bp->condition : QString());
    breakIgnore_->setText(bp ? QString::number(bp->ignoreHits) : QStringLiteral("0"));
    breakOnce_->setChecked(bp && bp->oneShot);
    breakCpu_->setCurrentIndex(breakCpu_->findData(bp ? bp->cpu : cpuId_));
    for (int i = 0; i < int(breakEvents_.size()); ++i)
        breakEvents_[i]->setChecked((bp ? bp->events : quint32(KR_DEBUG_EXEC)) & eventBits[i]);
    breakSave_->setText(bp ? text("Применить") : text("Добавить"));
    breakRemove_->setEnabled(bp != nullptr);
    updateBreakEventAvailability();
    feedback_->hide();
}

void CpuDebuggerDialog::saveBreakpoint() {
    if (!snapshot_.attached || breakCpu_->currentIndex() < 0) return;
    auto list = snapshot_.breakpoints;
    const auto* existing = findBreakpoint(list, editingBreakpoint_);
    if (!existing && list.size() >= maxBreakpoints) {
        feedback_->setText(text("Можно задать не более %1 точек останова.").arg(maxBreakpoints));
        feedback_->show();
        return;
    }
    DebuggerBreakpoint bp = existing ? *existing : DebuggerBreakpoint{};
    if (!existing) bp.id = newBreakpointId(list);
    bp.cpu = breakCpu_->currentData().toUInt();
    bp.enabled = breakEnabled_->isChecked();
    bp.oneShot = breakOnce_->isChecked();
    bp.events = 0;
    for (int i = 0; i < int(breakEvents_.size()); ++i)
        if (breakEvents_[i]->isEnabled() && breakEvents_[i]->isChecked()) bp.events |= eventBits[i];
    if (!bp.events) {
        feedback_->setText(text("Выберите событие для точки останова."));
        feedback_->show();
        return;
    }
    const bool firstOk = readHex(breakFirst_->text(), addressMask(bp.cpu), bp.first);
    bp.last = bp.first;
    if (!firstOk || (!breakLast_->text().isEmpty() && !readHex(breakLast_->text(), addressMask(bp.cpu), bp.last))
                 || bp.last < bp.first) {
        feedback_->setText(text("Проверьте границы диапазона для выбранного процессора."));
        feedback_->show();
        return;
    }
    bool ignoreOk = false;
    bp.ignoreHits = breakIgnore_->text().toULongLong(&ignoreOk, 10);
    if (!ignoreOk && !breakIgnore_->text().isEmpty()) {
        feedback_->setText(text("Укажите число пропускаемых срабатываний."));
        feedback_->show();
        return;
    }
    bp.condition = breakCondition_->text().trimmed();
    if (existing) list[existing - list.constData()] = bp;
    else list.append(bp);
    editingBreakpoint_ = bp.id;
    configure(std::move(list), bp.id);
}

void CpuDebuggerDialog::removeBreakpoint() {
    if (!snapshot_.attached || !editingBreakpoint_) return;
    auto list = snapshot_.breakpoints;
    const auto id = editingBreakpoint_;
    const auto end = std::remove_if(list.begin(), list.end(),
                                   [id](const DebuggerBreakpoint& bp) { return bp.id == id; });
    if (end == list.end()) return;
    list.erase(end, list.end());
    editBreakpoint(nullptr);
    configure(std::move(list));
}

void CpuDebuggerDialog::toggleExecuteBreakpoint(int row) {
    const auto* target = findTarget(snapshot_, cpuId_);
    if (!snapshot_.attached || snapshot_.selectedCpu != cpuId_ || !target ||
        !(target->capabilities & KR_DEBUG_CAP_EXEC) || !code_->item(row, 0)) return;
    const auto addr = code_->item(row, 0)->data(Qt::UserRole).toUInt();
    auto list = snapshot_.breakpoints;
    for (auto& bp : list) {
        if (bp.cpu == cpuId_ && bp.events == KR_DEBUG_EXEC && bp.first == addr && bp.last == addr &&
            bp.condition.isEmpty() && !bp.ignoreHits && !bp.oneShot) {
            bp.enabled = !bp.enabled;
            const quint32 id = bp.id;
            configure(std::move(list), id);
            return;
        }
    }
    if (list.size() >= maxBreakpoints) {
        feedback_->setText(text("Можно задать не более %1 точек останова.").arg(maxBreakpoints));
        feedback_->show();
        return;
    }
    DebuggerBreakpoint bp;
    bp.id = newBreakpointId(list);
    bp.cpu = cpuId_;
    bp.first = bp.last = addr;
    list.append(bp);
    configure(std::move(list), bp.id);
}

void CpuDebuggerDialog::configure(QVector<DebuggerBreakpoint> breakpoints, quint32 editedId) {
    feedback_->hide();
    DebuggerCommand request = refreshCommand();
    request.kind = DebuggerCommand::Configure;
    request.editedBreakpointId = editedId;
    request.breakpoints = std::move(breakpoints);
    emit command(request);
}

void CpuDebuggerDialog::importSymbols() {
    const quint32 cpu = cpuId_;
    const quint64 context = generation_;
    const QString path = QFileDialog::getOpenFileName(this, text("Импорт меток"), {},
        text("Текстовые файлы (*.txt *.sym);;Все файлы (*)"));
    if (path.isEmpty() || generation_ != context || !snapshot_.attached) return;
    const auto fail = [this](const QString& message) {
        feedback_->setText(message);
        feedback_->show();
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(text("Не удалось открыть файл меток."));
        return;
    }
    const auto bytes = file.read(maxSymbolFile + 1);
    if (bytes.size() > maxSymbolFile) {
        fail(text("Файл меток превышает 512 КиБ."));
        return;
    }
    if (file.error() != QFileDevice::NoError) {
        fail(text("Не удалось прочитать файл меток."));
        return;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString contents = decoder.decode(bytes);
    if (decoder.hasError()) {
        fail(text("Файл меток должен быть в UTF-8."));
        return;
    }

    auto merged = symbols_.value(cpu);
    static const QRegularExpression entry(QStringLiteral("^([^\\s]+)\\s+([^\\s#;]+)(?:\\s*[#;]\\s*(.*))?$"));
    qsizetype imported = 0;
    qsizetype lineNumber = 0;
    for (const auto line : QStringView(contents).split(QLatin1Char('\n'))) {
        ++lineNumber;
        const auto trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')) || trimmed.startsWith(QLatin1Char(';'))) continue;
        const auto match = entry.match(trimmed.toString());
        quint32 address = 0;
        if (!match.hasMatch() || !readHex(match.captured(1), addressMask(cpu), address) ||
            match.capturedLength(2) > 96 || match.capturedLength(3) > 256) {
            fail(text("Ошибка в строке %1 файла меток.").arg(lineNumber));
            return;
        }
        Symbol& symbol = merged[address];
        symbol.label = match.captured(2);
        if (match.hasCaptured(3)) symbol.comment = match.captured(3);
        if (merged.size() > maxSymbols) {
            fail(text("Можно задать не более %1 меток для процессора.").arg(maxSymbols));
            return;
        }
        ++imported;
    }
    symbols_.insert(cpu, std::move(merged));
    feedback_->setText(text("Загружено %1 меток.").arg(imported));
    feedback_->show();
    updateCode();
    updateBookmarks();
}

void CpuDebuggerDialog::showCodeMenu(const QPoint& point) {
    if (!snapshot_.attached || snapshot_.selectedCpu != cpuId_) return;
    const QModelIndex index = code_->indexAt(point);
    if (!index.isValid()) return;
    const auto address = code_->item(index.row(), 0)->data(Qt::UserRole).toUInt();
    const quint32 cpu = cpuId_;
    const quint64 context = generation_;
    Symbol original;
    const auto symbolsForCpu = symbols_.constFind(cpu);
    if (symbolsForCpu != symbols_.cend()) original = symbolsForCpu->value(address);
    QMenu menu(this);
    auto* breakpoint = menu.addAction(text("Переключить точку останова"));
    const auto* target = findTarget(snapshot_, cpu);
    breakpoint->setEnabled(target && (target->capabilities & KR_DEBUG_CAP_EXEC));
    auto* bookmark = menu.addAction(original.bookmarked ? text("Убрать закладку") : text("Добавить закладку"));
    auto* comment = menu.addAction(text("Комментарий…"));
    const auto* action = menu.exec(code_->viewport()->mapToGlobal(point));
    if (!action || !snapshot_.attached || generation_ != context || cpu != cpuId_) return;
    if (action == breakpoint) {
        for (int row = 0; row < code_->rowCount(); ++row)
            if (code_->item(row, 0)->data(Qt::UserRole).toUInt() == address) {
                toggleExecuteBreakpoint(row);
                break;
            }
        return;
    }

    Symbol edited = original;
    if (action == bookmark) {
        edited.bookmarked = !original.bookmarked;
    } else if (action == comment) {
        bool ok = false;
        const QString note = QInputDialog::getText(this, text("Комментарий"),
            QStringLiteral("%1 · 0x%2").arg(cpuName(cpu), hex(address, addressDigits(cpu))),
            QLineEdit::Normal, original.comment, &ok);
        if (!ok || !snapshot_.attached || generation_ != context) return;
        edited.comment = note.trimmed().left(256);
    }
    auto& symbols = symbols_[cpu];
    if (!edited.bookmarked && edited.label.isEmpty() && edited.comment.isEmpty()) {
        symbols.remove(address);
    } else if (symbols.size() < maxSymbols || symbols.contains(address)) {
        symbols.insert(address, std::move(edited));
    } else {
        feedback_->setText(text("Можно задать не более %1 меток для процессора.").arg(maxSymbols));
        feedback_->show();
    }
    updateCode();
    updateBookmarks();
}

void CpuDebuggerDialog::updateBookmarks() {
    const QSignalBlocker block(bookmarks_);
    bookmarks_->clear();
    bookmarks_->addItem(text("Закладки"));
    const auto symbols = symbols_.constFind(cpuId_);
    if (symbols != symbols_.cend()) {
        for (auto entry = symbols->cbegin(); entry != symbols->cend(); ++entry) {
            if (!entry->bookmarked) continue;
            QString title = hex(entry.key(), addressDigits(cpuId_));
            if (!entry->label.isEmpty()) title += QStringLiteral(" · ") + entry->label;
            bookmarks_->addItem(title, entry.key());
        }
    }
    bookmarks_->setVisible(bookmarks_->count() > 1 && snapshot_.attached);
}
