#include "ControlsPage.h"

#include "ControllerDiagram.h"

#include <QHideEvent>
#include <QApplication>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
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
    case ResetCommand: return QStringLiteral("Сброс");
    case HardResetCommand: return QStringLiteral("Перезапуск питания");
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

ControlsPage::ControlsPage(InputManager& input, QWidget* parent)
    : QWidget(parent), input_(input) {
    setFocusPolicy(Qt::StrongFocus);
    qApp->installEventFilter(this);

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
    playerBox_->addItem(QStringLiteral("Порт 1"), 0);
    playerBox_->addItem(QStringLiteral("Порт 2"), 1);
    deviceBox_ = new QComboBox(this);
    deviceBox_->setMinimumWidth(230);
    deadzoneSlider_ = new QSlider(Qt::Horizontal, this);
    deadzoneSlider_->setRange(5, 60);
    deadzoneSlider_->setMinimumWidth(140);
    deadzoneValue_ = new QLabel(this);
    deadzoneValue_->setMinimumWidth(35);

    scope->addWidget(new QLabel(QStringLiteral("Система"), this), 0, 0);
    scope->addWidget(profileBox_, 1, 0);
    scope->addWidget(new QLabel(QStringLiteral("Порт консоли"), this), 0, 1);
    scope->addWidget(playerBox_, 1, 1);
    scope->addWidget(new QLabel(QStringLiteral("Геймпад"), this), 2, 0);
    scope->addWidget(deviceBox_, 3, 0);
    scope->addWidget(new QLabel(QStringLiteral("Мёртвая зона"), this), 2, 1);
    auto* deadzoneRow = new QHBoxLayout;
    deadzoneRow->setContentsMargins(0, 0, 0, 0);
    deadzoneRow->addWidget(deadzoneSlider_);
    deadzoneRow->addWidget(deadzoneValue_);
    scope->addLayout(deadzoneRow, 3, 1);
    scope->setColumnStretch(0, 1);
    scope->setColumnStretch(1, 1);
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
    scope->addLayout(controllerTypeRow, 5, 1);
    deviceTypeLabel_ = new QLabel(QStringLiteral("Контроллер"), this);
    deviceTypeBox_ = new QComboBox(this);
    deviceTypeBox_->setObjectName(QStringLiteral("deviceTypeBox"));
    deviceTypeBox_->setToolTip(QStringLiteral("Контроллер, который видит игра. «Нет» отключает порт, сохраняя назначения."));
    scope->addWidget(deviceTypeLabel_, 4, 0);
    scope->addWidget(deviceTypeBox_, 5, 0);
    secondJustifierBox_ = new QCheckBox(QStringLiteral("Второй пистолет"), this);
    secondJustifierBox_->setObjectName(QStringLiteral("secondJustifierBox"));
    secondJustifierBox_->setToolTip(QStringLiteral("Второй Justifier в паре на порту 2; назначения настраиваются отдельно."));
    scope->addWidget(secondJustifierBox_, 6, 0);
    aimSourceLabel_ = new QLabel(QStringLiteral("Прицеливание"), this);
    aimSourceBox_ = new QComboBox(this);
    aimSourceBox_->setObjectName(QStringLiteral("aimSourceBox"));
    aimSourceBox_->addItem(QStringLiteral("Мышь"), int(AimSource::Mouse));
    aimSourceBox_->addItem(QStringLiteral("Левый стик"), int(AimSource::LeftStick));
    auto* aimRow = new QHBoxLayout;
    aimRow->addWidget(aimSourceLabel_);
    aimRow->addWidget(aimSourceBox_);
    aimRow->addStretch(1);
    scope->addLayout(aimRow, 6, 1);
    showCrosshairBox_ = new QCheckBox(QStringLiteral("Показывать прицел"), this);
    showCrosshairBox_->setObjectName(QStringLiteral("showCrosshairBox"));
    scope->addWidget(showCrosshairBox_, 7, 0, 1, 2);
    aimHint_ = new QLabel(QStringLiteral(
        "Мышь доступна одному пистолету; для второго выберите левый стик. "
        "Прицелы: первый пистолет — голубой, второй — оранжевый."), this);
    aimHint_->setWordWrap(true);
    scope->addWidget(aimHint_, 8, 0, 1, 2);
    mainLayout->addLayout(scope);
    portHint_ = new QLabel(QStringLiteral(
        "Порт отключён. Назначения можно настроить заранее; для игры выберите контроллер вместо «Нет»."), this);
    portHint_->setWordWrap(true);
    mainLayout->addWidget(portHint_);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("mappingTabs"));
    auto* gamePage = new QWidget(tabs_);
    auto* gameLayout = new QHBoxLayout(gamePage);
    gameLayout->setContentsMargins(7, 8, 7, 7);
    gameLayout->setSpacing(10);
    diagram_ = new ControllerDiagram(gamePage);
    gameLayout->addWidget(diagram_, 4);
    gameTable_ = new QTableWidget(gamePage);
    gameTable_->setColumnCount(4);
    gameTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Кнопка"), QStringLiteral("Клавиатура"),
         QStringLiteral("Геймпад"), QStringLiteral("Мышь")});
    gameTable_->verticalHeader()->hide();
    gameTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    gameTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    gameTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    gameTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    gameTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    gameTable_->horizontalHeader()->resizeSection(1, 155);
    gameTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    gameTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    gameTable_->horizontalHeader()->resizeSection(3, 112);
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
    assignButton_->setObjectName(QStringLiteral("assignBinding"));
    sequenceButton_ = new QPushButton(QStringLiteral("Назначить по порядку"), this);
    sequenceButton_->setObjectName(QStringLiteral("assignSequence"));
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
        const int type = deviceTypeBox_->currentData().toInt();
        if (type == -1) draft().connected = false;
        else {
            draft().controllerType = static_cast<ControllerType>(type);
            draft().connected = true;
        }
        dirty_.insert(draftKey());
        refreshContents();
    });
    connect(secondJustifierBox_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (refreshing_ || selectedProfile() != ConsoleProfile::MegaDrive || selectedPlayer() != 1) return;
        stopCapture();
        const int key = int(ConsoleProfile::MegaDrive) * 3 + 2;
        if (!drafts_.contains(key)) drafts_.insert(key, input_.mapping(ConsoleProfile::MegaDrive, 2));
        drafts_[key].connected = enabled;
        drafts_[key].controllerType = ControllerType::Justifier;
        dirty_.insert(key);
        refreshContents();
    });
    connect(showCrosshairBox_, &QCheckBox::toggled, this, [this](bool visible) {
        if (refreshing_) return;
        draft().showCrosshair = visible;
        dirty_.insert(draftKey());
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
            this, &ControlsPage::selectTarget);
    connect(assignButton_, &QPushButton::clicked, this, [this] {
        stopCapture();
        if (selectedTarget_ >= 0)
            beginCaptureFor(selectedTarget_);
    });
    connect(sequenceButton_, &QPushButton::clicked,
            this, &ControlsPage::startSequence);
    connect(skipButton_, &QPushButton::clicked,
            this, &ControlsPage::skipCapture);
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
    connect(&input_, &InputManager::devicesChanged,
            this, &ControlsPage::refreshDevices);
    connect(&input_, &InputManager::captureReady, this, [this] {
        if (captureTarget_ < 0) return;
        const QString label = captureTarget_ < 16
            ? InputManager::targetName(selectedProfile(), captureTarget_, draft().controllerType)
            : commandName(captureTarget_);
        setFeedback((captureTarget_ < 16
            ? QStringLiteral("Назначение «%1»: нажмите клавишу или кнопку геймпада. Esc — отмена.")
            : QStringLiteral("Назначение «%1»: нажмите кнопку геймпада. Esc — отмена.")).arg(label));
    });
    connect(&input_, &InputManager::bindingCaptured,
            this, &ControlsPage::onCaptured);
    connect(&input_, &InputManager::captureCancelled,
            this, &ControlsPage::onCaptureCancelled);
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
        "Выберите кнопку и нажмите «Назначить», затем клавишу или кнопку геймпада. "
        "Клавиатура и геймпад работают одновременно; одно назначение не стирает другое."));
}

ControlsPage::~ControlsPage() {
    qApp->removeEventFilter(this);
    stopCapture();
}

void ControlsPage::cancelCapture() {
    stopCapture();
}

ConsoleProfile ControlsPage::selectedProfile() const {
    return static_cast<ConsoleProfile>(profileBox_->currentData().toInt());
}

int ControlsPage::selectedPlayer() const {
    return playerBox_->currentData().toInt();
}

int ControlsPage::draftKey() const {
    return static_cast<int>(selectedProfile()) * 3 + selectedPlayer();
}

PlayerMapping& ControlsPage::draft() {
    const int key = draftKey();
    if (!drafts_.contains(key))
        drafts_.insert(key, input_.mapping(selectedProfile(), selectedPlayer()));
    return drafts_[key];
}

QVector<int> ControlsPage::activeGameTargets() {
    const auto& mapping = draft();
    return InputManager::gameTargets(selectedProfile(), mapping.sixButton, mapping.controllerType);
}

void ControlsPage::refreshPlayerChoices() {
    const int current = selectedPlayer();
    const int secondaryKey = int(selectedProfile()) * 3 + 2;
    const int parentKey = int(selectedProfile()) * 3 + 1;
    const PlayerMapping parent = drafts_.contains(parentKey) ? drafts_.value(parentKey)
        : input_.mapping(selectedProfile(), 1);
    const PlayerMapping secondary = drafts_.contains(secondaryKey) ? drafts_.value(secondaryKey)
        : input_.mapping(selectedProfile(), 2);
    const bool pair = selectedProfile() == ConsoleProfile::MegaDrive &&
        parent.connected && parent.controllerType == ControllerType::Justifier && secondary.connected;
    const QSignalBlocker blocker(playerBox_);
    playerBox_->clear();
    playerBox_->addItem(QStringLiteral("Порт 1"), 0);
    playerBox_->addItem(QStringLiteral("Порт 2"), 1);
    if (pair) playerBox_->addItem(QStringLiteral("Второй пистолет"), 2);
    playerBox_->setCurrentIndex(playerBox_->findData(current == 2 && !pair ? 1 : current));
}

QString ControlsPage::captureDeviceKey() {
    const QString key = draft().deviceKey;
    if (key != QStringLiteral("auto"))
        return key;
    const auto devices = input_.devices();
    if (devices.isEmpty())
        return key;
    QVector<QString> assigned(3);
    QVector<PlayerMapping> players;
    for (int player = 0; player < 3; ++player) {
        const int mappingKey = int(selectedProfile()) * 3 + player;
        players.append(drafts_.contains(mappingKey) ? drafts_.value(mappingKey)
            : input_.mapping(selectedProfile(), player));
        if (players[player].deviceKey != QStringLiteral("auto") &&
            players[player].deviceKey != QStringLiteral("none"))
            assigned[player] = players[player].deviceKey;
    }
    const bool chainedActive = selectedProfile() == ConsoleProfile::MegaDrive &&
        players[1].connected && players[1].controllerType == ControllerType::Justifier &&
        players[2].connected;
    const auto active = [&players, chainedActive](int player) {
        return player == 2 ? chainedActive : players[player].connected;
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (int player = 0; player < 3; ++player) {
            if (players[player].deviceKey != QStringLiteral("auto") ||
                active(player) != (pass == 0)) continue;
            for (const InputDevice& device : devices) {
                bool usedByActive = false;
                for (int other = 0; other < 3; ++other)
                    usedByActive |= active(other) && assigned[other] == device.key;
                if (!usedByActive) { assigned[player] = device.key; break; }
            }
            if (assigned[player].isEmpty()) assigned[player] = devices.first().key;
        }
    }
    return assigned[selectedPlayer()];
}

void ControlsPage::refreshDevices() {
    if (!deviceBox_) return;
    const QString currentKey = draft().deviceKey;
    const QSignalBlocker blocker(deviceBox_);
    deviceBox_->clear();
    deviceBox_->addItem(QStringLiteral("Автоматически"), QStringLiteral("auto"));
    deviceBox_->addItem(QStringLiteral("Без геймпада"), QStringLiteral("none"));
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

void ControlsPage::refreshContents() {
    refreshing_ = true;
    refreshPlayerChoices();
    const PlayerMapping mapping = draft();
    portHint_->setVisible(!mapping.connected);
    const bool md = selectedProfile() == ConsoleProfile::MegaDrive;
    const bool sms = selectedProfile() == ConsoleProfile::MasterSystem;
    const bool gun = mapping.controllerType != ControllerType::Pad;
    const bool chained = md && selectedPlayer() == 2;
    controllerTypeLabel_->setVisible(mapping.connected && md && !gun && !chained);
    controllerTypeBox_->setVisible(mapping.connected && md && !gun && !chained);
    controllerTypeBox_->setCurrentIndex(mapping.sixButton ? 1 : 0);
    deviceTypeLabel_->setVisible(!chained);
    deviceTypeBox_->setVisible(!chained);
    deviceTypeBox_->clear();
    deviceTypeBox_->addItem(QStringLiteral("Нет"), -1);
    deviceTypeBox_->addItem(QStringLiteral("Геймпад"), int(ControllerType::Pad));
    if (md && selectedPlayer() == 1) {
        deviceTypeBox_->addItem(QStringLiteral("Menacer"), int(ControllerType::Menacer));
        deviceTypeBox_->addItem(QStringLiteral("Justifier"), int(ControllerType::Justifier));
    } else if (sms) {
        deviceTypeBox_->addItem(QStringLiteral("Light Phaser"), int(ControllerType::LightPhaser));
    }
    deviceTypeBox_->setCurrentIndex(deviceTypeBox_->findData(
        mapping.connected ? int(mapping.controllerType) : -1));
    const int secondaryKey = int(ConsoleProfile::MegaDrive) * 3 + 2;
    const bool secondGun = drafts_.contains(secondaryKey) ? drafts_.value(secondaryKey).connected
        : input_.mapping(ConsoleProfile::MegaDrive, 2).connected;
    secondJustifierBox_->setVisible(md && selectedPlayer() == 1 && mapping.connected &&
                                    mapping.controllerType == ControllerType::Justifier);
    secondJustifierBox_->setChecked(secondGun);
    aimSourceLabel_->setVisible(mapping.connected && gun);
    aimSourceBox_->setVisible(mapping.connected && gun);
    aimSourceBox_->setCurrentIndex(aimSourceBox_->findData(int(mapping.aimSource)));
    aimHint_->setText(QStringLiteral("Мышь доступна одному пистолету; для второго выберите левый стик. ") +
        (md ? QStringLiteral("Пистолет подключается ко второму порту консоли. Для меню T2 и Lethal Enforcers нужен геймпад в первом порту. ") : QString()) +
        QStringLiteral("Прицелы: первый пистолет — голубой, второй — оранжевый."));
    aimHint_->setVisible(mapping.connected && gun);
    showCrosshairBox_->setVisible(mapping.connected && gun);
    showCrosshairBox_->setChecked(mapping.showCrosshair);
    gameTable_->setColumnHidden(3, !gun);
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

void ControlsPage::fillGameTable() {
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
        if (auto* line = keyEdit->findChild<QLineEdit*>())
            line->setPlaceholderText(QStringLiteral("Нажмите клавишу"));
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
        if (mapping.controllerType != ControllerType::Pad) {
            auto* mouseBox = new QComboBox(gameTable_);
            mouseBox->addItem(QStringLiteral("—"), 0);
            mouseBox->addItem(QStringLiteral("Левая"), int(Qt::LeftButton));
            mouseBox->addItem(QStringLiteral("Правая"), int(Qt::RightButton));
            mouseBox->addItem(QStringLiteral("Средняя"), int(Qt::MiddleButton));
            mouseBox->addItem(QStringLiteral("Боковая 1"), int(Qt::XButton1));
            mouseBox->addItem(QStringLiteral("Боковая 2"), int(Qt::XButton2));
            const int type = int(mapping.controllerType);
            mouseBox->setCurrentIndex(mouseBox->findData(mapping.mouse.value(type).value(target, 0)));
            gameTable_->setCellWidget(row, 3, mouseBox);
            connect(mouseBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [this, target, type, mouseBox] {
                        if (refreshing_) return;
                        draft().mouse[type][target] = mouseBox->currentData().toInt();
                        dirty_.insert(draftKey());
                    });
        }
    }
}

void ControlsPage::fillCommandTable() {
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

void ControlsPage::selectTarget(int target) {
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

void ControlsPage::refreshPadCell(int target) {
    QTableWidget* table = target < 16 ? gameTable_ : commandTable_;
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto* item = table->item(row, 0);
        if (item && item->data(Qt::UserRole).toInt() == target) {
            table->item(row, 2)->setText(padName(draft(), target));
            return;
        }
    }
}

void ControlsPage::updateActionButtons() {
    if (!assignButton_) return;
    const bool gamePage = tabs_->currentIndex() == 0;
    const bool targetSelected = selectedTarget_ >= 0;
    const bool deviceSelected = draft().deviceKey != QStringLiteral("none");
    // Keyboard setup does not require a physical pad or an enabled console port.
    assignButton_->setEnabled(targetSelected && (gamePage || deviceSelected));
    sequenceButton_->setEnabled(gamePage || deviceSelected);
    skipButton_->setEnabled(captureTarget_ >= 0 && !sequence_.isEmpty());
    clearButton_->setEnabled(targetSelected);
}

void ControlsPage::setFeedback(const QString& message, bool warning) {
    feedback_->setText(message);
    const QColor color = warning
        ? (palette().color(QPalette::Base).lightness() < 128
               ? QColor(245, 144, 122) : QColor(164, 48, 40))
        : palette().color(QPalette::Text);
    feedback_->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
}

bool ControlsPage::onKeyboardChanged(int target, const QKeySequence& sequence) {
    if (refreshing_) return false;
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
        return false;
    }
    draft().keyboard[target] = key;
    dirty_.insert(draftKey());
    setFeedback(key
        ? QStringLiteral("Клавиша назначена.")
        : QStringLiteral("Клавиша очищена."));
    return true;
}

void ControlsPage::beginCaptureFor(int target) {
    if (target < 16 && !activeGameTargets().contains(target)) return;
    const QString deviceKey = captureDeviceKey();
    bool padPresent = false;
    for (const InputDevice& device : input_.devices())
        padPresent |= device.key == deviceKey;
    if (target >= 16 && !padPresent) {
        stopCapture();
        setFeedback(QStringLiteral("Для назначения команды подключите геймпад. Сочетания клавиатуры указаны в таблице."), true);
        return;
    }
    captureTarget_ = target;
    selectTarget(target);
    setFocus(Qt::OtherFocusReason);
    const QString label = target < 16
        ? InputManager::targetName(selectedProfile(), target, draft().controllerType)
        : commandName(target);
    setFeedback((target < 16
        ? QStringLiteral("Назначение «%1»: нажмите клавишу или кнопку геймпада. Перед назначением геймпада отпустите его кнопки и стики. Esc — отмена.")
        : QStringLiteral("Назначение «%1»: отпустите кнопки и стики геймпада, затем нажмите нужную кнопку. Esc — отмена.")).arg(label));
    if (padPresent)
        input_.beginCapture(deviceKey, selectedPlayer());
    updateActionButtons();
}

void ControlsPage::startSequence() {
    stopCapture();
    sequence_ = tabs_->currentIndex() == 0
        ? activeGameTargets() : commands;
    sequenceIndex_ = 0;
    if (!sequence_.isEmpty())
        beginCaptureFor(sequence_.first());
}

void ControlsPage::advanceSequence() {
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

void ControlsPage::skipCapture() {
    if (captureTarget_ < 0 || sequence_.isEmpty()) return;
    skipping_ = true;
    captureTarget_ = -1;
    input_.cancelCapture();
    QTimer::singleShot(0, this, [this] {
        if (skipping_)
            onCaptureCancelled();
    });
}

void ControlsPage::stopCapture() {
    captureTarget_ = -1;
    sequence_.clear();
    sequenceIndex_ = 0;
    skipping_ = false;
    if (input_.capturing())
        input_.cancelCapture();
    updateActionButtons();
}

void ControlsPage::onCaptured(const PadBinding& binding) {
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
        QTimer::singleShot(0, this, &ControlsPage::advanceSequence);
    }
}

void ControlsPage::onCaptureCancelled() {
    if (skipping_) {
        skipping_ = false;
        QTimer::singleShot(0, this, &ControlsPage::advanceSequence);
        return;
    }
    if (captureTarget_ >= 0) {
        captureTarget_ = -1;
        sequence_.clear();
        setFeedback(QStringLiteral("Назначение отменено или время ожидания истекло."));
        updateActionButtons();
    }
}

bool ControlsPage::validateDrafts() {
    // Validate the physical ports and any chained Justifier together.
    for (int profile = 0; profile < 4; ++profile) {
        if (!dirty_.contains(profile * 3) && !dirty_.contains(profile * 3 + 1) &&
            !dirty_.contains(profile * 3 + 2)) continue;
        PlayerMapping players[3];
        for (int player = 0; player < 3; ++player) {
            const int key = profile * 3 + player;
            players[player] = drafts_.contains(key) ? drafts_.value(key)
                : input_.mapping(static_cast<ConsoleProfile>(profile), player);
        }
        if (profile == int(ConsoleProfile::MegaDrive) &&
            players[0].controllerType != ControllerType::Pad) {
            setFeedback(QStringLiteral("Для Mega Drive пистолет выбирается на порту 2."), true);
            return false;
        }
        const bool firstMouseGun = profile == int(ConsoleProfile::MasterSystem) &&
            players[0].connected && players[0].controllerType == ControllerType::LightPhaser &&
            players[0].aimSource == AimSource::Mouse;
        const bool secondMouseGun = players[1].connected &&
            players[1].controllerType != ControllerType::Pad &&
            players[1].aimSource == AimSource::Mouse;
        const bool chainedMouseGun = profile == int(ConsoleProfile::MegaDrive) &&
            players[1].connected && players[1].controllerType == ControllerType::Justifier &&
            players[2].connected && players[2].aimSource == AimSource::Mouse;
        if ((firstMouseGun && secondMouseGun) || (secondMouseGun && chainedMouseGun)) {
            setFeedback(QStringLiteral("Одна мышь управляет одним пистолетом. "
                                       "Для другого игрока выберите левый стик."), true);
            return false;
        }
    }
    for (int key : dirty_) {
        const PlayerMapping& mapping = drafts_[key];
        const auto targets = InputManager::gameTargets(static_cast<ConsoleProfile>(key / 3), mapping.sixButton, mapping.controllerType);
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
        if (mapping.controllerType != ControllerType::Pad) {
            QSet<int> mouseButtons;
            for (int target : targets) {
                const int button = mapping.mouse.value(int(mapping.controllerType)).value(target, 0);
                if (!button) continue;
                if (mouseButtons.contains(button)) {
                    setFeedback(QStringLiteral("Проверьте мышь: одна кнопка назначена двум действиям."), true);
                    return false;
                }
                mouseButtons.insert(button);
            }
        }
    }
    return true;
}

void ControlsPage::setHardwareConfigurationLocked(bool locked) {
    deviceTypeBox_->setEnabled(!locked);
    controllerTypeBox_->setEnabled(!locked);
    secondJustifierBox_->setEnabled(!locked);
    const QString hint = locked
        ? QStringLiteral("Тип и подключение контроллеров берутся из мувика до окончания записи или воспроизведения.")
        : QString();
    deviceTypeBox_->setToolTip(hint);
    controllerTypeBox_->setToolTip(hint);
    secondJustifierBox_->setToolTip(hint);
}

bool ControlsPage::apply() {
    if (captureTarget_ >= 0 || !sequence_.isEmpty() || input_.capturing()) {
        setFeedback(QStringLiteral("Завершите или отмените текущее назначение."), true);
        return false;
    }
    if (!validateDrafts())
        return false;
    for (int key : dirty_)
        input_.setMapping(static_cast<ConsoleProfile>(key / 3),
                          key % 3, drafts_[key]);
    dirty_.clear();
    return true;
}

bool ControlsPage::eventFilter(QObject* watched, QEvent* event) {
    // Release remembered keys if focus leaves the application before key-up.
    if (event->type() == QEvent::ApplicationDeactivate) {
        captureKeysDown_.clear();
        if (captureTarget_ >= 0 || !sequence_.isEmpty()) {
            stopCapture();
            setFeedback(QStringLiteral("Назначение отменено."));
        }
    }
    if (captureTarget_ < 0 && captureKeysDown_.isEmpty())
        return QWidget::eventFilter(watched, event);
    // This is only the visible settings page's Qt input, never a system keyboard grab.
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!isVisible() || !widget || widget->window() != window())
        return QWidget::eventFilter(watched, event);
    if (event->type() == QEvent::KeyRelease) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const int key = InputManager::keyboardKey(keyEvent->key(), keyEvent->nativeScanCode());
        if (captureKeysDown_.contains(key)) {
            if (!keyEvent->isAutoRepeat()) captureKeysDown_.remove(key);
            return true;
        }
    }
    if (captureTarget_ < 0) return QWidget::eventFilter(watched, event);
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    if (event->type() != QEvent::KeyPress)
        return QWidget::eventFilter(watched, event);
    auto* keyEvent = static_cast<QKeyEvent*>(event);
    if (keyEvent->isAutoRepeat()) return true;
    const int key = InputManager::keyboardKey(keyEvent->key(), keyEvent->nativeScanCode());
    if (captureKeysDown_.contains(key)) return true;
    captureKeysDown_.insert(key);
    if (key == Qt::Key_Escape) {
        stopCapture();
        setFeedback(QStringLiteral("Назначение отменено."));
        return true;
    }
    if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt ||
        key == Qt::Key_AltGr || key == Qt::Key_Meta || key == Qt::Key_unknown)
        return true;
    if (captureTarget_ >= 16) {
        setFeedback(QStringLiteral("У команд фиксированные сочетания клавиатуры. Нажмите кнопку геймпада или Esc."), true);
        return true;
    }
    const int target = captureTarget_;
    if (!onKeyboardChanged(target, QKeySequence(QKeyCombination(
            keyEvent->modifiers() & ~Qt::KeypadModifier, Qt::Key(key)))))
        return true;
    captureTarget_ = -1;
    if (input_.capturing()) input_.cancelCapture();
    for (int row = 0; row < gameTable_->rowCount(); ++row) {
        if (gameTable_->item(row, 0)->data(Qt::UserRole).toInt() != target) continue;
        auto* editor = gameTable_->cellWidget(row, 1)->findChild<QKeySequenceEdit*>();
        const QSignalBlocker blocker(editor);
        editor->setKeySequence(QKeySequence(key));
        break;
    }
    if (!sequence_.isEmpty()) QTimer::singleShot(0, this, &ControlsPage::advanceSequence);
    updateActionButtons();
    return true;
}

void ControlsPage::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape &&
        (captureTarget_ >= 0 || !sequence_.isEmpty() || input_.capturing())) {
        stopCapture();
        setFeedback(QStringLiteral("Назначение отменено."));
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ControlsPage::hideEvent(QHideEvent* event) {
    stopCapture();
    captureKeysDown_.clear();
    QWidget::hideEvent(event);
}
