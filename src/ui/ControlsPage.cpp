#include "i18n/Localization.h"
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
#include <QTabBar>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {
const QVector<int> commands = {
    PauseCommand, FastForwardCommand, SaveCommand, LoadCommand,
    ResetCommand, FullscreenCommand, MuteCommand,
    NextSlotCommand, PreviousSlotCommand, MenuCommand, RewindCommand, HardResetCommand,
    FrameAdvanceCommand, FrameBackCommand
};

QString commandName(int command) {
    switch (command) {
    case PauseCommand: return Localization::text(QStringLiteral("Пауза"));
    case FastForwardCommand: return Localization::text(QStringLiteral("Ускорение (удерживать)"));
    case SaveCommand: return Localization::text(QStringLiteral("Быстрое сохранение"));
    case LoadCommand: return Localization::text(QStringLiteral("Быстрая загрузка"));
    case ResetCommand: return Localization::text(QStringLiteral("Сброс"));
    case HardResetCommand: return Localization::text(QStringLiteral("Перезапуск питания"));
    case FrameAdvanceCommand: return Localization::text(QStringLiteral("Следующий кадр"));
    case FrameBackCommand: return Localization::text(QStringLiteral("Предыдущий кадр"));
    case FullscreenCommand: return Localization::text(QStringLiteral("Полный экран"));
    case MuteCommand: return Localization::text(QStringLiteral("Без звука"));
    case NextSlotCommand: return Localization::text(QStringLiteral("Следующий слот"));
    case PreviousSlotCommand: return Localization::text(QStringLiteral("Предыдущий слот"));
    case MenuCommand: return Localization::text(QStringLiteral("Меню"));
    case RewindCommand: return Localization::text(QStringLiteral("Перемотка назад (удерживать)"));
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
    case FrameBackCommand: return QStringLiteral(",");
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
    mainLayout->setContentsMargins(6, 4, 6, 4);
    mainLayout->setSpacing(5);

    auto* scope = new QGridLayout;
    scope->setHorizontalSpacing(12);
    scope->setVerticalSpacing(2);
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
    playerBox_->addItem(Localization::text(QStringLiteral("Порт 1")), 0);
    playerBox_->addItem(Localization::text(QStringLiteral("Порт 2")), 1);
    adapterLabel_ = new QLabel(Localization::text(QStringLiteral("Адаптер контроллеров")), this);
    adapterBox_ = new QComboBox(this);
    adapterBox_->setObjectName(QStringLiteral("adapterBox"));
    adapterBox_->setMinimumWidth(170);
    deviceBox_ = new QComboBox(this);
    deviceBox_->setMinimumWidth(230);
    deadzoneSlider_ = new QSlider(Qt::Horizontal, this);
    deadzoneSlider_->setRange(5, 60);
    deadzoneSlider_->setMinimumWidth(140);
    deadzoneValue_ = new QLabel(this);
    deadzoneValue_->setMinimumWidth(35);

    scope->addWidget(new QLabel(Localization::text(QStringLiteral("Система")), this), 0, 0);
    scope->addWidget(profileBox_, 1, 0);
    scope->addWidget(adapterLabel_, 0, 1);
    scope->addWidget(adapterBox_, 1, 1);
    scope->setColumnStretch(0, 1);
    scope->setColumnStretch(1, 1);
    mainLayout->addLayout(scope);
    mouseCaptureBox_ = new QCheckBox(Localization::text(QStringLiteral("Захватывать мышь")), this);
    mouseCaptureBox_->setObjectName(QStringLiteral("mouseCaptureBox"));
    mouseCaptureBox_->setChecked(input_.mouseCaptureEnabled());
    backgroundGamepadBox_ = new QCheckBox(Localization::text(QStringLiteral("Геймпад работает в фоне")), this);
    backgroundGamepadBox_->setObjectName(QStringLiteral("backgroundGamepadBox"));
    backgroundGamepadBox_->setChecked(input_.backgroundGamepadEnabled());
    auto* inputOptions = new QHBoxLayout;
    inputOptions->addWidget(mouseCaptureBox_);
    inputOptions->addWidget(backgroundGamepadBox_);
    inputOptions->addStretch(1);
    mainLayout->addLayout(inputOptions);

    tabs_ = new QTabBar(this);
    tabs_->setObjectName(QStringLiteral("mappingTabs"));
    tabs_->setExpanding(false);
    tabs_->setUsesScrollButtons(true);
    mainLayout->addWidget(tabs_);

    gamePage_ = new QWidget(this);
    auto* gamePageLayout = new QVBoxLayout(gamePage_);
    gamePageLayout->setContentsMargins(0, 0, 0, 0);
    gamePageLayout->setSpacing(5);
    auto* gameOptions = new QGridLayout;
    gameOptions->setHorizontalSpacing(10);
    gameOptions->setVerticalSpacing(2);
    auto* deadzoneRow = new QHBoxLayout;
    deadzoneRow->setContentsMargins(0, 0, 0, 0);
    deadzoneRow->addWidget(deadzoneSlider_);
    deadzoneRow->addWidget(deadzoneValue_);
    gameOptions->addWidget(new QLabel(Localization::text(QStringLiteral("Геймпад")), gamePage_), 2, 0);
    gameOptions->addWidget(deviceBox_, 3, 0);
    gameOptions->addWidget(new QLabel(Localization::text(QStringLiteral("Мёртвая зона")), gamePage_), 2, 1);
    gameOptions->addLayout(deadzoneRow, 3, 1);
    controllerTypeLabel_ = new QLabel(Localization::text(QStringLiteral("Тип геймпада")), this);
    controllerTypeBox_ = new QComboBox(this);
    controllerTypeBox_->setObjectName(QStringLiteral("controllerTypeBox"));
    controllerTypeBox_->addItem(Localization::text(QStringLiteral("3 кнопки")), false);
    controllerTypeBox_->addItem(Localization::text(QStringLiteral("6 кнопок")), true);
    gameOptions->addWidget(controllerTypeLabel_, 0, 1);
    gameOptions->addWidget(controllerTypeBox_, 1, 1);
    deviceTypeLabel_ = new QLabel(Localization::text(QStringLiteral("Контроллер")), this);
    deviceTypeBox_ = new QComboBox(this);
    deviceTypeBox_->setObjectName(QStringLiteral("deviceTypeBox"));
    gameOptions->addWidget(deviceTypeLabel_, 0, 0);
    gameOptions->addWidget(deviceTypeBox_, 1, 0);
    secondJustifierBox_ = new QCheckBox(Localization::text(QStringLiteral("Второй пистолет")), this);
    secondJustifierBox_->setObjectName(QStringLiteral("secondJustifierBox"));
    gameOptions->addWidget(secondJustifierBox_, 1, 2);
    aimSourceLabel_ = new QLabel(Localization::text(QStringLiteral("Прицеливание")), this);
    aimSourceBox_ = new QComboBox(this);
    aimSourceBox_->setObjectName(QStringLiteral("aimSourceBox"));
    aimSourceBox_->addItem(Localization::text(QStringLiteral("Мышь")), int(AimSource::Mouse));
    aimSourceBox_->addItem(Localization::text(QStringLiteral("Левый стик")), int(AimSource::LeftStick));
    gameOptions->addWidget(aimSourceLabel_, 0, 1);
    gameOptions->addWidget(aimSourceBox_, 1, 1);
    showCrosshairBox_ = new QCheckBox(Localization::text(QStringLiteral("Показывать прицел")), this);
    showCrosshairBox_->setObjectName(QStringLiteral("showCrosshairBox"));
    gameOptions->addWidget(showCrosshairBox_, 3, 2);
    analogSensitivityLabel_ = new QLabel(Localization::text(QStringLiteral("Скорость стика")), this);
    analogSensitivitySlider_ = new QSlider(Qt::Horizontal, this);
    analogSensitivitySlider_->setRange(25, 400);
    analogSensitivitySlider_->setObjectName(QStringLiteral("analogSensitivitySlider"));
    analogSensitivityValue_ = new QLabel(this);
    auto* sensitivityRow = new QHBoxLayout;
    sensitivityRow->setContentsMargins(0, 0, 0, 0);
    sensitivityRow->addWidget(analogSensitivityLabel_);
    sensitivityRow->addWidget(analogSensitivitySlider_, 1);
    sensitivityRow->addWidget(analogSensitivityValue_);
    gameOptions->addLayout(sensitivityRow, 3, 2);
    gameOptions->setColumnStretch(0, 2);
    gameOptions->setColumnStretch(1, 2);
    gameOptions->setColumnStretch(2, 1);
    gamePageLayout->addLayout(gameOptions);

    auto* gameLayout = new QHBoxLayout;
    gameLayout->setContentsMargins(0, 0, 0, 0);
    gameLayout->setSpacing(8);
    diagram_ = new ControllerDiagram(gamePage_);
    diagram_->setMinimumSize(240, 210);
    gameLayout->addWidget(diagram_, 4);
    gameTable_ = new QTableWidget(gamePage_);
    gameTable_->setObjectName(QStringLiteral("gameMappingTable"));
    gameTable_->setColumnCount(4);
    gameTable_->setHorizontalHeaderLabels(
        {Localization::text(QStringLiteral("Кнопка")), Localization::text(QStringLiteral("Клавиатура")),
         Localization::text(QStringLiteral("Геймпад")), Localization::text(QStringLiteral("Мышь"))});
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
    gamePageLayout->addLayout(gameLayout, 1);
    mainLayout->addWidget(gamePage_, 1);

    commandPage_ = new QWidget(this);
    auto* commandLayout = new QVBoxLayout(commandPage_);
    commandLayout->setContentsMargins(0, 0, 0, 0);
    auto* commandSource = new QHBoxLayout;
    commandSource->addWidget(new QLabel(Localization::text(QStringLiteral("Геймпад порта")), commandPage_));
    commandSource->addWidget(playerBox_);
    commandSource->addStretch(1);
    commandLayout->addLayout(commandSource);
    commandTable_ = new QTableWidget(commandPage_);
    commandTable_->setObjectName(QStringLiteral("commandMappingTable"));
    commandTable_->setColumnCount(3);
    commandTable_->setHorizontalHeaderLabels(
        {Localization::text(QStringLiteral("Команда")), Localization::text(QStringLiteral("Клавиатура")),
         Localization::text(QStringLiteral("Геймпад"))});
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
    mainLayout->addWidget(commandPage_, 1);
    commandPage_->hide();

    auto* actionRow = new QHBoxLayout;
    assignButton_ = new QPushButton(Localization::text(QStringLiteral("Назначить")), this);
    assignButton_->setObjectName(QStringLiteral("assignBinding"));
    sequenceButton_ = new QPushButton(Localization::text(QStringLiteral("Назначить по порядку")), this);
    sequenceButton_->setObjectName(QStringLiteral("assignSequence"));
    skipButton_ = new QPushButton(Localization::text(QStringLiteral("Пропустить")), this);
    clearButton_ = new QPushButton(Localization::text(QStringLiteral("Очистить геймпад")), this);
    auto* defaultsButton = new QPushButton(Localization::text(QStringLiteral("По умолчанию")), this);
    actionRow->addWidget(assignButton_);
    actionRow->addWidget(sequenceButton_);
    actionRow->addWidget(skipButton_);
    actionRow->addWidget(clearButton_);
    actionRow->addStretch(1);
    actionRow->addWidget(defaultsButton);
    mainLayout->addLayout(actionRow);

    feedback_ = new QLabel(this);
    feedback_->setWordWrap(true);
    feedback_->setMinimumHeight(28);
    mainLayout->addWidget(feedback_);

    connect(profileBox_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { stopCapture(); refreshContents(); });
    connect(adapterBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (refreshing_) return;
        stopCapture();
        const ConsoleProfile system = selectedProfile();
        const int profile = int(system);
        const ControllerAdapter before = selectedAdapter();
        const ControllerAdapter after = ControllerAdapter(adapterBox_->currentData().toInt());
        if (before == after) return;
        for (int player = 0; player < int(MaxControllerPlayers); ++player) {
            const int key = profile * int(MaxControllerPlayers) + player;
            if (before == ControllerAdapter::None) {
                ordinaryConnectionDrafts_[key] = drafts_.contains(key)
                    ? drafts_[key].connected : input_.ordinaryMapping(system, player).connected;
            } else {
                adapterConnectionDrafts_[key] = drafts_.contains(key)
                    ? drafts_[key].connected : input_.adapterConnection(system, unsigned(player));
            }
        }
        adapterDrafts_[profile] = after;
        dirtyAdapters_.insert(profile);
        for (int player = 0; player < int(MaxControllerPlayers); ++player) {
            const int key = profile * int(MaxControllerPlayers) + player;
            if (!dirty_.contains(key))
                drafts_[key] = input_.ordinaryMapping(system, player);
            drafts_[key].connected = after == ControllerAdapter::None
                ? ordinaryConnectionDrafts_.value(key, input_.ordinaryMapping(system, player).connected)
                : adapterConnectionDrafts_.value(key, input_.adapterConnection(system, unsigned(player)));
        }
        refreshContents();
    });
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
            if (selectedAdapter() == ControllerAdapter::None)
                draft().controllerType = static_cast<ControllerType>(type);
            draft().connected = true;
        }
        dirty_.insert(draftKey());
        refreshContents();
    });
    connect(secondJustifierBox_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (refreshing_ || selectedProfile() != ConsoleProfile::MegaDrive || selectedPlayer() != 1) return;
        stopCapture();
        const int key = int(ConsoleProfile::MegaDrive) * int(MaxControllerPlayers) + 2;
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
        refreshContents();
    });
    connect(deadzoneSlider_, &QSlider::valueChanged, this, [this](int value) {
        deadzoneValue_->setText(QStringLiteral("%1 %").arg(value));
        if (refreshing_) return;
        draft().deadzonePercent = value;
        dirty_.insert(draftKey());
    });
    connect(analogSensitivitySlider_, &QSlider::valueChanged, this, [this](int value) {
        analogSensitivityValue_->setText(QStringLiteral("%1 %").arg(value));
        if (refreshing_) return;
        draft().analogSensitivity = value;
        dirty_.insert(draftKey());
    });
    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        stopCapture();
        if (index < 0) return;
        if (!commandTabSelected()) {
            const int playerIndex = playerBox_->findData(tabs_->tabData(index).toInt());
            if (playerIndex >= 0 && playerIndex != playerBox_->currentIndex()) {
                playerBox_->setCurrentIndex(playerIndex);
                return;
            }
        }
        gamePage_->setVisible(!commandTabSelected());
        commandPage_->setVisible(commandTabSelected());
        const auto& targets = commandTabSelected() ? commands : activeGameTargets();
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
    connect(diagram_, &ControllerDiagram::targetDoubleClicked, this, [this](int target) {
        stopCapture();
        beginCaptureFor(target);
    });
    connect(gameTable_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (const auto* item = gameTable_->item(row, 0)) {
            stopCapture();
            beginCaptureFor(item->data(Qt::UserRole).toInt());
        }
    });
    connect(commandTable_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (const auto* item = commandTable_->item(row, 0)) {
            stopCapture();
            beginCaptureFor(item->data(Qt::UserRole).toInt());
        }
    });
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
        setFeedback(Localization::text(QStringLiteral("Назначение кнопки очищено.")));
        updateActionButtons();
    });
    connect(defaultsButton, &QPushButton::clicked, this, [this] {
        stopCapture();
        draft() = InputManager::defaultMapping(selectedProfile(), selectedPlayer());
        if (selectedAdapter() != ControllerAdapter::None)
            draft().connected = true;
        dirty_.insert(draftKey());
        refreshContents();
        setFeedback(Localization::text(QStringLiteral("Настройки этого игрока и системы сброшены к исходным.")));
    });
    connect(&input_, &InputManager::jcartChanged, this, [this] {
        const int profile = int(ConsoleProfile::MegaDrive);
        if (adapterDrafts_.contains(profile) &&
            adapterPlayerCount(adapterDrafts_.value(profile)) == MaxControllerPlayers) {
            adapterDrafts_.remove(profile);
            dirtyAdapters_.remove(profile);
        }
        refreshContents();
    });
    connect(&input_, &InputManager::devicesChanged,
            this, &ControlsPage::refreshDevices);
    connect(&input_, &InputManager::captureReady, this, [this] {
        if (captureTarget_ < 0) return;
        const QString label = captureTarget_ < 16
            ? InputManager::targetName(selectedProfile(), captureTarget_, displayMapping().controllerType)
            : commandName(captureTarget_);
        setFeedback((captureTarget_ < 16
            ? Localization::text(QStringLiteral("Назначение «%1»: нажмите клавишу или кнопку геймпада. Esc — отмена."))
            : Localization::text(QStringLiteral("Назначение «%1»: нажмите кнопку геймпада. Esc — отмена."))).arg(label));
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
}

ControlsPage::~ControlsPage() {
    qApp->removeEventFilter(this);
    stopCapture();
}

ControlsPage::ViewState ControlsPage::viewState() const {
    return {int(selectedProfile()), selectedPlayer(), commandTabSelected()};
}

void ControlsPage::setViewState(const ViewState& state) {
    stopCapture();
    const int profileIndex = profileBox_->findData(state.profile);
    if (profileIndex >= 0) profileBox_->setCurrentIndex(profileIndex);
    const int playerIndex = playerBox_->findData(state.player);
    if (playerIndex >= 0) playerBox_->setCurrentIndex(playerIndex);
    refreshContents();
    tabs_->setCurrentIndex(state.commands ? tabs_->count() - 1 : playerBox_->currentIndex());
}

void ControlsPage::setExternalFeedback(bool external) {
    externalFeedback_ = external;
    feedback_->setVisible(!externalFeedback_);
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
    return static_cast<int>(selectedProfile()) * int(MaxControllerPlayers) + selectedPlayer();
}

ControllerAdapter ControlsPage::selectedAdapter() const {
    const int profile = int(selectedProfile());
    return adapterDrafts_.contains(profile)
        ? adapterDrafts_.value(profile) : input_.adapter(selectedProfile());
}

bool ControlsPage::selectedJCart() const {
    return selectedProfile() == ConsoleProfile::MegaDrive &&
        selectedProfile() == input_.profile() && input_.jcart();
}

unsigned ControlsPage::selectedJCartBase() const {
    if (!selectedJCart()) return 0;
    const ControllerAdapter adapter = selectedAdapter();
    if (adapter != ControllerAdapter::None) return adapterPlayerCount(adapter);
    const int parentKey = int(ConsoleProfile::MegaDrive) * int(MaxControllerPlayers) + 1;
    const int chainKey = parentKey + 1;
    if (!dirtyAdapters_.contains(int(ConsoleProfile::MegaDrive)) &&
        !dirty_.contains(parentKey) && !dirty_.contains(chainKey))
        return input_.jcartPlayerBase();
    const PlayerMapping parent = drafts_.contains(parentKey) ? drafts_.value(parentKey)
        : input_.ordinaryMapping(ConsoleProfile::MegaDrive, 1);
    const PlayerMapping chain = drafts_.contains(chainKey) ? drafts_.value(chainKey)
        : input_.ordinaryMapping(ConsoleProfile::MegaDrive, 2);
    return parent.connected && parent.controllerType == ControllerType::Justifier &&
           chain.connected ? 3u : 2u;
}

bool ControlsPage::selectedJCartPlayer(int player) const {
    const unsigned base = selectedJCartBase();
    return base && player >= int(base) && player < int(base + 2);
}

PlayerMapping ControlsPage::displayMapping() const {
    const int key = int(selectedProfile()) * int(MaxControllerPlayers) + selectedPlayer();
    PlayerMapping mapping = drafts_.contains(key) ? drafts_.value(key)
        : input_.mapping(selectedProfile(), selectedPlayer());
    if (selectedAdapter() != ControllerAdapter::None ||
        selectedJCartPlayer(selectedPlayer())) {
        mapping.controllerType = ControllerType::Pad;
        if (!dirty_.contains(key) && input_.adapter(selectedProfile()) == ControllerAdapter::None &&
            (selectedPlayer() < int(adapterPlayerCount(selectedAdapter())) ||
             selectedJCartPlayer(selectedPlayer())))
            mapping.connected = true;
    }
    return mapping;
}

PlayerMapping& ControlsPage::draft() {
    const int key = draftKey();
    if (!drafts_.contains(key))
        drafts_.insert(key, input_.mapping(selectedProfile(), selectedPlayer()));
    return drafts_[key];
}

QVector<int> ControlsPage::activeGameTargets() {
    const auto mapping = displayMapping();
    return InputManager::gameTargets(selectedProfile(), mapping.sixButton, mapping.controllerType);
}

bool ControlsPage::commandTabSelected() const {
    return tabs_->count() > 0 && tabs_->currentIndex() == tabs_->count() - 1;
}

void ControlsPage::refreshPlayerChoices() {
    const bool wasCommand = commandTabSelected();
    const int current = selectedPlayer();
    const ControllerAdapter adapter = selectedAdapter();
    const QSignalBlocker blocker(playerBox_);
    playerBox_->clear();
    if (adapter != ControllerAdapter::None) {
        for (unsigned player = 0; player < adapterPlayerCount(adapter); ++player)
            playerBox_->addItem(QStringLiteral("P%1").arg(player + 1), int(player));
    } else {
        const int secondaryKey = int(selectedProfile()) * int(MaxControllerPlayers) + 2;
        const int parentKey = int(selectedProfile()) * int(MaxControllerPlayers) + 1;
        const PlayerMapping parent = drafts_.contains(parentKey) ? drafts_.value(parentKey)
            : input_.ordinaryMapping(selectedProfile(), 1);
        const PlayerMapping secondary = drafts_.contains(secondaryKey) ? drafts_.value(secondaryKey)
            : input_.ordinaryMapping(selectedProfile(), 2);
        const bool pair = selectedProfile() == ConsoleProfile::MegaDrive &&
            parent.connected && parent.controllerType == ControllerType::Justifier && secondary.connected;
        playerBox_->addItem(Localization::text(QStringLiteral("Порт 1")), 0);
        if (selectedProfile() != ConsoleProfile::GameGear)
            playerBox_->addItem(Localization::text(QStringLiteral("Порт 2")), 1);
        if (pair) playerBox_->addItem(Localization::text(QStringLiteral("Второй пистолет")), 2);
    }
    const unsigned base = selectedJCartBase();
    if (base) for (unsigned extra = 0; extra < 2; ++extra)
        playerBox_->addItem(Localization::text(QStringLiteral("J-Cart %1 · P%2"))
            .arg(extra + 1).arg(base + extra + 1), int(base + extra));
    const int found = playerBox_->findData(current);
    playerBox_->setCurrentIndex(found >= 0 ? found : 0);
    const QSignalBlocker tabBlocker(tabs_);
    while (tabs_->count() > 0) tabs_->removeTab(tabs_->count() - 1);
    for (int index = 0; index < playerBox_->count(); ++index) {
        const int tab = tabs_->addTab(playerBox_->itemText(index));
        tabs_->setTabData(tab, playerBox_->itemData(index));
    }
    tabs_->addTab(Localization::text(QStringLiteral("Команды")));
    tabs_->setCurrentIndex(wasCommand ? tabs_->count() - 1 : playerBox_->currentIndex());
    gamePage_->setVisible(!wasCommand);
    commandPage_->setVisible(wasCommand);
}

QString ControlsPage::captureDeviceKey() {
    const QString key = draft().deviceKey;
    if (key != QStringLiteral("auto"))
        return key;
    const auto devices = input_.devices();
    if (devices.isEmpty())
        return key;
    QVector<QString> assigned(static_cast<qsizetype>(MaxControllerPlayers));
    QVector<PlayerMapping> players;
    for (int player = 0; player < int(MaxControllerPlayers); ++player) {
        const int mappingKey = int(selectedProfile()) * int(MaxControllerPlayers) + player;
        players.append(drafts_.contains(mappingKey) ? drafts_.value(mappingKey)
            : input_.mapping(selectedProfile(), player));
        if (players[player].deviceKey != QStringLiteral("auto") &&
            players[player].deviceKey != QStringLiteral("none"))
            assigned[player] = players[player].deviceKey;
    }
    const bool chainedActive = selectedAdapter() == ControllerAdapter::None &&
        selectedProfile() == ConsoleProfile::MegaDrive &&
        players[1].connected && players[1].controllerType == ControllerType::Justifier &&
        players[2].connected;
    const auto active = [this, &players, chainedActive](int player) {
        if (selectedJCartPlayer(player)) return players[player].connected;
        if (selectedAdapter() != ControllerAdapter::None)
            return player < int(adapterPlayerCount(selectedAdapter())) && players[player].connected;
        return player == 2 ? chainedActive : player < 2 && players[player].connected;
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (int player = 0; player < int(MaxControllerPlayers); ++player) {
            if (players[player].deviceKey != QStringLiteral("auto") ||
                active(player) != (pass == 0)) continue;
            for (const InputDevice& device : devices) {
                bool usedByActive = false;
                for (int other = 0; other < int(MaxControllerPlayers); ++other)
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
    deviceBox_->addItem(Localization::text(QStringLiteral("Автоматически")), QStringLiteral("auto"));
    deviceBox_->addItem(Localization::text(QStringLiteral("Без геймпада")), QStringLiteral("none"));
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
            Localization::text(QStringLiteral("Отключено: %1")).arg(name), currentKey);
        deviceBox_->setItemData(deviceBox_->count() - 1,
                                currentKey, Qt::ToolTipRole);
        index = deviceBox_->count() - 1;
    }
    deviceBox_->setCurrentIndex(index);
    updateActionButtons();
}

void ControlsPage::refreshContents() {
    refreshing_ = true;
    const ConsoleProfile profile = selectedProfile();
    const ControllerAdapter adapter = selectedAdapter();
    const bool md = profile == ConsoleProfile::MegaDrive;
    const bool sms = profile == ConsoleProfile::MasterSystem;
    const bool supportsAdapter = md || sms;
    {
        const QSignalBlocker blocker(adapterBox_);
        adapterBox_->clear();
        adapterBox_->addItem(Localization::text(QStringLiteral("Нет адаптера")), int(ControllerAdapter::None));
        if (md) {
            adapterBox_->addItem(Localization::text(QStringLiteral("Team Player · порт 1")), int(ControllerAdapter::TeamPlayerPort1));
            adapterBox_->addItem(Localization::text(QStringLiteral("Team Player · порт 2")), int(ControllerAdapter::TeamPlayerPort2));
            if (!selectedJCart())
                adapterBox_->addItem(Localization::text(QStringLiteral("Team Player · оба порта")), int(ControllerAdapter::TeamPlayerBoth));
            adapterBox_->addItem(QStringLiteral("4 Way Play"), int(ControllerAdapter::FourWayPlay));
        } else if (sms) {
            adapterBox_->addItem(Localization::text(QStringLiteral("Master Tap · порт 1")), int(ControllerAdapter::MasterTapPort1));
            adapterBox_->addItem(Localization::text(QStringLiteral("Master Tap · порт 2")), int(ControllerAdapter::MasterTapPort2));
            adapterBox_->addItem(Localization::text(QStringLiteral("Master Tap · оба порта")), int(ControllerAdapter::MasterTapBoth));
        }
        adapterBox_->setCurrentIndex(adapterBox_->findData(int(adapter)));
    }
    adapterLabel_->setVisible(supportsAdapter);
    adapterBox_->setVisible(supportsAdapter);
    refreshPlayerChoices();
    const PlayerMapping mapping = displayMapping();
    const bool gun = isLightgun(mapping.controllerType);
    const bool analog = isAnalogController(mapping.controllerType);
    const bool chained = md && adapter == ControllerAdapter::None && selectedPlayer() == 2 &&
        !selectedJCartPlayer(selectedPlayer());
    controllerTypeLabel_->setVisible(mapping.connected && md && !gun && !analog && !chained);
    controllerTypeBox_->setVisible(mapping.connected && md && !gun && !analog && !chained);
    controllerTypeBox_->setCurrentIndex(mapping.sixButton ? 1 : 0);
    deviceTypeLabel_->setVisible(!chained);
    deviceTypeBox_->setVisible(!chained);
    deviceTypeBox_->clear();
    deviceTypeBox_->addItem(Localization::text(QStringLiteral("Нет")), -1);
    deviceTypeBox_->addItem(Localization::text(QStringLiteral("Геймпад")), int(ControllerType::Pad));
    if (adapter == ControllerAdapter::None && !selectedJCartPlayer(selectedPlayer())) {
        if (md && selectedPlayer() < 2) {
            deviceTypeBox_->addItem(QStringLiteral("MD Mouse"), int(ControllerType::Mouse));
            if (selectedPlayer() == 1) {
                deviceTypeBox_->addItem(QStringLiteral("Menacer"), int(ControllerType::Menacer));
                deviceTypeBox_->addItem(QStringLiteral("Justifier"), int(ControllerType::Justifier));
            }
        } else if (sms && selectedPlayer() < 2) {
            deviceTypeBox_->addItem(QStringLiteral("Light Phaser"), int(ControllerType::LightPhaser));
            deviceTypeBox_->addItem(QStringLiteral("SMS Paddle"), int(ControllerType::Paddle));
        }
    }
    deviceTypeBox_->setCurrentIndex(deviceTypeBox_->findData(
        mapping.connected ? int(mapping.controllerType) : -1));
    const int secondaryKey = int(ConsoleProfile::MegaDrive) * int(MaxControllerPlayers) + 2;
    const bool secondGun = drafts_.contains(secondaryKey) ? drafts_.value(secondaryKey).connected
        : input_.mapping(ConsoleProfile::MegaDrive, 2).connected;
    secondJustifierBox_->setVisible(adapter == ControllerAdapter::None && md &&
                                    selectedPlayer() == 1 && mapping.connected &&
                                    mapping.controllerType == ControllerType::Justifier);
    secondJustifierBox_->setChecked(secondGun);
    aimSourceLabel_->setText(gun ? Localization::text(QStringLiteral("Прицеливание"))
        : Localization::text(QStringLiteral("Движение")));
    aimSourceLabel_->setVisible(mapping.connected && (gun || analog));
    aimSourceBox_->setVisible(mapping.connected && (gun || analog));
    aimSourceBox_->setCurrentIndex(aimSourceBox_->findData(int(mapping.aimSource)));
    showCrosshairBox_->setVisible(mapping.connected && gun);
    showCrosshairBox_->setChecked(mapping.showCrosshair);
    gameTable_->setColumnHidden(3, !(gun || analog));
    const bool sensitivityVisible = mapping.connected &&
        mapping.controllerType == ControllerType::Mouse &&
        mapping.aimSource == AimSource::LeftStick;
    analogSensitivityLabel_->setVisible(sensitivityVisible);
    analogSensitivitySlider_->setVisible(sensitivityVisible);
    analogSensitivityValue_->setVisible(sensitivityVisible);
    analogSensitivitySlider_->setValue(qBound(25, mapping.analogSensitivity, 400));
    analogSensitivityValue_->setText(QStringLiteral("%1 %").arg(analogSensitivitySlider_->value()));
    refreshDevices();
    deadzoneSlider_->setValue(qBound(5, mapping.deadzonePercent, 60));
    deadzoneValue_->setText(QStringLiteral("%1 %").arg(deadzoneSlider_->value()));
    diagram_->setProfile(profile);
    diagram_->setSixButton(mapping.sixButton);
    diagram_->setControllerType(mapping.controllerType);
    // The table scrolls independently; keep the illustration compact enough
    // that the whole settings page fits the standard dialog height.
    diagram_->setMinimumHeight(210);
    diagram_->setTargets(activeGameTargets());
    fillGameTable();
    fillCommandTable();
    refreshing_ = false;
    const auto& targets = commandTabSelected() ? commands : activeGameTargets();
    selectTarget(targets.isEmpty() ? -1 : targets.first());
}

void ControlsPage::fillGameTable() {
    const QVector<int> targets = activeGameTargets();
    const PlayerMapping mapping = displayMapping();
    gameTable_->clearContents();
    gameTable_->setRowCount(targets.size());
    for (int row = 0; row < targets.size(); ++row) {
        const int target = targets[row];
        gameTable_->setRowHeight(row, 30);
        gameTable_->setItem(row, 0,
            cell(InputManager::targetName(selectedProfile(), target, mapping.controllerType), target));
        auto* keyEdit = new PhysicalKeySequenceEdit(gameTable_);
        keyEdit->setProperty("bindingTarget", target);
        keyEdit->installEventFilter(this);
        if (auto* line = keyEdit->findChild<QLineEdit*>()) {
            line->setPlaceholderText(Localization::text(QStringLiteral("Нажмите клавишу")));
            line->setProperty("bindingTarget", target);
            line->installEventFilter(this);
        }
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
        clearKey->setToolTip(Localization::text(QStringLiteral("Очистить клавишу")));
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
        if (isLightgun(mapping.controllerType) || isAnalogController(mapping.controllerType)) {
            auto* mouseBox = new QComboBox(gameTable_);
            mouseBox->addItem(QStringLiteral("—"), 0);
            mouseBox->addItem(Localization::text(QStringLiteral("Левая")), int(Qt::LeftButton));
            mouseBox->addItem(Localization::text(QStringLiteral("Правая")), int(Qt::RightButton));
            mouseBox->addItem(Localization::text(QStringLiteral("Средняя")), int(Qt::MiddleButton));
            mouseBox->addItem(Localization::text(QStringLiteral("Боковая 1")), int(Qt::XButton1));
            mouseBox->addItem(Localization::text(QStringLiteral("Боковая 2")), int(Qt::XButton2));
            const int type = int(mapping.controllerType);
            mouseBox->setCurrentIndex(mouseBox->findData(mapping.mouse.value(type).value(target, 0)));
            gameTable_->setCellWidget(row, 3, mouseBox);
            connect(mouseBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [this, target, type, mouseBox] {
                        if (refreshing_) return;
                        auto& buttons = draft().mouse[type];
                        const int button = mouseBox->currentData().toInt();
                        if (button) {
                            const auto targets = activeGameTargets();
                            for (auto it = buttons.begin(); it != buttons.end(); ++it) {
                                if (it.key() == target || !targets.contains(it.key()) || it.value() != button)
                                    continue;
                                it.value() = 0;
                                for (int row = 0; row < gameTable_->rowCount(); ++row) {
                                    const auto* item = gameTable_->item(row, 0);
                                    if (!item || item->data(Qt::UserRole).toInt() != it.key()) continue;
                                    auto* previous = qobject_cast<QComboBox*>(gameTable_->cellWidget(row, 3));
                                    if (previous) {
                                        const QSignalBlocker blocker(previous);
                                        previous->setCurrentIndex(previous->findData(0));
                                    }
                                    break;
                                }
                            }
                        }
                        buttons[target] = button;
                        dirty_.insert(draftKey());
                    });
        }
    }
}

void ControlsPage::fillCommandTable() {
    const PlayerMapping mapping = displayMapping();
    commandTable_->clearContents();
    commandTable_->setRowCount(commands.size());
    for (int row = 0; row < commands.size(); ++row) {
        const int target = commands[row];
        commandTable_->setRowHeight(row, 30);
        commandTable_->setItem(row, 0, cell(commandName(target), target));
        commandTable_->setItem(row, 1, cell(commandShortcut(target)));
        commandTable_->setItem(row, 2, cell(padName(mapping, target)));
    }
}

void ControlsPage::selectTarget(int target) {
    if (captureTarget_ >= 0 && captureTarget_ != target)
        stopCapture();
    selectedTarget_ = target;
    if (target >= 0 && target < 16)
        diagram_->setSelectedTarget(target);
    else
        diagram_->setSelectedTarget(-1);
    QTableWidget* table = commandTabSelected() ? commandTable_ : gameTable_;
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

void ControlsPage::refreshKeyboardCell(int target) {
    for (int row = 0; row < gameTable_->rowCount(); ++row) {
        const auto* item = gameTable_->item(row, 0);
        if (!item || item->data(Qt::UserRole).toInt() != target) continue;
        auto* editor = gameTable_->cellWidget(row, 1)->findChild<QKeySequenceEdit*>();
        const QSignalBlocker blocker(editor);
        const int key = draft().keyboard.value(target, 0);
        editor->setKeySequence(key ? QKeySequence(key) : QKeySequence());
        return;
    }
}

void ControlsPage::updateActionButtons() {
    if (!assignButton_) return;
    const bool gamePage = !commandTabSelected();
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
    feedback_->setVisible(!externalFeedback_);
    emit feedbackChanged(message, warning);
    const QColor color = warning
        ? (palette().color(QPalette::Base).lightness() < 128
               ? QColor(245, 144, 122) : QColor(164, 48, 40))
        : (palette().color(QPalette::Base).lightness() < 128
               ? QColor(116, 222, 153) : QColor(29, 127, 65));
    feedback_->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
}

void ControlsPage::selectFailure(int profile, int player, bool commandTab) {
    const int profileIndex = profileBox_->findData(profile);
    if (profileIndex >= 0) profileBox_->setCurrentIndex(profileIndex);
    const int playerIndex = playerBox_->findData(player);
    if (playerIndex >= 0) playerBox_->setCurrentIndex(playerIndex);
    tabs_->setCurrentIndex(commandTab ? tabs_->count() - 1 : playerBox_->currentIndex());
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
        error = Localization::text(QStringLiteral("Назначьте одну клавишу без Ctrl, Alt или Shift."));
    else if (reservedKey(key))
        error = Localization::text(QStringLiteral("0–9 выбирают слот; Tab, Esc, P, M, R, F5, F8, F12, запятая и точка зарезервированы."));
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
    PlayerMapping& mapping = draft();
    if (key) {
        const auto targets = activeGameTargets();
        for (auto it = mapping.keyboard.begin(); it != mapping.keyboard.end(); ++it) {
            if (it.key() == target || !targets.contains(it.key()) || it.value() != key) continue;
            it.value() = 0;
            refreshKeyboardCell(it.key());
        }
    }
    mapping.keyboard[target] = key;
    dirty_.insert(draftKey());
    setFeedback(key
        ? Localization::text(QStringLiteral("Клавиша назначена."))
        : Localization::text(QStringLiteral("Клавиша очищена.")));
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
        setFeedback(Localization::text(QStringLiteral("Для назначения команды подключите геймпад. Сочетания клавиатуры указаны в таблице.")), true);
        return;
    }
    captureTarget_ = target;
    selectTarget(target);
    setFocus(Qt::OtherFocusReason);
    const QString label = target < 16
        ? InputManager::targetName(selectedProfile(), target, displayMapping().controllerType)
        : commandName(target);
    setFeedback((target < 16
        ? Localization::text(QStringLiteral("Назначение «%1»: нажмите клавишу или кнопку геймпада. Esc — отмена."))
        : Localization::text(QStringLiteral("Назначение «%1»: нажмите кнопку геймпада. Esc — отмена."))).arg(label));
    if (padPresent)
        input_.beginCapture(deviceKey, selectedPlayer());
    updateActionButtons();
}

void ControlsPage::startSequence() {
    stopCapture();
    sequence_ = commandTabSelected() ? commands : activeGameTargets();
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
        setFeedback(Localization::text(QStringLiteral("Последовательное назначение завершено.")));
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
    const bool hadPending = captureTarget_ >= 0 || !sequence_.isEmpty();
    captureTarget_ = -1;
    sequence_.clear();
    sequenceIndex_ = 0;
    skipping_ = false;
    if (input_.capturing())
        input_.cancelCapture();
    if (hadPending) setFeedback(QString());
    updateActionButtons();
}

void ControlsPage::onCaptured(const PadBinding& binding) {
    if (captureTarget_ < 0) return;
    const int target = captureTarget_;
    captureTarget_ = -1;
    PlayerMapping& current = draft();
    const auto targets = activeGameTargets();
    const QString deviceKey = captureDeviceKey();
    for (auto it = current.pad.begin(); it != current.pad.end(); ++it) {
        if ((it.key() >= 16 || targets.contains(it.key())) &&
            it.key() != target && it.value().kind != PadBinding::None &&
            input_.capturedBindingConflicts(selectedProfile(), deviceKey,
                                            it.key(), it.value(), binding)) {
            it.value() = PadBinding{};
            refreshPadCell(it.key());
        }
    }
    current.pad[target] = binding;
    dirty_.insert(draftKey());
    refreshPadCell(target);
    if (sequence_.isEmpty()) {
        setFeedback(Localization::text(QStringLiteral("Назначено: %1")).arg(
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
        setFeedback(Localization::text(QStringLiteral("Назначение отменено или время ожидания истекло.")));
        updateActionButtons();
    }
}

bool ControlsPage::validateDrafts() {
    for (int profile = 0; profile < 4; ++profile) {
        bool touched = dirtyAdapters_.contains(profile);
        for (int player = 0; player < int(MaxControllerPlayers); ++player)
            touched |= dirty_.contains(profile * int(MaxControllerPlayers) + player);
        if (!touched) continue;
        const ConsoleProfile system = ConsoleProfile(profile);
        const ControllerAdapter adapter = adapterDrafts_.contains(profile)
            ? adapterDrafts_.value(profile) : input_.adapter(system);
        PlayerMapping players[MaxControllerPlayers];
        for (int player = 0; player < int(MaxControllerPlayers); ++player) {
            const int key = profile * int(MaxControllerPlayers) + player;
            players[player] = drafts_.contains(key) ? drafts_.value(key)
                : input_.mapping(system, player);
        }
        if (adapter == ControllerAdapter::None) {
            if (profile == int(ConsoleProfile::MegaDrive) &&
                players[0].controllerType != ControllerType::Pad &&
                players[0].controllerType != ControllerType::Mouse) {
                selectFailure(profile, 0);
                setFeedback(Localization::text(QStringLiteral("Для Mega Drive пистолет выбирается на порту 2.")), true);
                return false;
            }
            int firstMouse = -1;
            int conflictingMouse = -1;
            for (int player = 0; player < 2; ++player)
                if (players[player].connected &&
                    (isLightgun(players[player].controllerType) ||
                     isAnalogController(players[player].controllerType)) &&
                    players[player].aimSource == AimSource::Mouse) {
                    if (firstMouse >= 0) conflictingMouse = player;
                    else firstMouse = player;
                }
            if (profile == int(ConsoleProfile::MegaDrive) &&
                players[1].connected && players[1].controllerType == ControllerType::Justifier &&
                players[2].connected && players[2].aimSource == AimSource::Mouse) {
                if (firstMouse >= 0) conflictingMouse = 2;
                else firstMouse = 2;
            }
            if (conflictingMouse >= 0) {
                selectFailure(profile, conflictingMouse);
                setFeedback(Localization::text(QStringLiteral(
                    "Одна мышь управляет одним устройством. Для другого выберите левый стик.")), true);
                return false;
            }
        }
    }
    for (int key : dirty_) {
        const int profile = key / int(MaxControllerPlayers);
        const ConsoleProfile system = ConsoleProfile(profile);
        const ControllerAdapter adapter = adapterDrafts_.contains(profile)
            ? adapterDrafts_.value(profile) : input_.adapter(system);
        const PlayerMapping& mapping = drafts_[key];
        unsigned jcartBase = 0;
        if (system == ConsoleProfile::MegaDrive &&
            system == input_.profile() && input_.jcart()) {
            if (adapter != ControllerAdapter::None)
                jcartBase = adapterPlayerCount(adapter);
            else {
                const int parentKey = profile * int(MaxControllerPlayers) + 1;
                const PlayerMapping parent = drafts_.contains(parentKey)
                    ? drafts_.value(parentKey) : input_.ordinaryMapping(system, 1);
                const PlayerMapping chain = drafts_.contains(parentKey + 1)
                    ? drafts_.value(parentKey + 1) : input_.ordinaryMapping(system, 2);
                jcartBase = parent.connected &&
                    parent.controllerType == ControllerType::Justifier && chain.connected ? 3u : 2u;
            }
        }
        const int player = key % int(MaxControllerPlayers);
        const bool jcartExtra = jcartBase && player >= int(jcartBase) &&
            player < int(jcartBase + 2);
        const ControllerType type = adapter == ControllerAdapter::None && !jcartExtra
            ? mapping.controllerType : ControllerType::Pad;
        const auto targets = InputManager::gameTargets(system, mapping.sixButton, type);
        QSet<int> keyboard;
        for (auto it = mapping.keyboard.cbegin(); it != mapping.keyboard.cend(); ++it) {
            const int assigned = it.value();
            if (!targets.contains(it.key()) || !assigned) continue;
            if (reservedKey(assigned) || keyboard.contains(assigned)) {
                selectFailure(profile, player);
                setFeedback(Localization::text(QStringLiteral("Проверьте клавиши: есть повтор или "
                                           "зарезервированная клавиша.")), true);
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
                    selectFailure(profile, player, it.key() >= 16);
                    setFeedback(Localization::text(QStringLiteral("Проверьте геймпад: один элемент "
                                               "назначен двум действиям.")), true);
                    return false;
                }
            }
            pad.append(assigned);
        }
        if (type != ControllerType::Pad) {
            QSet<int> mouseButtons;
            for (int target : targets) {
                const int button = mapping.mouse.value(int(type)).value(target, 0);
                if (!button) continue;
                if (mouseButtons.contains(button)) {
                    selectFailure(profile, player);
                    setFeedback(Localization::text(QStringLiteral("Проверьте мышь: одна кнопка назначена двум действиям.")), true);
                    return false;
                }
                mouseButtons.insert(button);
            }
        }
    }
    return true;
}

void ControlsPage::setTopologyLocked(bool locked) {
    setHardwareConfigurationLocked(locked);
}

void ControlsPage::setHardwareConfigurationLocked(bool locked) {
    adapterBox_->setEnabled(!locked);
    deviceTypeBox_->setEnabled(!locked);
    controllerTypeBox_->setEnabled(!locked);
    secondJustifierBox_->setEnabled(!locked);
    const QString hint = locked
        ? Localization::text(QStringLiteral("Тип и подключение контроллеров заданы записью управления до её остановки."))
        : QString();
    adapterBox_->setToolTip(hint);
    deviceTypeBox_->setToolTip(hint);
    controllerTypeBox_->setToolTip(hint);
    secondJustifierBox_->setToolTip(hint);
}

bool ControlsPage::apply() {
    if (captureTarget_ >= 0 || !sequence_.isEmpty() || input_.capturing()) {
        setFeedback(Localization::text(QStringLiteral("Завершите или отмените текущее назначение.")), true);
        return false;
    }
    if (!validateDrafts())
        return false;
    for (int profile : dirtyAdapters_)
        input_.setAdapter(ConsoleProfile(profile), adapterDrafts_[profile]);
    dirtyAdapters_.clear();
    for (int key : dirty_)
        input_.setMapping(static_cast<ConsoleProfile>(key / int(MaxControllerPlayers)),
                          key % int(MaxControllerPlayers), drafts_[key]);
    dirty_.clear();
    input_.setBackgroundGamepadEnabled(backgroundGamepadBox_->isChecked());
    input_.setMouseCaptureEnabled(mouseCaptureBox_->isChecked());
    return true;
}

bool ControlsPage::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonDblClick &&
        watched->property("bindingTarget").isValid()) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!isVisible() || !widget || widget->window() != window())
            return QWidget::eventFilter(watched, event);
        stopCapture();
        beginCaptureFor(watched->property("bindingTarget").toInt());
        return true;
    }
    // Release remembered keys if focus leaves the application before key-up.
    if (event->type() == QEvent::ApplicationDeactivate) {
        captureKeysDown_.clear();
        if (captureTarget_ >= 0 || !sequence_.isEmpty()) {
            stopCapture();
            setFeedback(Localization::text(QStringLiteral("Назначение отменено.")));
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
        setFeedback(Localization::text(QStringLiteral("Назначение отменено.")));
        return true;
    }
    if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt ||
        key == Qt::Key_AltGr || key == Qt::Key_Meta || key == Qt::Key_unknown)
        return true;
    if (captureTarget_ >= 16) {
        setFeedback(Localization::text(QStringLiteral("У команд фиксированные сочетания клавиатуры. Нажмите кнопку геймпада или Esc.")), true);
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
        setFeedback(Localization::text(QStringLiteral("Назначение отменено.")));
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
