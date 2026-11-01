#include "MainWindow.h"
#include "AudioOutput.h"
#include "ControlsDialog.h"
#include "../core/EmulationSession.h"
#include "../core/RomArchive.h"
#include "../input/InputManager.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QWindowStateChangeEvent>

namespace {
QString displayName(const QString& path) { return QFileInfo(path).completeBaseName(); }
ConsoleProfile profileFor(const QFileInfo& file) {
    const QString ext = file.suffix().toLower();
    if (ext == QLatin1String("sms")) return ConsoleProfile::MasterSystem;
    if (ext == QLatin1String("gg")) return ConsoleProfile::GameGear;
    if (ext == QLatin1String("sg")) return ConsoleProfile::SG1000;
    return ConsoleProfile::MegaDrive;
}
}

MainWindow::MainWindow(EmulationSession& session, const QString& dataPath,
                       bool smokeTest, QWidget* parent)
    : QMainWindow(parent), session_(session),
      settings_(QDir(dataPath).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat),
      smokeTest_(smokeTest) {
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(QStringLiteral("Kega Refusion"));
    setMinimumSize(640, 440);
    resize(920, 620);
    setAcceptDrops(true);
    view_ = new GameView(this);
    view_->setOsdEnabled(settings_.value(QStringLiteral("view/osd"), true).toBool());
    view_->setAcceptDrops(true);
    view_->installEventFilter(this);
    setCentralWidget(view_);
    audio_ = new AudioOutput(this);
    input_ = new InputManager(settings_.fileName(), this);
    connect(audio_, &AudioOutput::unavailable, this, [](const QString& text) {
        qWarning("%s", qPrintable(text));
    });
    connect(input_, &InputManager::maskChanged, this, [this](unsigned port, unsigned mask) {
        session_.setButtons(port, mask);
    });
    connect(input_, &InputManager::commandChanged, this, &MainWindow::handleInputCommand);
    connect(input_, &InputManager::notification, this, [](const QString& text) {
        qInfo("%s", qPrintable(text));
    });

    recentPaths_ = settings_.value(QStringLiteral("recent/files")).toStringList();
    stateSlot_ = qBound(0, settings_.value(QStringLiteral("emulation/slot"), 0).toInt(), 9);
    fastForwardFactor_ = settings_.value(QStringLiteral("emulation/fastForwardFactor"), 4).toInt();
    if (!QList<int>{2,3,4,6,8}.contains(fastForwardFactor_)) fastForwardFactor_ = 4;
    rewindSeconds_ = settings_.value(QStringLiteral("emulation/rewindSeconds"), 30).toInt();
    if (!QList<int>{0,10,30,60}.contains(rewindSeconds_)) rewindSeconds_ = 30;
    pauseOnFocusLoss_ = settings_.value(QStringLiteral("emulation/pauseOnFocusLoss"), true).toBool();
    session_.setFastForwardFactor(fastForwardFactor_);
    session_.setRewindSeconds(rewindSeconds_);
    view_->setFastForwardFactor(fastForwardFactor_);
    buildMenus();
    const int aspect = settings_.value(QStringLiteral("view/aspect"), 0).toInt();
    view_->setAspectMode(aspect == 1 ? GameView::AspectMode::SquarePixels :
                         aspect == 2 ? GameView::AspectMode::Stretch : GameView::AspectMode::Console);
    view_->setIntegerScaling(settings_.value(QStringLiteral("view/integer"), false).toBool());
    const QByteArray geometry = settings_.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);

    connect(&session_, &EmulationSession::videoFrame, view_, [this](const QImage& image, double ratio) {
        view_->setFrame(image, ratio);
    });
    connect(&session_, &EmulationSession::audioSamples, audio_, [this](const QByteArray& pcm, int rate) {
        audio_->play(pcm, rate);
    });
    connect(&session_, &EmulationSession::audioReset, audio_, &AudioOutput::clear);
    connect(&session_, &EmulationSession::gameLoaded, this, [this](const QString& path, double, const QString& contentName) {
        loading_ = false;
        currentRom_ = path;
        input_->setProfile(profileFor(QFileInfo(contentName)));
        setWindowTitle(QStringLiteral("%1 — Kega Refusion").arg(displayName(contentName)));
        recentPaths_.removeAll(path);
        recentPaths_.prepend(path);
        while (recentPaths_.size() > 10) recentPaths_.removeLast();
        settings_.setValue(QStringLiteral("recent/files"), recentPaths_);
        updateRecentMenu();
        view_->setFocus();
        updateInputEnabled();
    });
    connect(&session_, &EmulationSession::pausedChanged, this, [this](bool paused) {
        setPausedUi(paused);
    });
    connect(&session_, &EmulationSession::effectivePauseChanged, view_, &GameView::setPausedStatus);
    connect(&session_, &EmulationSession::rewindingChanged, view_, &GameView::setRewindStatus);
    connect(&session_, &EmulationSession::stateSlotsChanged, this, [this](int selected, unsigned occupied) {
        occupiedSlots_ = occupied;
        if (selected >= 0 && selected == stateSlot_ && session_.hasGame())
            view_->showStateSlots(stateSlot_, occupiedSlots_);
    });
    connect(&session_, &EmulationSession::notification, this, [](const QString& text) {
        qWarning("%s", qPrintable(text));
    });
    connect(&session_, &EmulationSession::actionCompleted, this, [this](const QString& text) {
        view_->showOsd(text, 2000, QStringLiteral("action"));
    });
    connect(&session_, &EmulationSession::actionNotice, this, [this](const QString& text) {
        view_->showOsd(text, 2000, QStringLiteral("action"));
    });
    connect(&session_, &EmulationSession::errorOccurred, this, [this](const QString& text) {
        loading_ = false;
        updateInputEnabled();
        if (!smokeTest_) QMessageBox::warning(this, QStringLiteral("Kega Refusion"), text);
    });

    inputTimer_ = new QTimer(this);
    inputTimer_->setInterval(8);
    connect(inputTimer_, &QTimer::timeout, this, &MainWindow::updateInputEnabled);
    inputTimer_->start();
    qApp->installEventFilter(this);
}

void MainWindow::buildMenus() {
    auto shortcut = [this](QAction* action) {
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
    };
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&Файл"));
    QAction* openAction = fileMenu->addAction(QStringLiteral("Открыть игру…"));
    openAction->setShortcut(QKeySequence::Open);
    shortcut(openAction);
    connect(openAction, &QAction::triggered, this, [this] {
        releaseInput();
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Открыть игру"),
            settings_.value(QStringLiteral("files/lastDirectory"), QDir::homePath()).toString(),
            QStringLiteral("Игры Mega Drive и Master System (*.md *.gen *.bin *.smd *.sms *.gg *.sg *.68k *.zip *.7z);;Архивы (*.zip *.7z);;Все файлы (*)"));
        if (!path.isEmpty()) openRom(path);
    });
    recentMenu_ = fileMenu->addMenu(QStringLiteral("Недавние игры"));
    updateRecentMenu();
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(QStringLiteral("Выход"));
    quitAction->setShortcut(QKeySequence::Quit);
    shortcut(quitAction);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu* emulationMenu = menuBar()->addMenu(QStringLiteral("&Эмуляция"));
    pauseAction_ = emulationMenu->addAction(QStringLiteral("Пауза"));
    pauseAction_->setCheckable(true);
    pauseAction_->setShortcut(QKeySequence(QStringLiteral("P")));
    pauseAction_->setAutoRepeat(false);
    pauseAction_->setEnabled(false);
    shortcut(pauseAction_);
    connect(pauseAction_, &QAction::toggled, this, [this](bool paused) {
        session_.setPaused(paused);
    });
    resetAction_ = emulationMenu->addAction(QStringLiteral("Сброс"));
    resetAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    resetAction_->setAutoRepeat(false);
    resetAction_->setEnabled(false);
    shortcut(resetAction_);
    connect(resetAction_, &QAction::triggered, this, [this] {
        releaseInput();
        session_.resetGame();
    });
    fastForwardAction_ = emulationMenu->addAction(QStringLiteral("Ускорение (Tab)"));
    fastForwardAction_->setCheckable(true);
    fastForwardAction_->setEnabled(false);
    connect(fastForwardAction_, &QAction::toggled, this, &MainWindow::updateFastForward);
    QMenu* speedMenu = emulationMenu->addMenu(QStringLiteral("Скорость ускорения"));
    auto* speedGroup = new QActionGroup(this);
    for (int factor : {2,3,4,6,8}) {
        QAction* action = speedMenu->addAction(QStringLiteral("×%1").arg(factor));
        action->setCheckable(true); action->setChecked(factor == fastForwardFactor_);
        speedGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, factor] {
            fastForwardFactor_ = factor;
            settings_.setValue(QStringLiteral("emulation/fastForwardFactor"), factor);
            session_.setFastForwardFactor(factor); view_->setFastForwardFactor(factor);
        });
    }
    rewindAction_ = emulationMenu->addAction(QStringLiteral("Перемотка назад (R)"));
    rewindAction_->setCheckable(true); rewindAction_->setEnabled(false);
    connect(rewindAction_, &QAction::toggled, this, &MainWindow::updateRewind);
    QMenu* historyMenu = emulationMenu->addMenu(QStringLiteral("История перемотки"));
    auto* historyGroup = new QActionGroup(this);
    for (int seconds : {0,10,30,60}) {
        QAction* action = historyMenu->addAction(seconds ? QStringLiteral("%1 с").arg(seconds) : QStringLiteral("Выключена"));
        action->setCheckable(true); action->setChecked(seconds == rewindSeconds_);
        historyGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, seconds] {
            rewindSeconds_ = seconds;
            settings_.setValue(QStringLiteral("emulation/rewindSeconds"), seconds);
            session_.setRewindSeconds(seconds);
            if (!seconds) { rewindHeld_ = rewindPadHeld_ = false; rewindAction_->setChecked(false); }
            rewindAction_->setEnabled(session_.hasGame() && seconds > 0);
            updateRewind();
        });
    }
    QAction* focusPause = emulationMenu->addAction(QStringLiteral("Пауза при потере фокуса"));
    focusPause->setCheckable(true); focusPause->setChecked(pauseOnFocusLoss_);
    connect(focusPause, &QAction::toggled, this, [this](bool enabled) {
        pauseOnFocusLoss_ = enabled;
        settings_.setValue(QStringLiteral("emulation/pauseOnFocusLoss"), enabled);
        updateInputEnabled();
    });
    emulationMenu->addSeparator();
    saveAction_ = emulationMenu->addAction(QStringLiteral("Сохранить состояние"));
    saveAction_->setShortcut(QKeySequence(QStringLiteral("F5")));
    saveAction_->setAutoRepeat(false);
    saveAction_->setEnabled(false);
    shortcut(saveAction_);
    connect(saveAction_, &QAction::triggered, this, [this] {
        session_.saveState(stateSlot_);
    });
    loadAction_ = emulationMenu->addAction(QStringLiteral("Загрузить состояние"));
    loadAction_->setShortcut(QKeySequence(QStringLiteral("F8")));
    loadAction_->setAutoRepeat(false);
    loadAction_->setEnabled(false);
    shortcut(loadAction_);
    connect(loadAction_, &QAction::triggered, this, [this] {
        releaseInput();
        session_.loadState(stateSlot_);
    });
    QMenu* slotMenu = emulationMenu->addMenu(QStringLiteral("Слот сохранения"));
    QActionGroup* slotGroup = new QActionGroup(this);
    slotGroup->setExclusive(true);
    for (int slot = 0; slot < 10; ++slot) {
        QAction* action = slotMenu->addAction(QString::number(slot));
        action->setCheckable(true);
        action->setChecked(slot == stateSlot_);
        slotGroup->addAction(action);
        slotActions_.append(action);
        connect(action, &QAction::triggered, this, [this, slot] { setSlot(slot); });
    }
    connect(&session_, &EmulationSession::gameLoaded, this, [this](const QString&, double) {
        pauseAction_->setEnabled(true);
        resetAction_->setEnabled(true);
        fastForwardAction_->setEnabled(true);
        rewindAction_->setEnabled(rewindSeconds_ > 0);
        saveAction_->setEnabled(true);
        loadAction_->setEnabled(true);
    });

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("&Вид"));
    QAction* osdAction = viewMenu->addAction(QStringLiteral("Экранные сообщения (OSD)"));
    osdAction->setCheckable(true);
    osdAction->setChecked(settings_.value(QStringLiteral("view/osd"), true).toBool());
    connect(osdAction, &QAction::toggled, this, [this](bool enabled) {
        settings_.setValue(QStringLiteral("view/osd"), enabled);
        view_->setOsdEnabled(enabled);
    });
    QMenu* aspectMenu = viewMenu->addMenu(QStringLiteral("Соотношение сторон"));
    QActionGroup* aspectGroup = new QActionGroup(this);
    aspectGroup->setExclusive(true);
    struct AspectChoice { const char* label; GameView::AspectMode mode; int value; };
    const AspectChoice choices[] = {
        {"Как на консоли", GameView::AspectMode::Console, 0},
        {"Квадратные пиксели", GameView::AspectMode::SquarePixels, 1},
        {"Растянуть", GameView::AspectMode::Stretch, 2},
    };
    const int aspect = settings_.value(QStringLiteral("view/aspect"), 0).toInt();
    for (const auto& choice : choices) {
        QAction* action = aspectMenu->addAction(QString::fromUtf8(choice.label));
        action->setCheckable(true);
        action->setChecked(choice.value == aspect);
        aspectGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, choice] {
            view_->setAspectMode(choice.mode);
            settings_.setValue(QStringLiteral("view/aspect"), choice.value);
        });
    }
    QAction* integerAction = viewMenu->addAction(QStringLiteral("Целочисленный масштаб"));
    integerAction->setCheckable(true);
    integerAction->setChecked(settings_.value(QStringLiteral("view/integer"), false).toBool());
    connect(integerAction, &QAction::toggled, this, [this](bool enabled) {
        view_->setIntegerScaling(enabled);
        settings_.setValue(QStringLiteral("view/integer"), enabled);
    });
    viewMenu->addSeparator();
    fullscreenAction_ = viewMenu->addAction(QStringLiteral("Полный экран"));
    fullscreenAction_->setShortcut(QKeySequence(QStringLiteral("Alt+Return")));
    fullscreenAction_->setAutoRepeat(false);
    fullscreenAction_->setCheckable(true);
    shortcut(fullscreenAction_);
    connect(fullscreenAction_, &QAction::toggled, this, &MainWindow::setFullscreenMode);

    QMenu* soundMenu = menuBar()->addMenu(QStringLiteral("&Звук"));
    muteAction_ = soundMenu->addAction(QStringLiteral("Без звука"));
    muteAction_->setCheckable(true);
    muteAction_->setShortcut(QKeySequence(QStringLiteral("M")));
    muteAction_->setAutoRepeat(false);
    muteAction_->setChecked(settings_.value(QStringLiteral("audio/muted"), false).toBool());
    shortcut(muteAction_);
    audio_->setMuted(muteAction_->isChecked());
    connect(muteAction_, &QAction::toggled, this, [this](bool muted) {
        audio_->setMuted(muted);
        settings_.setValue(QStringLiteral("audio/muted"), muted);
    });
    QWidget* volumePanel = new QWidget(soundMenu);
    QVBoxLayout* volumeLayout = new QVBoxLayout(volumePanel);
    volumeLayout->setContentsMargins(12, 6, 12, 8);
    volumeLayout->addWidget(new QLabel(QStringLiteral("Громкость"), volumePanel));
    QSlider* volumeSlider = new QSlider(Qt::Horizontal, volumePanel);
    volumeSlider->setRange(0, 100);
    volumeSlider->setMinimumWidth(160);
    volumeSlider->setValue(qBound(0, settings_.value(QStringLiteral("audio/volume"), 80).toInt(), 100));
    audio_->setVolume(volumeSlider->value());
    volumeLayout->addWidget(volumeSlider);
    QWidgetAction* volumeAction = new QWidgetAction(soundMenu);
    volumeAction->setDefaultWidget(volumePanel);
    soundMenu->addAction(volumeAction);
    connect(volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        audio_->setVolume(value);
        settings_.setValue(QStringLiteral("audio/volume"), value);
    });

    QMenu* settingsMenu = menuBar()->addMenu(QStringLiteral("&Настройки"));
    QAction* controlsAction = settingsMenu->addAction(QStringLiteral("Управление…"));
    connect(controlsAction, &QAction::triggered, this, &MainWindow::editControls);
    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Справка"));
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("О программе"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this, QStringLiteral("О Kega Refusion"),
            QStringLiteral("<b>Kega Refusion</b><br>Компактный эмулятор на базе Genesis Plus GX (Libretro)."
            "<br><br>Игры: MD, GEN, BIN, SMD, SMS, GG, SG, 68K. "
            "Поддерживаются архивы ZIP и 7z. CD и 32X пока не подготовлены.<br>"
            "Состояния сохраняются в выбранный слот от 0 до 9.<br>"
            "Клавиатуру и геймпады можно настроить в разделе «Управление»."));
    });
}

void MainWindow::updateRecentMenu() {
    recentMenu_->clear();
    QStringList existing;
    for (const QString& path : recentPaths_) {
        if (!QFileInfo::exists(path)) continue;
        existing.append(path);
        QAction* action = recentMenu_->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path] { openRom(path); });
    }
    recentPaths_ = existing;
    recentMenu_->setEnabled(!existing.isEmpty());
}

void MainWindow::setSlot(int slot) {
    stateSlot_ = (slot + 10) % 10;
    settings_.setValue(QStringLiteral("emulation/slot"), stateSlot_);
    if (stateSlot_ < slotActions_.size()) slotActions_[stateSlot_]->setChecked(true);
    if (session_.hasGame()) session_.requestSlotStatus(stateSlot_);
}

void MainWindow::openRom(const QString& path) {
    const QFileInfo file(path);
    if (!file.exists() || !file.isFile()) {
        const QString text = QStringLiteral("Файл не найден: %1").arg(path);
        if (!smokeTest_) QMessageBox::warning(this, QStringLiteral("Kega Refusion"), text);
        return;
    }
    releaseInput();
    int archiveIndex = -1;
    if (RomArchive::isArchive(path)) {
        QString error;
        const auto entries = RomArchive::list(path, &error);
        if (!error.isEmpty() || entries.isEmpty()) {
            if (!smokeTest_) QMessageBox::warning(this, QStringLiteral("Kega Refusion"),
                error.isEmpty() ? QStringLiteral("В архиве нет поддерживаемых игр.") : error);
            // Let the session report the same failure to the smoke-test observer.
            if (smokeTest_) session_.loadGame(path);
            return;
        }
        if (entries.size() == 1) archiveIndex = entries.first().index;
        else if (!smokeTest_) {
            QStringList choices;
            for (const auto& entry : entries)
                choices.append(QStringLiteral("%1 (%2 КиБ) [%3]").arg(entry.name).arg((entry.size+1023)/1024).arg(entry.index));
            bool accepted = false;
            const QString choice = QInputDialog::getItem(this, QStringLiteral("Игра в архиве"),
                QStringLiteral("Выберите ROM"), choices, 0, false, &accepted);
            if (!accepted) { view_->setFocus(); return; }
            archiveIndex = entries[choices.indexOf(choice)].index;
        }
    }
    loading_ = true;
    settings_.setValue(QStringLiteral("files/lastDirectory"), file.absolutePath());
    session_.loadGame(file.absoluteFilePath(), archiveIndex);
}

void MainWindow::editControls() {
    releaseInput();
    input_->setEnabled(false);
    ControlsDialog dialog(*input_, this);
    dialog.exec();
    updateInputEnabled();
    view_->setFocus();
}

void MainWindow::releaseInput() {
    fastForwardHeld_ = false;
    fastForwardPadHeld_ = false;
    rewindHeld_ = rewindPadHeld_ = false;
    if (rewindAction_ && rewindAction_->isChecked()) rewindAction_->setChecked(false);
    updateRewind();
    input_->releaseKeyboard();
    input_->setEnabled(false);
    if (fastForwardAction_ && fastForwardAction_->isChecked())
        fastForwardAction_->setChecked(false);
    updateFastForward();
}

void MainWindow::updateInputEnabled() {
    session_.setBackgroundPaused(pauseOnFocusLoss_ &&
        (!isActiveWindow() || QApplication::applicationState() != Qt::ApplicationActive));
    const bool enabled = isActiveWindow() && view_->hasFocus() && session_.hasGame() && !loading_ &&
                         !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
    input_->setEnabled(enabled);
    input_->poll();
}

void MainWindow::updateFastForward() {
    const bool enabled = fastForwardHeld_ || fastForwardPadHeld_ ||
                         (fastForwardAction_ && fastForwardAction_->isChecked());
    view_->setFastForwardStatus(enabled && !(rewindSeconds_ > 0 && (rewindHeld_ || rewindPadHeld_ ||
        (rewindAction_ && rewindAction_->isChecked()))));
    if (fastForwardActive_ == enabled) return;
    fastForwardActive_ = enabled;
    if (enabled) audio_->clear();
    session_.setFastForward(enabled);
}

void MainWindow::updateRewind() {
    const bool enabled = rewindSeconds_ > 0 && (rewindHeld_ || rewindPadHeld_ ||
        (rewindAction_ && rewindAction_->isChecked()));
    session_.setRewind(enabled);
    updateFastForward();
}

void MainWindow::setPausedUi(bool paused) {
    if (pauseAction_) {
        const QSignalBlocker blocker(pauseAction_);
        pauseAction_->setChecked(paused);
    }
}

void MainWindow::setFullscreenMode(bool enabled) {
    if (enabled == isFullScreen()) {
        if (fullscreenAction_) {
            const QSignalBlocker blocker(fullscreenAction_);
            fullscreenAction_->setChecked(enabled);
        }
        return;
    }
    if (enabled) {
        windowedGeometry_ = saveGeometry();
        wasMaximizedBeforeFullscreen_ = isMaximized();
        menuBar()->hide();
        showFullScreen();
    } else {
        menuBar()->show();
        if (wasMaximizedBeforeFullscreen_) showMaximized();
        else showNormal();
    }
    if (fullscreenAction_) {
        const QSignalBlocker blocker(fullscreenAction_);
        fullscreenAction_->setChecked(isFullScreen());
    }
    view_->setFocus();
}

void MainWindow::toggleFullscreenMenu() {
    if (!isFullScreen()) return;
    const bool visible = !menuBar()->isVisible();
    menuBar()->setVisible(visible);
    view_->setFocus();
}

void MainWindow::showOsdDemo() {
    view_->setPausedStatus(true);
    view_->setFastForwardStatus(true);
    view_->showStateSlots(3, (1u << 1) | (1u << 3) | (1u << 8), 4000);
}

void MainWindow::handleInputCommand(int command, bool pressed) {
    if (command == RewindCommand) { rewindPadHeld_ = pressed; updateRewind(); return; }
    if (command == FastForwardCommand) {
        fastForwardPadHeld_ = pressed;
        updateFastForward();
        return;
    }
    if (!pressed) return;
    switch (command) {
    case PauseCommand: if (pauseAction_->isEnabled()) pauseAction_->trigger(); break;
    case SaveCommand: if (saveAction_->isEnabled()) saveAction_->trigger(); break;
    case LoadCommand: if (loadAction_->isEnabled()) loadAction_->trigger(); break;
    case ResetCommand: if (resetAction_->isEnabled()) resetAction_->trigger(); break;
    case FullscreenCommand: fullscreenAction_->trigger(); break;
    case MuteCommand: muteAction_->trigger(); break;
    case NextSlotCommand: setSlot(stateSlot_ + 1); break;
    case PreviousSlotCommand: setSlot(stateSlot_ + 9); break;
    case MenuCommand:
        if (isFullScreen()) toggleFullscreenMenu();
        else {
            menuBar()->setFocus();
            menuBar()->setActiveAction(menuBar()->actions().first());
        }
        break;
    default: break;
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == this || watched == view_) &&
        (event->type() == QEvent::DragEnter || event->type() == QEvent::Drop)) {
        if (event->type() == QEvent::DragEnter)
            dragEnterEvent(static_cast<QDragEnterEvent*>(event));
        else
            dropEvent(static_cast<QDropEvent*>(event));
        return event->isAccepted();
    }
    if (event->type() == QEvent::ApplicationDeactivate ||
        (watched == view_ && event->type() == QEvent::FocusOut)) {
        releaseInput();
        return QMainWindow::eventFilter(watched, event);
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease &&
        event->type() != QEvent::ShortcutOverride)
        return QMainWindow::eventFilter(watched, event);
    if (QApplication::activeModalWidget() || QApplication::activePopupWidget() || !isActiveWindow())
        return QMainWindow::eventFilter(watched, event);
    auto* keyEvent = static_cast<QKeyEvent*>(event);
    const bool pressed = event->type() == QEvent::KeyPress;
    const bool shortcutOverride = event->type() == QEvent::ShortcutOverride;
    const Qt::KeyboardModifiers modifiers = keyEvent->modifiers() & ~Qt::KeypadModifier;
    const int key = InputManager::keyboardKey(keyEvent->key(), keyEvent->nativeScanCode());
    if (!shortcutOverride && !pressed && !keyEvent->isAutoRepeat() && key == Qt::Key_R && rewindHeld_) {
        rewindHeld_ = false; updateRewind(); return true;
    }
    // Claim these keys before Qt resolves the logical QAction shortcuts. A
    // shortcut and its physical counterpart must produce exactly one action.
    QAction* commandAction = nullptr;
    if (modifiers == Qt::NoModifier) {
        if (key == Qt::Key_P) commandAction = pauseAction_;
        else if (key == Qt::Key_M) commandAction = muteAction_;
        else if (key == Qt::Key_F5) commandAction = saveAction_;
        else if (key == Qt::Key_F8) commandAction = loadAction_;
    } else if (modifiers == Qt::ControlModifier && key == Qt::Key_R) {
        commandAction = resetAction_;
    }
    if (commandAction && commandAction->isEnabled()) {
        if (shortcutOverride) event->accept();
        else if (pressed && !keyEvent->isAutoRepeat()) commandAction->trigger();
        return true;
    }
    if (key == Qt::Key_Escape && modifiers == Qt::NoModifier && isFullScreen()) {
        if (shortcutOverride) event->accept();
        else if (pressed && !keyEvent->isAutoRepeat()) toggleFullscreenMenu();
        return true;
    }
    // Right Alt/AltGr arrives as Ctrl+Alt on Windows, or GroupSwitch on XKB.
    // Accept both representations for fullscreen, including keypad Enter.
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) &&
        (modifiers & (Qt::AltModifier | Qt::GroupSwitchModifier)) &&
        !(modifiers & (Qt::ShiftModifier | Qt::MetaModifier))) {
        if (shortcutOverride) event->accept();
        else if (pressed && !keyEvent->isAutoRepeat()) setFullscreenMode(!isFullScreen());
        return true;
    }
    if (!session_.hasGame() || loading_ || !view_->hasFocus())
        return QMainWindow::eventFilter(watched, event);
    if (key >= Qt::Key_0 && key <= Qt::Key_9 && modifiers == Qt::NoModifier) {
        if (shortcutOverride) event->accept();
        else if (pressed && !keyEvent->isAutoRepeat()) setSlot(key - Qt::Key_0);
        return true;
    }
    if (key == Qt::Key_R && (modifiers == Qt::NoModifier || (!pressed && rewindHeld_))) {
        if (shortcutOverride) { event->accept(); return true; }
        if (!keyEvent->isAutoRepeat()) { rewindHeld_ = pressed; updateRewind(); }
        return true;
    }
    if (key == Qt::Key_Tab) {
        if (shortcutOverride && modifiers == Qt::NoModifier) {
            event->accept();
            return true;
        }
        if (!shortcutOverride && !pressed && !keyEvent->isAutoRepeat() && fastForwardHeld_) {
            fastForwardHeld_ = false;
            updateFastForward();
            return true;
        }
        if (pressed && !keyEvent->isAutoRepeat() && modifiers == Qt::NoModifier) {
            fastForwardHeld_ = true;
            updateFastForward();
            return true;
        }
    }
    if (shortcutOverride) return QMainWindow::eventFilter(watched, event);
    if (keyEvent->isAutoRepeat()) return true;
    if (!pressed) {
        if (input_->keyEvent(key, false)) return true;
        return QMainWindow::eventFilter(watched, event);
    }
    if (modifiers != Qt::NoModifier || key == Qt::Key_P || key == Qt::Key_M ||
        key == Qt::Key_F5 || key == Qt::Key_F8)
        return QMainWindow::eventFilter(watched, event);
    if (input_->keyEvent(key, true)) return true;
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            openRom(url.toLocalFile());
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::ActivationChange && !isActiveWindow()) releaseInput();
    if (event->type() == QEvent::WindowStateChange) {
        const auto* change = static_cast<QWindowStateChangeEvent*>(event);
        if (fullscreenAction_) {
            const QSignalBlocker blocker(fullscreenAction_);
            fullscreenAction_->setChecked(isFullScreen());
        }
        if (isFullScreen() && !(change->oldState() & Qt::WindowFullScreen)) menuBar()->hide();
        else if (!isFullScreen()) menuBar()->show();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    releaseInput();
    settings_.setValue(QStringLiteral("window/geometry"),
                       isFullScreen() && !windowedGeometry_.isEmpty() ?
                           windowedGeometry_ : saveGeometry());
    settings_.sync();
    QMainWindow::closeEvent(event);
}
