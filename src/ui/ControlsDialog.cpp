#include "ControlsDialog.h"

#include "ControllerDiagram.h"

#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTableWidget>
#include <QToolButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
const QVector<int> commands = {
    PauseCommand, FastForwardCommand, SaveCommand, LoadCommand,
    ResetCommand, FullscreenCommand, MuteCommand,
    NextSlotCommand, PreviousSlotCommand, MenuCommand, RewindCommand, HardResetCommand, FrameAdvanceCommand
};

QString commandName(int command) {
    switch (command) {
    case PauseCommand: return QStringLiteral("Пауза");
    case FastForwardCommand: return QStringLiteral("Ускорение (удерживать)");
    case SaveCommand: return QStringLiteral("Сохранить состояние");
    case LoadCommand: return QStringLiteral("Загрузить состояние");
    case ResetCommand: return QStringLiteral("Мягкий сброс");
    case HardResetCommand: return QStringLiteral("Жёсткий сброс");
    case FrameAdvanceCommand: return QStringLiteral("Следующий кадр");
    case FullscreenCommand: return QStringLiteral("Полный экран");
    case MuteCommand: return QStringLiteral("Без звука");
    case NextSlotCommand: return QStringLiteral("Следующий слот");
    case PreviousSlotCommand: return QStringLiteral("Предыдущий слот");
    case MenuCommand: return QStringLiteral("Меню");
    case RewindCommand: return QStringLiteral("Перемотка назад (удерживать)");
    default: return QString();
    }
}

QString commandShortcut(int command) {
    switch (command) {
    case PauseCommand: return QStringLiteral("P");
    case FastForwardCommand: return QStringLiteral("Tab");
    case RewindCommand: return QStringLiteral("R");
    case SaveCommand: return QStringLiteral("F5");
    case LoadCommand: return QStringLiteral("F8");
    case ResetCommand: return QStringLiteral("Ctrl+R");
    case HardResetCommand: return QStringLiteral("Ctrl+Shift+R");
    case FrameAdvanceCommand: return QStringLiteral(".");
    case FullscreenCommand: return QStringLiteral("Alt+Enter");
    case MuteCommand: return QStringLiteral("M");
    default: return QStringLiteral("—");
    }
}

bool reservedKey(int key) {
    return InputManager::reservedKeyboardKey(key);
}

// Record the same physical identity that gameplay uses, regardless of layout.
class PhysicalKeySequenceEdit final : public QKeySequenceEdit {
public:
    using QKeySequenceEdit::QKeySequenceEdit;
protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::KeyRelease) {
            event->accept();
            return true;
        }
        if (event->type() != QEvent::KeyPress)
            return QKeySequenceEdit::event(event);
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Tab || keyEvent->key() == Qt::Key_Backtab)
            return QKeySequenceEdit::event(event);
        const int key = InputManager::keyboardKey(keyEvent->key(), keyEvent->nativeScanCode());
        if (!keyEvent->isAutoRepeat() && key != Qt::Key_Control &&
            key != Qt::Key_Shift && key != Qt::Key_Meta && key != Qt::Key_Alt &&
            key != Qt::Key_AltGr && key != Qt::Key_unknown) {
            const auto modifiers = keyEvent->modifiers() & ~Qt::KeypadModifier;
            setKeySequence(QKeySequence(QKeyCombination(modifiers, Qt::Key(key))));
        }
        event->accept();
        return true;
    }
};

QTableWidgetItem* cell(const QString& text, int target = -1) {
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    if (target >= 0)
        item->setData(Qt::UserRole, target);
    return item;
}

QString padName(const PlayerMapping& mapping, int target) {
    const auto it = mapping.pad.constFind(target);
    return it == mapping.pad.cend() || it->kind == PadBinding::None
        ? QStringLiteral("—") : InputManager::bindingName(*it);
}
}

ControlsDialog::ControlsDialog(InputManager& input, QWidget* parent)
    : QDialog(parent), input_(input) {
    setWindowTitle(QStringLiteral("Управление — Kega Refusion"));
    setMinimumSize(860, 580);
    resize(950, 660);
    setSizeGripEnabled(true);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(9);

    auto* scope = new QGridLayout;
    scope->setHorizontalSpacing(10);
    profileBox_ = new QComboBox(this);
    profileBox_->setObjectName(QStringLiteral("profileBox"));
    for (ConsoleProfile profile : {ConsoleProfile::MegaDrive,
                                   ConsoleProfile::MasterSystem,
                                   ConsoleProfile::GameGear,
                                   ConsoleProfile::SG1000}) {
        profileBox_->addItem(InputManager::profileName(profile),
                             static_cast<int>(profile));
    }
    profileBox_->setCurrentIndex(static_cast<int>(input_.profile()));
    playerBox_ = new QComboBox(this);
    playerBox_->setObjectName(QStringLiteral("playerBox"));
    playerBox_->addItem(QStringLiteral("Игрок 1"), 0);
    playerBox_->addItem(QStringLiteral("Игрок 2"), 1);
    deviceBox_ = new QComboBox(this);
    deviceBox_->setMinimumWidth(230);
    deadzoneSlider_ = new QSlider(Qt::Horizontal, this);
    deadzoneSlider_->setRange(5, 60);
    deadzoneSlider_->setMinimumWidth(140);
    deadzoneValue_ = new QLabel(this);
    deadzoneValue_->setMinimumWidth(35);

    scope->addWidget(new QLabel(QStringLiteral("Система"), this), 0, 0);
    scope->addWidget(profileBox_, 1, 0);
    scope->addWidget(new QLabel(QStringLiteral("Игрок"), this), 0, 1);
    scope->addWidget(playerBox_, 1, 1);
    scope->addWidget(new QLabel(QStringLiteral("Устройство"), this), 0, 2);
    scope->addWidget(deviceBox_, 1, 2);
    scope->addWidget(new QLabel(QStringLiteral("Мёртвая зона"), this), 0, 3);
    auto* deadzoneRow = new QHBoxLayout;
    deadzoneRow->setContentsMargins(0, 0, 0, 0);
    deadzoneRow->addWidget(deadzoneSlider_);
    deadzoneRow->addWidget(deadzoneValue_);
    scope->addLayout(deadzoneRow, 1, 3);
    scope->setColumnStretch(2, 2);
    scope->setColumnStretch(3, 1);
    connectedBox_ = new QCheckBox(QStringLiteral("Подключён к консоли"), this);
    connectedBox_->setObjectName(QStringLiteral("connectedBox"));
    connectedBox_->setToolTip(QStringLiteral("Отключённый порт не виден игре. Назначения сохраняются."));
    scope->addWidget(connectedBox_, 2, 0, 1, 2);
    controllerTypeLabel_ = new QLabel(QStringLiteral("Тип геймпада"), this);
    controllerTypeBox_ = new QComboBox(this);
    controllerTypeBox_->setObjectName(QStringLiteral("controllerTypeBox"));
    controllerTypeBox_->addItem(QStringLiteral("3 кнопки"), false);
    controllerTypeBox_->addItem(QStringLiteral("6 кнопок"), true);
    controllerTypeBox_->setToolTip(QStringLiteral("Тип геймпада, который видит игра. Назначения сохраняются при переключении."));
    auto* controllerTypeRow = new QHBoxLayout;
    controllerTypeRow->addWidget(controllerTypeLabel_);
    controllerTypeRow->addWidget(controllerTypeBox_);
    controllerTypeRow->addStretch(1);
    scope->addLayout(controllerTypeRow, 2, 2, 1, 2);
    deviceTypeLabel_ = new QLabel(QStringLiteral("Контроллер"), this);
    deviceTypeBox_ = new QComboBox(this);
    deviceTypeBox_->setObjectName(QStringLiteral("deviceTypeBox"));
    deviceTypeBox_->setToolTip(QStringLiteral("Устройство, которое видит игра. Назначения кнопок сохраняются."));
    scope->addWidget(deviceTypeLabel_, 3, 0);
    scope->addWidget(deviceTypeBox_, 3, 1);
    aimSourceLabel_ = new QLabel(QStringLiteral("Прицеливание"), this);
    aimSourceBox_ = new QComboBox(this);
    aimSourceBox_->setObjectName(QStringLiteral("aimSourceBox"));
    aimSourceBox_->addItem(QStringLiteral("Мышь"), int(AimSource::Mouse));
    aimSourceBox_->addItem(QStringLiteral("Левый стик"), int(AimSource::LeftStick));
    auto* aimRow = new QHBoxLayout;
    aimRow->addWidget(aimSourceLabel_);
    aimRow->addWidget(aimSourceBox_);
    aimRow->addStretch(1);
    scope->addLayout(aimRow, 3, 2, 1, 2);
    aimHint_ = new QLabel(QStringLiteral(
        "Мышь доступна одному пистолету; для второго выберите левый стик. "
        "Прицелы: игрок 1 — голубой, игрок 2 — оранжевый."), this);
    aimHint_->setWordWrap(true);
    scope->addWidget(aimHint_, 4, 0, 1, 4);
    mainLayout->addLayout(scope);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("mappingTabs"));
    auto* gamePage = new QWidget(tabs_);
    auto* gameLayout = new QHBoxLayout(gamePage);
    gameLayout->setContentsMargins(7, 8, 7, 7);
    gameLayout->setSpacing(10);
    diagram_ = new ControllerDiagram(gamePage);
    gameLayout->addWidget(diagram_, 4);
    gameTable_ = new QTableWidget(gamePage);
    gameTable_->setColumnCount(3);
    gameTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Кнопка"), QStringLiteral("Клавиатура"),
         QStringLiteral("Геймпад")});
    gameTable_->verticalHeader()->hide();
    gameTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    gameTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    gameTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    gameTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    gameTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    gameTable_->horizontalHeader()->resizeSection(1, 155);
    gameTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    gameTable_->setAlternatingRowColors(true);
    gameLayout->addWidget(gameTable_, 5);
    tabs_->addTab(gamePage, QStringLiteral("Кнопки игры"));

    auto* commandPage = new QWidget(tabs_);
    auto* commandLayout = new QVBoxLayout(commandPage);
    auto* commandHint = new QLabel(
        QStringLiteral("Команды можно назначить на геймпад. "
                       "Сочетания клавиатуры показаны для справки; здесь они не меняются."),
        commandPage);
    commandHint->setWordWrap(true);
    commandLayout->addWidget(commandHint);
    commandTable_ = new QTableWidget(commandPage);
    commandTable_->setColumnCount(3);
    commandTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Команда"), QStringLiteral("Клавиатура"),
         QStringLiteral("Геймпад")});
    commandTable_->verticalHeader()->hide();
    commandTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    commandTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    commandTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    commandTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    commandTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    commandTable_->horizontalHeader()->resizeSection(1, 125);
    commandTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    commandTable_->setAlternatingRowColors(true);
    commandLayout->addWidget(commandTable_);
    tabs_->addTab(commandPage, QStringLiteral("Команды"));
    mainLayout->addWidget(tabs_, 1);

    auto* actionRow = new QHBoxLayout;
    assignButton_ = new QPushButton(QStringLiteral("Назначить"), this);
    sequenceButton_ = new QPushButton(QStringLiteral("Назначить по порядку"), this);
    skipButton_ = new QPushButton(QStringLiteral("Пропустить"), this);
    clearButton_ = new QPushButton(QStringLiteral("Очистить геймпад"), this);
    auto* defaultsButton = new QPushButton(QStringLiteral("По умолчанию"), this);
    actionRow->addWidget(assignButton_);
    actionRow->addWidget(sequenceButton_);
    actionRow->addWidget(skipButton_);
    actionRow->addWidget(clearButton_);
    actionRow->addStretch(1);
    actionRow->addWidget(defaultsButton);
    mainLayout->addLayout(actionRow);

    feedback_ = new QLabel(this);
    feedback_->setWordWrap(true);
    feedback_->setMinimumHeight(25);
    mainLayout->addWidget(feedback_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("Сохранить"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    mainLayout->addWidget(buttons);

    connect(profileBox_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { stopCapture(); refreshContents(); });
    connect(playerBox_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { stopCapture(); refreshContents(); });
    connect(deviceBox_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] {
                if (refreshing_) return;
                stopCapture();
                draft().deviceKey = deviceBox_->currentData().toString();
                dirty_.insert(draftKey());
                updateActionButtons();
            });
    connect(connectedBox_, &QCheckBox::toggled, this, [this](bool connected) {
        if (refreshing_) return;
        stopCapture();
        draft().connected = connected;
        dirty_.insert(draftKey());
        updateActionButtons();
    });
    connect(controllerTypeBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (refreshing_) return;
        stopCapture();
        draft().sixButton = controllerTypeBox_->currentData().toBool();
        dirty_.insert(draftKey());
        refreshContents();
    });
    connect(deviceTypeBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (refreshing_) return;
        stopCapture();
        draft().controllerType = static_cast<ControllerType>(deviceTypeBox_->currentData().toInt());
        dirty_.insert(draftKey());
        refreshContents();
    });
    connect(aimSourceBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (refreshing_) return;
        stopCapture();
        draft().aimSource = static_cast<AimSource>(aimSourceBox_->currentData().toInt());
        dirty_.insert(draftKey());
    });
    connect(deadzoneSlider_, &QSlider::valueChanged, this, [this](int value) {
        deadzoneValue_->setText(QStringLiteral("%1 %").arg(value));
        if (refreshing_) return;
        draft().deadzonePercent = value;
        dirty_.insert(draftKey());
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        stopCapture();
        const auto& targets = tabs_->currentIndex() == 0
            ? activeGameTargets() : commands;
        selectTarget(targets.isEmpty() ? -1 : targets.first());
    });
    connect(gameTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        if (!refreshing_ && gameTable_->currentRow() >= 0) {
            const auto* item = gameTable_->item(gameTable_->currentRow(), 0);
            if (item) selectTarget(item->data(Qt::UserRole).toInt());
        }
    });
    connect(commandTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        if (!refreshing_ && commandTable_->currentRow() >= 0) {
            const auto* item = commandTable_->item(commandTable_->currentRow(), 0);
            if (item) selectTarget(item->data(Qt::UserRole).toInt());
        }
    });
    connect(diagram_, &ControllerDiagram::targetClicked,
            this, &ControlsDialog::selectTarget);
    connect(assignButton_, &QPushButton::clicked, this, [this] {
        stopCapture();
        if (selectedTarget_ >= 0)
            beginCaptureFor(selectedTarget_);
    });
    connect(sequenceButton_, &QPushButton::clicked,
            this, &ControlsDialog::startSequence);
    connect(skipButton_, &QPushButton::clicked,
            this, &ControlsDialog::skipCapture);
    connect(clearButton_, &QPushButton::clicked, this, [this] {
        if (selectedTarget_ < 0) return;
        stopCapture();
        draft().pad[selectedTarget_] = PadBinding{};
        dirty_.insert(draftKey());
        refreshPadCell(selectedTarget_);
        setFeedback(QStringLiteral("Назначение кнопки очищено."));
        updateActionButtons();
    });
    connect(defaultsButton, &QPushButton::clicked, this, [this] {
        stopCapture();
        draft() = InputManager::defaultMapping(selectedProfile(), selectedPlayer());
        dirty_.insert(draftKey());
        refreshContents();
        setFeedback(QStringLiteral("Настройки этого игрока и системы сброшены к исходным."));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &ControlsDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        stopCapture();
        reject();
    });

    connect(&input_, &InputManager::devicesChanged,
            this, &ControlsDialog::refreshDevices);
    connect(&input_, &InputManager::captureReady, this, [this] {
        if (captureTarget_ < 0) return;
        const QString label = captureTarget_ < 16
            ? InputManager::targetName(selectedProfile(), captureTarget_, draft().controllerType)
            : commandName(captureTarget_);
        setFeedback(QStringLiteral("Назначение «%1»: нажмите кнопку, крестовину "
                                   "или отклоните ось. Esc — отмена.").arg(label));
    });
    connect(&input_, &InputManager::bindingCaptured,
            this, &ControlsDialog::onCaptured);
    connect(&input_, &InputManager::captureCancelled,
            this, &ControlsDialog::onCaptureCancelled);
    connect(&input_, &InputManager::previewChanged, this,
            [this](unsigned player, unsigned mask) {
                if (int(player) == selectedPlayer() &&
                    selectedProfile() == input_.profile())
                    diagram_->setPressedMask(mask);
            });
    connect(&input_, &InputManager::notification, this,
            [this](const QString& message) {
                if (captureTarget_ >= 0)
                    setFeedback(message);
            });

    refreshContents();
    setFeedback(QStringLiteral(
        "Выберите кнопку на рисунке или в таблице, затем нажмите «Назначить». "
        "«Авто» использует стандартную раскладку подключённого геймпада."));
}

ConsoleProfile ControlsDialog::selectedProfile() const {
    return static_cast<ConsoleProfile>(profileBox_->currentData().toInt());
}

int ControlsDialog::selectedPlayer() const {
    return playerBox_->currentData().toInt();
}

int ControlsDialog::draftKey() const {
    return static_cast<int>(selectedProfile()) * 2 + selectedPlayer();
}

PlayerMapping& ControlsDialog::draft() {
    const int key = draftKey();
    if (!drafts_.contains(key))
        drafts_.insert(key, input_.mapping(selectedProfile(), selectedPlayer()));
    return drafts_[key];
}

QVector<int> ControlsDialog::activeGameTargets() {
    const auto& mapping = draft();
    return InputManager::gameTargets(selectedProfile(), mapping.sixButton, mapping.controllerType);
}

QString ControlsDialog::captureDeviceKey() {
    const QString key = draft().deviceKey;
    if (key != QStringLiteral("auto"))
        return key;
    const auto devices = input_.devices();
    if (devices.isEmpty())
        return key;
    const int otherPlayer = 1 - selectedPlayer();
    const int otherKey = static_cast<int>(selectedProfile()) * 2 + otherPlayer;
    const PlayerMapping other = drafts_.contains(otherKey)
        ? drafts_.value(otherKey) : input_.mapping(selectedProfile(), otherPlayer);
    QString reserved = other.deviceKey;
    if (reserved == QStringLiteral("auto"))
        reserved = selectedPlayer() == 1 ? devices.first().key : QString();
    if (reserved == QStringLiteral("none"))
        reserved.clear();
    for (const InputDevice& device : devices) {
        if (device.key != reserved)
            return device.key;
    }
    return key;
}

void ControlsDialog::refreshDevices() {
    if (!deviceBox_) return;
    const QString currentKey = draft().deviceKey;
    const QSignalBlocker blocker(deviceBox_);
    deviceBox_->clear();
    deviceBox_->addItem(QStringLiteral("Автоматически"), QStringLiteral("auto"));
    deviceBox_->addItem(QStringLiteral("Только клавиатура"), QStringLiteral("none"));
    const auto devices = input_.devices();
    for (const InputDevice& device : devices) {
        knownDevices_.insert(device.key, device.name);
        deviceBox_->addItem(device.name, device.key);
        deviceBox_->setItemData(deviceBox_->count() - 1,
                                device.key, Qt::ToolTipRole);
    }
    int index = deviceBox_->findData(currentKey);
    if (index < 0) {
        const QString name = knownDevices_.value(currentKey, currentKey);
        deviceBox_->addItem(
            QStringLiteral("Отключено: %1").arg(name), currentKey);
        deviceBox_->setItemData(deviceBox_->count() - 1,
                                currentKey, Qt::ToolTipRole);
        index = deviceBox_->count() - 1;
    }
    deviceBox_->setCurrentIndex(index);
    updateActionButtons();
}

void ControlsDialog::refreshContents() {
    refreshing_ = true;
    const PlayerMapping mapping = draft();
    connectedBox_->setChecked(mapping.connected);
    const bool md = selectedProfile() == ConsoleProfile::MegaDrive;
    const bool sms = selectedProfile() == ConsoleProfile::MasterSystem;
    const bool gun = mapping.controllerType != ControllerType::Pad;
    controllerTypeLabel_->setVisible(md && !gun);
    controllerTypeBox_->setVisible(md && !gun);
    controllerTypeBox_->setCurrentIndex(mapping.sixButton ? 1 : 0);
    deviceTypeLabel_->setVisible(md || sms);
    deviceTypeBox_->setVisible(md || sms);
    deviceTypeBox_->clear();
    deviceTypeBox_->addItem(QStringLiteral("Геймпад"), int(ControllerType::Pad));
    if (md) {
        deviceTypeBox_->addItem(QStringLiteral("Menacer"), int(ControllerType::Menacer));
        deviceTypeBox_->addItem(QStringLiteral("Justifier"), int(ControllerType::Justifier));
    } else if (sms) {
        deviceTypeBox_->addItem(QStringLiteral("Light Phaser"), int(ControllerType::LightPhaser));
    }
    deviceTypeBox_->setCurrentIndex(deviceTypeBox_->findData(int(mapping.controllerType)));
    aimSourceLabel_->setVisible(gun);
    aimSourceBox_->setVisible(gun);
    aimSourceBox_->setCurrentIndex(aimSourceBox_->findData(int(mapping.aimSource)));
    aimHint_->setVisible(gun);
    refreshDevices();
    deadzoneSlider_->setValue(qBound(5, mapping.deadzonePercent, 60));
    deadzoneValue_->setText(
        QStringLiteral("%1 %").arg(deadzoneSlider_->value()));
    diagram_->setProfile(selectedProfile());
    diagram_->setSixButton(mapping.sixButton);
    diagram_->setControllerType(mapping.controllerType);
    diagram_->setTargets(activeGameTargets());
    fillGameTable();
    fillCommandTable();
    refreshing_ = false;
    const auto& targets = tabs_->currentIndex() == 0
        ? activeGameTargets() : commands;
    selectTarget(targets.isEmpty() ? -1 : targets.first());
}

void ControlsDialog::fillGameTable() {
    const QVector<int> targets = activeGameTargets();
    const PlayerMapping mapping = draft();
    gameTable_->clearContents();
    gameTable_->setRowCount(targets.size());
    for (int row = 0; row < targets.size(); ++row) {
        const int target = targets[row];
        gameTable_->setRowHeight(row, 34);
        gameTable_->setItem(row, 0,
            cell(InputManager::targetName(selectedProfile(), target, mapping.controllerType), target));
        auto* keyEdit = new PhysicalKeySequenceEdit(gameTable_);
        keyEdit->setToolTip(QStringLiteral("Клавиша по положению, независимо от раскладки. 0–9 — выбор слота; точка — следующий кадр."));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        keyEdit->setMaximumSequenceLength(1);
#endif
        const int key = mapping.keyboard.value(target, 0);
        if (key)
            keyEdit->setKeySequence(QKeySequence(key));
        auto* keyCell = new QWidget(gameTable_);
        auto* keyLayout = new QHBoxLayout(keyCell);
        keyLayout->setContentsMargins(2, 1, 2, 1);
        keyLayout->setSpacing(2);
        keyLayout->addWidget(keyEdit, 1);
        auto* clearKey = new QToolButton(keyCell);
        clearKey->setText(QStringLiteral("×"));
        clearKey->setToolTip(QStringLiteral("Очистить клавишу"));
        clearKey->setFixedWidth(22);
        keyLayout->addWidget(clearKey);
        gameTable_->setCellWidget(row, 1, keyCell);
        connect(keyEdit, &QKeySequenceEdit::keySequenceChanged,
                this, [this, target](const QKeySequence& sequence) {
                    onKeyboardChanged(target, sequence);
                });
        connect(clearKey, &QToolButton::clicked, this, [this, keyEdit, target] {
            const QSignalBlocker blocker(keyEdit);
            keyEdit->clear();
            onKeyboardChanged(target, QKeySequence());
        });
        gameTable_->setItem(row, 2, cell(padName(mapping, target)));
    }
}

void ControlsDialog::fillCommandTable() {
    const PlayerMapping mapping = draft();
    commandTable_->clearContents();
    commandTable_->setRowCount(commands.size());
    for (int row = 0; row < commands.size(); ++row) {
        const int target = commands[row];
        commandTable_->setRowHeight(row, 34);
        commandTable_->setItem(row, 0, cell(commandName(target), target));
        commandTable_->setItem(row, 1, cell(commandShortcut(target)));
        commandTable_->setItem(row, 2, cell(padName(mapping, target)));
    }
}

void ControlsDialog::selectTarget(int target) {
    selectedTarget_ = target;
    if (target >= 0 && target < 16)
        diagram_->setSelectedTarget(target);
    else
        diagram_->setSelectedTarget(-1);
    QTableWidget* table = tabs_->currentIndex() == 0 ? gameTable_ : commandTable_;
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto* item = table->item(row, 0);
        if (item && item->data(Qt::UserRole).toInt() == target) {
            if (table->currentRow() != row)
                table->selectRow(row);
            break;
        }
    }
    updateActionButtons();
}

void ControlsDialog::refreshPadCell(int target) {
    QTableWidget* table = target < 16 ? gameTable_ : commandTable_;
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto* item = table->item(row, 0);
        if (item && item->data(Qt::UserRole).toInt() == target) {
            table->item(row, 2)->setText(padName(draft(), target));
            return;
        }
    }
}

void ControlsDialog::updateActionButtons() {
    if (!assignButton_) return;
    const bool connected = draft().connected;
    gameTable_->setEnabled(connected);
    diagram_->setEnabled(connected);
    aimSourceBox_->setEnabled(connected);
    const bool gamePage = tabs_->currentIndex() == 0;
    const bool targetSelected = selectedTarget_ >= 0 && (!gamePage || connected);
    const bool deviceSelected = draft().deviceKey != QStringLiteral("none");
    assignButton_->setEnabled(targetSelected && deviceSelected);
    sequenceButton_->setEnabled(deviceSelected && (!gamePage || connected));
    skipButton_->setEnabled(captureTarget_ >= 0 && !sequence_.isEmpty());
    clearButton_->setEnabled(targetSelected);
}

void ControlsDialog::setFeedback(const QString& message, bool warning) {
    feedback_->setText(message);
    const QColor color = warning
        ? (palette().color(QPalette::Base).lightness() < 128
               ? QColor(245, 144, 122) : QColor(164, 48, 40))
        : palette().color(QPalette::Text);
    feedback_->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
}

void ControlsDialog::onKeyboardChanged(int target, const QKeySequence& sequence) {
    if (refreshing_) return;
    int key = 0;
    const Qt::KeyboardModifiers modifiers =
        sequence.isEmpty() ? Qt::NoModifier : sequence[0].keyboardModifiers();
    if (!sequence.isEmpty())
        key = sequence[0].key();
    QString error;
    if (sequence.count() > 1 || modifiers != Qt::NoModifier)
        error = QStringLiteral("Назначьте одну клавишу без Ctrl, Alt или Shift.");
    else if (reservedKey(key))
        error = QStringLiteral("0–9 выбирают слот; Tab, Esc, P, M, R, F5, F8, F12 и точка зарезервированы.");
    else if (key) {
        const PlayerMapping& mapping = draft();
        const auto targets = activeGameTargets();
        for (auto it = mapping.keyboard.cbegin(); it != mapping.keyboard.cend(); ++it) {
            if (it.key() != target && targets.contains(it.key()) && it.value() == key) {
                error = QStringLiteral("Эта клавиша уже назначена другой кнопке.");
                break;
            }
        }
    }
    if (!error.isEmpty()) {
        auto* editor = qobject_cast<QKeySequenceEdit*>(sender());
        if (editor) {
            const QSignalBlocker blocker(editor);
            const int oldKey = draft().keyboard.value(target, 0);
            editor->setKeySequence(oldKey ? QKeySequence(oldKey) : QKeySequence());
        }
        setFeedback(error, true);
        return;
    }
    draft().keyboard[target] = key;
    dirty_.insert(draftKey());
    setFeedback(key
        ? QStringLiteral("Клавиша назначена.")
        : QStringLiteral("Клавиша очищена."));
}

void ControlsDialog::beginCaptureFor(int target) {
    if (target < 16 && (!draft().connected || !activeGameTargets().contains(target))) return;
    if (draft().deviceKey == QStringLiteral("none")) {
        setFeedback(QStringLiteral("Сначала выберите устройство."), true);
        return;
    }
    const QString deviceKey = captureDeviceKey();
    bool connected = false;
    for (const InputDevice& device : input_.devices())
        connected |= device.key == deviceKey;
    if (!connected) {
        captureTarget_ = -1;
        sequence_.clear();
        setFeedback(QStringLiteral(
            "Геймпад не подключён или занят другим игроком. "
            "Подключите устройство либо выберите доступное в списке."), true);
        updateActionButtons();
        return;
    }
    captureTarget_ = target;
    selectTarget(target);
    const QString label = target < 16
        ? InputManager::targetName(selectedProfile(), target, draft().controllerType)
        : commandName(target);
    setFeedback(QStringLiteral(
        "Назначение «%1»: отпустите кнопки и верните стики в центр. "
        "Ожидание нейтрального положения… Esc — отмена.").arg(label));
    input_.beginCapture(deviceKey, selectedPlayer());
    updateActionButtons();
}

void ControlsDialog::startSequence() {
    stopCapture();
    sequence_ = tabs_->currentIndex() == 0
        ? activeGameTargets() : commands;
    sequenceIndex_ = 0;
    if (!sequence_.isEmpty())
        beginCaptureFor(sequence_.first());
}

void ControlsDialog::advanceSequence() {
    if (sequence_.isEmpty()) return;
    ++sequenceIndex_;
    if (sequenceIndex_ >= sequence_.size()) {
        sequence_.clear();
        captureTarget_ = -1;
        setFeedback(QStringLiteral("Последовательное назначение завершено."));
        updateActionButtons();
        return;
    }
    beginCaptureFor(sequence_[sequenceIndex_]);
}

void ControlsDialog::skipCapture() {
    if (captureTarget_ < 0 || sequence_.isEmpty()) return;
    skipping_ = true;
    captureTarget_ = -1;
    input_.cancelCapture();
    QTimer::singleShot(0, this, [this] {
        if (skipping_)
            onCaptureCancelled();
    });
}

void ControlsDialog::stopCapture() {
    captureTarget_ = -1;
    sequence_.clear();
    sequenceIndex_ = 0;
    skipping_ = false;
    if (input_.capturing())
        input_.cancelCapture();
    updateActionButtons();
}

void ControlsDialog::onCaptured(const PadBinding& binding) {
    if (captureTarget_ < 0) return;
    const int target = captureTarget_;
    captureTarget_ = -1;
    const PlayerMapping& current = draft();
    const auto targets = activeGameTargets();
    for (auto it = current.pad.cbegin(); it != current.pad.cend(); ++it) {
        if ((it.key() >= 16 || targets.contains(it.key())) &&
            it.key() != target && it.value().kind != PadBinding::None &&
            it.value().kind != PadBinding::Automatic &&
            it.value() == binding) {
            setFeedback(QStringLiteral("Этот элемент геймпада уже назначен. "
                                       "Выберите другой или пропустите."), true);
            if (!sequence_.isEmpty()) {
                QTimer::singleShot(0, this, [this, target] {
                    if (!sequence_.isEmpty())
                        beginCaptureFor(target);
                });
            }
            updateActionButtons();
            return;
        }
    }
    draft().pad[target] = binding;
    dirty_.insert(draftKey());
    refreshPadCell(target);
    if (sequence_.isEmpty()) {
        setFeedback(QStringLiteral("Назначено: %1").arg(
            InputManager::bindingName(binding)));
        updateActionButtons();
    } else {
        QTimer::singleShot(0, this, &ControlsDialog::advanceSequence);
    }
}

void ControlsDialog::onCaptureCancelled() {
    if (skipping_) {
        skipping_ = false;
        QTimer::singleShot(0, this, &ControlsDialog::advanceSequence);
        return;
    }
    if (captureTarget_ >= 0) {
        captureTarget_ = -1;
        sequence_.clear();
        setFeedback(QStringLiteral("Назначение отменено или время ожидания истекло."));
        updateActionButtons();
    }
}

bool ControlsDialog::validateDrafts() {
    // Validate both logical players together, including an unchanged player's
    // stored mapping. Hardware topology is applied by the emulation host.
    for (int profile = 0; profile < 4; ++profile) {
        if (!dirty_.contains(profile * 2) && !dirty_.contains(profile * 2 + 1)) continue;
        PlayerMapping players[2];
        for (int player = 0; player < 2; ++player) {
            const int key = profile * 2 + player;
            players[player] = drafts_.contains(key) ? drafts_.value(key)
                : input_.mapping(static_cast<ConsoleProfile>(profile), player);
        }
        const bool firstGun = players[0].connected && players[0].controllerType != ControllerType::Pad;
        const bool secondGun = players[1].connected && players[1].controllerType != ControllerType::Pad;
        if (!firstGun || !secondGun) continue;
        if (profile == int(ConsoleProfile::MegaDrive) &&
            (players[0].controllerType != players[1].controllerType ||
             players[0].controllerType == ControllerType::Menacer)) {
            setFeedback(QStringLiteral("Mega Drive поддерживает один Menacer или два Justifier. "
                                       "Для двух пистолетов выберите Justifier у обоих игроков."), true);
            return false;
        }
        if (players[0].aimSource == AimSource::Mouse && players[1].aimSource == AimSource::Mouse) {
            setFeedback(QStringLiteral("Одна мышь управляет одним пистолетом. "
                                       "Для другого игрока выберите левый стик."), true);
            return false;
        }
    }
    for (int key : dirty_) {
        const PlayerMapping& mapping = drafts_[key];
        const auto targets = InputManager::gameTargets(static_cast<ConsoleProfile>(key / 2), mapping.sixButton, mapping.controllerType);
        QSet<int> keyboard;
        for (auto it = mapping.keyboard.cbegin(); it != mapping.keyboard.cend(); ++it) {
            const int assigned = it.value();
            if (!targets.contains(it.key()) || !assigned) continue;
            if (reservedKey(assigned) || keyboard.contains(assigned)) {
                setFeedback(QStringLiteral("Проверьте клавиши: есть повтор или "
                                           "зарезервированная клавиша."), true);
                return false;
            }
            keyboard.insert(assigned);
        }
        QVector<PadBinding> pad;
        for (auto it = mapping.pad.cbegin(); it != mapping.pad.cend(); ++it) {
            if (it.key() < 16 && !targets.contains(it.key())) continue;
            const PadBinding& assigned = it.value();
            if (assigned.kind == PadBinding::None ||
                assigned.kind == PadBinding::Automatic) continue;
            for (const PadBinding& previous : pad) {
                if (assigned == previous) {
                    setFeedback(QStringLiteral("Проверьте геймпад: один элемент "
                                               "назначен двум действиям."), true);
                    return false;
                }
            }
            pad.append(assigned);
        }
    }
    return true;
}

void ControlsDialog::save() {
    if (captureTarget_ >= 0 || !sequence_.isEmpty() || input_.capturing()) {
        setFeedback(QStringLiteral("Завершите или отмените текущее назначение."), true);
        return;
    }
    if (!validateDrafts())
        return;
    for (int key : dirty_)
        input_.setMapping(static_cast<ConsoleProfile>(key / 2),
                          key % 2, drafts_[key]);
    accept();
}

void ControlsDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && captureTarget_ >= 0) {
        stopCapture();
        setFeedback(QStringLiteral("Назначение отменено."));
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void ControlsDialog::closeEvent(QCloseEvent* event) {
    stopCapture();
    QDialog::closeEvent(event);
}
