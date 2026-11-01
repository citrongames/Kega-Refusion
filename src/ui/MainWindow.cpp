#include "MainWindow.h"
#include "AudioOutput.h"
#include "ControlsDialog.h"
#include "../core/EmulationSession.h"
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
    view_->setAcceptDrops(true);
    view_->installEventFilter(this);
    setCentralWidget(view_);
    audio_ = new AudioOutput(this);
    input_ = new InputManager(settings_.fileName(), this);
    connect(audio_, &AudioOutput::unavailable, this, [this](const QString& text) {
        view_->showOsd(text, 7000, QStringLiteral("audio-error"));
    });
    connect(input_, &InputManager::maskChanged, this, [this](unsigned port, unsigned mask) {
        session_.setButtons(port, mask);
    });
    connect(input_, &InputManager::commandChanged, this, &MainWindow::handleInputCommand);
    connect(input_, &InputManager::notification, this, [this](const QString& text) {
        view_->showOsd(text, 3500, QStringLiteral("device"));
    });

    recentPaths_ = settings_.value(QStringLiteral("recent/files")).toStringList();
    stateSlot_ = qBound(0, settings_.value(QStringLiteral("emulation/slot"), 0).toInt(), 9);
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
    connect(&session_, &EmulationSession::gameLoaded, this, [this](const QString& path, double fps) {
        loading_ = false;
        currentRom_ = path;
        input_->setProfile(profileFor(QFileInfo(path)));
        setWindowTitle(QStringLiteral("%1 — Kega Refusion").arg(displayName(path)));
        recentPaths_.removeAll(path);
        recentPaths_.prepend(path);
        while (recentPaths_.size() > 10) recentPaths_.removeLast();
        settings_.setValue(QStringLiteral("recent/files"), recentPaths_);
        updateRecentMenu();
        view_->showOsd(QStringLiteral("%1 · %2 кадров/с").arg(QFileInfo(path).fileName())
                       .arg(fps, 0, 'f', 2), 4000, QStringLiteral("game"));
        view_->setFocus();
        updateInputEnabled();
    });
    connect(&session_, &EmulationSession::pausedChanged, this, [this](bool paused) {
        setPausedUi(paused);
        if (!paused) view_->showOsd(QStringLiteral("Продолжено"), 1800, QStringLiteral("pause"));
    });
    connect(&session_, &EmulationSession::notification, this, [this](const QString& text) {
        view_->showOsd(text, 4200, QStringLiteral("core"));
    });
    connect(&session_, &EmulationSession::errorOccurred, this, [this](const QString& text) {
        loading_ = false;
        view_->showOsd(text, 9000, QStringLiteral("error"));
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
            QStringLiteral("Игры Mega Drive и Master System (*.md *.gen *.bin *.smd *.sms *.gg *.sg *.68k);;Все файлы (*)"));
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
        view_->showOsd(QStringLiteral("Сброс"), 2200, QStringLiteral("reset"));
    });
    fastForwardAction_ = emulationMenu->addAction(QStringLiteral("Ускорение (Tab)"));
    fastForwardAction_->setCheckable(true);
    fastForwardAction_->setEnabled(false);
    connect(fastForwardAction_, &QAction::toggled, this, &MainWindow::updateFastForward);
    emulationMenu->addSeparator();
    saveAction_ = emulationMenu->addAction(QStringLiteral("Сохранить состояние"));
    saveAction_->setShortcut(QKeySequence(QStringLiteral("F5")));
    saveAction_->setAutoRepeat(false);
    saveAction_->setEnabled(false);
    shortcut(saveAction_);
    connect(saveAction_, &QAction::triggered, this, [this] {
        session_.saveState(stateSlot_);
        view_->showOsd(QStringLiteral("Сохранение: слот %1").arg(stateSlot_), 2200,
                       QStringLiteral("state"));
    });
    loadAction_ = emulationMenu->addAction(QStringLiteral("Загрузить состояние"));
    loadAction_->setShortcut(QKeySequence(QStringLiteral("F8")));
    loadAction_->setAutoRepeat(false);
    loadAction_->setEnabled(false);
    shortcut(loadAction_);
    connect(loadAction_, &QAction::triggered, this, [this] {
        session_.loadState(stateSlot_);
        view_->showOsd(QStringLiteral("Загрузка: слот %1").arg(stateSlot_), 2200,
                       QStringLiteral("state"));
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
        saveAction_->setEnabled(true);
        loadAction_->setEnabled(true);
    });

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("&Вид"));
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
            view_->showOsd(QStringLiteral("Формат: %1").arg(QString::fromUtf8(choice.label)),
                           2500, QStringLiteral("aspect"));
        });
    }
    QAction* integerAction = viewMenu->addAction(QStringLiteral("Целочисленный масштаб"));
    integerAction->setCheckable(true);
    integerAction->setChecked(settings_.value(QStringLiteral("view/integer"), false).toBool());
    connect(integerAction, &QAction::toggled, this, [this](bool enabled) {
        view_->setIntegerScaling(enabled);
        settings_.setValue(QStringLiteral("view/integer"), enabled);
        view_->showOsd(enabled ? QStringLiteral("Масштаб: целый") :
                       QStringLiteral("Масштаб: свободный"), 2500, QStringLiteral("scale"));
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
        view_->showOsd(muted ? QStringLiteral("Звук выключен") :
                       QStringLiteral("Звук включен"), 2400, QStringLiteral("sound"));
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
        view_->showOsd(QStringLiteral("Громкость: %1%").arg(value), 1800,
                       QStringLiteral("volume"));
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
            "Поддержка CD, 32X и ZIP пока не подготовлена.<br>"
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
    view_->showOsd(QStringLiteral("Слот: %1").arg(stateSlot_), 2400, QStringLiteral("slot"));
}

void MainWindow::openRom(const QString& path) {
    const QFileInfo file(path);
    if (!file.exists() || !file.isFile()) {
        const QString text = QStringLiteral("Файл не найден: %1").arg(path);
        view_->showOsd(text, 8000, QStringLiteral("error"));
        if (!smokeTest_) QMessageBox::warning(this, QStringLiteral("Kega Refusion"), text);
        return;
    }
    releaseInput();
    loading_ = true;
    settings_.setValue(QStringLiteral("files/lastDirectory"), file.absolutePath());
    view_->showOsd(QStringLiteral("Загрузка: %1").arg(file.fileName()), 4000,
                   QStringLiteral("game"));
    session_.loadGame(file.absoluteFilePath());
}

void MainWindow::editControls() {
    releaseInput();
    input_->setEnabled(false);
    ControlsDialog dialog(*input_, this);
    const bool saved = dialog.exec() == QDialog::Accepted;
    updateInputEnabled();
    view_->setFocus();
    if (saved) view_->showOsd(QStringLiteral("Управление обновлено"), 2000, QStringLiteral("controls"));
}

void MainWindow::releaseInput() {
    fastForwardHeld_ = false;
    fastForwardPadHeld_ = false;
    input_->releaseKeyboard();
    input_->setEnabled(false);
    if (fastForwardAction_ && fastForwardAction_->isChecked())
        fastForwardAction_->setChecked(false);
    updateFastForward();
}

void MainWindow::updateInputEnabled() {
    const bool enabled = isActiveWindow() && view_->hasFocus() && session_.hasGame() && !loading_ &&
                         !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
    input_->setEnabled(enabled);
    input_->poll();
}

void MainWindow::updateFastForward() {
    const bool enabled = fastForwardHeld_ || fastForwardPadHeld_ ||
                         (fastForwardAction_ && fastForwardAction_->isChecked());
    if (fastForwardActive_ == enabled) return;
    fastForwardActive_ = enabled;
    if (enabled) audio_->clear();
    session_.setFastForward(enabled);
    view_->setFastForwardStatus(enabled);
    if (!enabled && session_.hasGame())
        view_->showOsd(QStringLiteral("Обычная скорость"), 1400, QStringLiteral("speed"));
}

void MainWindow::setPausedUi(bool paused) {
    if (pauseAction_) {
        const QSignalBlocker blocker(pauseAction_);
        pauseAction_->setChecked(paused);
    }
    view_->setPausedStatus(paused);
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
        view_->showOsd(QStringLiteral("Полный экран"), 2100, QStringLiteral("fullscreen"));
    } else {
        menuBar()->show();
        if (wasMaximizedBeforeFullscreen_) showMaximized();
        else showNormal();
        view_->showOsd(QStringLiteral("Оконный режим"), 2100, QStringLiteral("fullscreen"));
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
    view_->showOsd(visible ? QStringLiteral("Меню показано") :
                   QStringLiteral("Меню скрыто"), 1700, QStringLiteral("menu"));
    view_->setFocus();
}

void MainWindow::showOsdDemo() {
    view_->setPausedStatus(true);
    view_->setFastForwardStatus(true);
    view_->showOsd(QStringLiteral("Сохранено: слот 9"), 4000, QStringLiteral("demo"));
    view_->showOsd(QStringLiteral("Громкость: 80%"), 4000, QStringLiteral("volume"));
}

void MainWindow::handleInputCommand(int command, bool pressed) {
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
            view_->showOsd(QStringLiteral("Меню"), 1500, QStringLiteral("menu"));
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
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease)
        return QMainWindow::eventFilter(watched, event);
    if (QApplication::activeModalWidget() || QApplication::activePopupWidget() || !isActiveWindow())
        return QMainWindow::eventFilter(watched, event);
    auto* keyEvent = static_cast<QKeyEvent*>(event);
    if (keyEvent->isAutoRepeat()) return QMainWindow::eventFilter(watched, event);
    const bool pressed = event->type() == QEvent::KeyPress;
    const Qt::KeyboardModifiers modifiers = keyEvent->modifiers();
    const int key = keyEvent->key();
    if (key == Qt::Key_Escape && modifiers == Qt::NoModifier && isFullScreen()) {
        if (pressed) toggleFullscreenMenu();
        return true;
    }
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) &&
        (modifiers & Qt::AltModifier) &&
        !(modifiers & (Qt::ControlModifier | Qt::ShiftModifier | Qt::MetaModifier))) {
        if (pressed) setFullscreenMode(!isFullScreen());
        return true;
    }
    if (!session_.hasGame() || loading_ || !view_->hasFocus())
        return QMainWindow::eventFilter(watched, event);
    if (key == Qt::Key_Tab) {
        if (!pressed && fastForwardHeld_) {
            fastForwardHeld_ = false;
            updateFastForward();
            return true;
        }
        if (pressed && modifiers == Qt::NoModifier) {
            fastForwardHeld_ = true;
            updateFastForward();
            return true;
        }
    }
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
