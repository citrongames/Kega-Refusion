#include "MainWindow.h"

#include "AudioOutput.h"
#include "../core/EmulationSession.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace {
struct InputBinding {
    const char* label;
    unsigned id;
    int defaultKey;
};

constexpr InputBinding bindings[] = {
    {"Вверх", 4, Qt::Key_Up},
    {"Вниз", 5, Qt::Key_Down},
    {"Влево", 6, Qt::Key_Left},
    {"Вправо", 7, Qt::Key_Right},
    {"A", 1, Qt::Key_Z},
    {"B / 1", 0, Qt::Key_X},
    {"C / 2", 8, Qt::Key_C},
    {"X", 10, Qt::Key_A},
    {"Y", 9, Qt::Key_S},
    {"Z", 11, Qt::Key_D},
    {"Start", 3, Qt::Key_Return},
    {"Mode", 2, Qt::Key_Backspace},
};

QString keySetting(unsigned id) {
    return QStringLiteral("keys/button_%1").arg(id);
}

QString displayName(const QString& path) {
    return QFileInfo(path).completeBaseName();
}
} // namespace

MainWindow::MainWindow(EmulationSession& session, const QString& dataPath,
                       bool smokeTest, QWidget* parent)
    : QMainWindow(parent),
      session_(session),
      settings_(QDir(dataPath).filePath(QStringLiteral("settings.ini")),
                QSettings::IniFormat),
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
    connect(audio_, &AudioOutput::unavailable, this, [this](const QString& message) {
        statusBar()->showMessage(message, 10000);
    });
    recentPaths_ = settings_.value(QStringLiteral("recent/files")).toStringList();
    stateSlot_ = qBound(0, settings_.value(QStringLiteral("emulation/slot"), 0).toInt(), 9);

    for (const auto& binding : bindings) {
        const int key = settings_.value(keySetting(binding.id),
                                        binding.defaultKey).toInt();
        if (key != 0)
            keyToButton_.insert(key, binding.id);
    }
    buildMenus();

    const int aspect = settings_.value(QStringLiteral("view/aspect"), 0).toInt();
    view_->setAspectMode(aspect == 1 ? GameView::AspectMode::SquarePixels
                          : aspect == 2 ? GameView::AspectMode::Stretch
                                        : GameView::AspectMode::Console);
    view_->setIntegerScaling(settings_.value(QStringLiteral("view/integer"), false).toBool());

    const QByteArray geometry = settings_.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    statusBar()->showMessage(QStringLiteral("Откройте игру: Ctrl+O"), 0);

    connect(&session_, &EmulationSession::videoFrame, view_,
            [this](const QImage& frame, double aspect) {
                view_->setFrame(frame, aspect);
            });
    connect(&session_, &EmulationSession::audioSamples, audio_,
            [this](const QByteArray& pcm, int rate) {
                audio_->play(pcm, rate);
            });
    connect(&session_, &EmulationSession::audioReset,
            audio_, &AudioOutput::clear);
    connect(&session_, &EmulationSession::gameLoaded, this,
            [this](const QString& path, double fps) {
                currentRom_ = path;
                setWindowTitle(QStringLiteral("%1 — Kega Refusion").arg(displayName(path)));
                recentPaths_.removeAll(path);
                recentPaths_.prepend(path);
                while (recentPaths_.size() > 10)
                    recentPaths_.removeLast();
                settings_.setValue(QStringLiteral("recent/files"), recentPaths_);
                updateRecentMenu();
                statusBar()->showMessage(
                    QStringLiteral("%1 · %2 кадров/с").arg(QFileInfo(path).fileName())
                        .arg(fps, 0, 'f', 2), 5000);
                view_->setFocus();
            });
    connect(&session_, &EmulationSession::pausedChanged, this,
            [this](bool paused) {
                setPausedUi(paused);
                statusBar()->showMessage(
                    paused ? QStringLiteral("Пауза") : QStringLiteral("Продолжено"), 2500);
            });
    connect(&session_, &EmulationSession::notification, this,
            [this](const QString& message) {
                statusBar()->showMessage(message, 5000);
            });
    connect(&session_, &EmulationSession::errorOccurred, this,
            [this](const QString& message) {
                statusBar()->showMessage(message, 10000);
                if (!smokeTest_)
                    QMessageBox::warning(this, QStringLiteral("Kega Refusion"), message);
            });
    statusBar()->hide();
    qApp->installEventFilter(this);
}

void MainWindow::buildMenus() {
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&Файл"));
    QAction* openAction = fileMenu->addAction(QStringLiteral("Открыть игру…"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, [this] {
        releaseInput();
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Открыть игру"),
            settings_.value(QStringLiteral("files/lastDirectory"),
                            QDir::homePath()).toString(),
            QStringLiteral("Игры Mega Drive и Master System (*.md *.gen *.bin *.smd *.sms *.gg *.sg *.68k);;Все файлы (*)"));
        if (!path.isEmpty())
            openRom(path);
    });

    recentMenu_ = fileMenu->addMenu(QStringLiteral("Недавние игры"));
    updateRecentMenu();
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(QStringLiteral("Выход"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu* emulationMenu = menuBar()->addMenu(QStringLiteral("&Эмуляция"));
    pauseAction_ = emulationMenu->addAction(QStringLiteral("Пауза"));
    pauseAction_->setCheckable(true);
    pauseAction_->setShortcut(QKeySequence(QStringLiteral("P")));
    pauseAction_->setEnabled(false);
    connect(pauseAction_, &QAction::toggled, this,
            [this](bool paused) { session_.setPaused(paused); });

    QAction* resetAction = emulationMenu->addAction(QStringLiteral("Сброс"));
    resetAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    resetAction->setEnabled(false);
    connect(resetAction, &QAction::triggered, this, [this] {
        releaseInput();
        session_.resetGame();
    });

    fastForwardAction_ = emulationMenu->addAction(
        QStringLiteral("Ускорение (удерживать Tab)"));
    fastForwardAction_->setCheckable(true);
    fastForwardAction_->setEnabled(false);
    connect(fastForwardAction_, &QAction::toggled, this,
            [this] { updateFastForward(); });

    emulationMenu->addSeparator();
    QAction* saveAction = emulationMenu->addAction(QStringLiteral("Сохранить состояние"));
    saveAction->setShortcut(QKeySequence(QStringLiteral("F5")));
    saveAction->setEnabled(false);
    connect(saveAction, &QAction::triggered, this,
            [this] { session_.saveState(stateSlot_); });
    QAction* loadAction = emulationMenu->addAction(QStringLiteral("Загрузить состояние"));
    loadAction->setShortcut(QKeySequence(QStringLiteral("F8")));
    loadAction->setEnabled(false);
    connect(loadAction, &QAction::triggered, this,
            [this] { session_.loadState(stateSlot_); });

    QMenu* slotMenu = emulationMenu->addMenu(QStringLiteral("Слот сохранения"));
    QActionGroup* slotGroup = new QActionGroup(this);
    slotGroup->setExclusive(true);
    for (int slot = 0; slot < 10; ++slot) {
        QAction* action = slotMenu->addAction(QString::number(slot));
        action->setCheckable(true);
        action->setChecked(slot == stateSlot_);
        slotGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, slot] {
            stateSlot_ = slot;
            settings_.setValue(QStringLiteral("emulation/slot"), slot);
            statusBar()->showMessage(
                QStringLiteral("Слот сохранения: %1").arg(slot), 2500);
        });
    }

    connect(&session_, &EmulationSession::gameLoaded, this,
            [=](const QString&, double) {
                pauseAction_->setEnabled(true);
                resetAction->setEnabled(true);
                fastForwardAction_->setEnabled(true);
                saveAction->setEnabled(true);
                loadAction->setEnabled(true);
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
        });
    }
    QAction* integerAction = viewMenu->addAction(QStringLiteral("Целочисленный масштаб"));
    integerAction->setCheckable(true);
    integerAction->setChecked(
        settings_.value(QStringLiteral("view/integer"), false).toBool());
    connect(integerAction, &QAction::toggled, this, [this](bool enabled) {
        view_->setIntegerScaling(enabled);
        settings_.setValue(QStringLiteral("view/integer"), enabled);
    });
    viewMenu->addSeparator();
    QAction* fullscreenAction = viewMenu->addAction(QStringLiteral("Полный экран"));
    fullscreenAction->setShortcut(QKeySequence(QStringLiteral("Alt+Return")));
    fullscreenAction->setCheckable(true);
    connect(fullscreenAction, &QAction::toggled, this, [this](bool fullscreen) {
        if (fullscreen)
            showFullScreen();
        else
            showNormal();
    });

    QMenu* soundMenu = menuBar()->addMenu(QStringLiteral("&Звук"));
    QAction* muteAction = soundMenu->addAction(QStringLiteral("Без звука"));
    muteAction->setCheckable(true);
    muteAction->setShortcut(QKeySequence(QStringLiteral("M")));
    muteAction->setChecked(settings_.value(QStringLiteral("audio/muted"), false).toBool());
    audio_->setMuted(muteAction->isChecked());
    connect(muteAction, &QAction::toggled, this, [this](bool muted) {
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
    volumeSlider->setValue(
        qBound(0, settings_.value(QStringLiteral("audio/volume"), 80).toInt(), 100));
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
    QAction* controlsAction = settingsMenu->addAction(QStringLiteral("Клавиши управления…"));
    connect(controlsAction, &QAction::triggered, this, &MainWindow::editControls);

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Справка"));
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("О программе"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, QStringLiteral("О Kega Refusion"),
            QStringLiteral(
                "<b>Kega Refusion</b><br>"
                "Компактный эмулятор на базе Genesis Plus GX (Libretro).<br><br>"
                "Игры: MD, GEN, BIN, SMD, SMS, GG, SG, 68K. "
                "Поддержка CD, 32X и ZIP пока не подготовлена.<br>"
                "Состояния сохраняются в выбранный слот от 0 до 9.<br>"
                "Клавиши можно изменить в настройках."));
    });


}

void MainWindow::updateRecentMenu() {
    recentMenu_->clear();
    QStringList existing;
    for (const QString& path : recentPaths_) {
        if (!QFileInfo::exists(path))
            continue;
        existing.append(path);
        QAction* action = recentMenu_->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
        connect(action, &QAction::triggered, this,
                [this, path] { openRom(path); });
    }
    recentPaths_ = existing;
    recentMenu_->setEnabled(!existing.isEmpty());
}

void MainWindow::openRom(const QString& path) {
    const QFileInfo file(path);
    if (!file.exists() || !file.isFile()) {
        const QString message = QStringLiteral("Файл не найден: %1").arg(path);
        statusBar()->showMessage(message, 10000);
        if (!smokeTest_)
            QMessageBox::warning(this, QStringLiteral("Kega Refusion"), message);
        return;
    }
    releaseInput();
    settings_.setValue(QStringLiteral("files/lastDirectory"), file.absolutePath());
    statusBar()->showMessage(
        QStringLiteral("Загрузка %1…").arg(file.fileName()), 0);
    session_.loadGame(file.absoluteFilePath());
}

void MainWindow::editControls() {
    releaseInput();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Клавиши управления"));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QLabel* hint = new QLabel(
        QStringLiteral("Кнопки Genesis: Z/X/C — A/B/C, A/S/D — X/Y/Z. "
                       "Tab удерживает ускорение."),
        &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    QFormLayout* form = new QFormLayout;
    QHash<unsigned, QKeySequenceEdit*> editors;
    for (const auto& binding : bindings) {
        int key = settings_.value(keySetting(binding.id), binding.defaultKey).toInt();
        QKeySequenceEdit* editor = new QKeySequenceEdit(&dialog);
        if (key)
            editor->setKeySequence(QKeySequence(key));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        editor->setMaximumSequenceLength(1);
#endif
        form->addRow(QString::fromUtf8(binding.label), editor);
        editors.insert(binding.id, editor);
    }
    layout->addLayout(form);
    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("Сохранить"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QHash<int, unsigned> proposed;
        QHash<unsigned, int> selected;
        for (const auto& binding : bindings) {
            const QKeySequence sequence = editors.value(binding.id)->keySequence();
            if (sequence.count() > 1) {
                QMessageBox::warning(
                    &dialog, QStringLiteral("Клавиши управления"),
                    QStringLiteral("Для кнопки нужна только одна клавиша."));
                return;
            }
            int key = sequence.isEmpty() ? 0 : sequence[0].key();
            const Qt::KeyboardModifiers modifiers =
                sequence.isEmpty() ? Qt::NoModifier : sequence[0].keyboardModifiers();
            if (modifiers != Qt::NoModifier || key == Qt::Key_Tab ||
                key == Qt::Key_Escape || key == Qt::Key_P ||
                key == Qt::Key_M || key == Qt::Key_F5 ||
                key == Qt::Key_F8) {
                QMessageBox::warning(
                    &dialog, QStringLiteral("Клавиши управления"),
                    QStringLiteral("Используйте отдельную клавишу без Ctrl, Alt или Shift. "
                                   "Tab, Esc, P, M, F5 и F8 зарезервированы."));
                return;
            }
            if (key && proposed.contains(key)) {
                QMessageBox::warning(
                    &dialog, QStringLiteral("Клавиши управления"),
                    QStringLiteral("Одна клавиша назначена двум кнопкам."));
                return;
            }
            selected.insert(binding.id, key);
            if (key)
                proposed.insert(key, binding.id);
        }
        releaseInput();
        keyToButton_ = proposed;
        for (auto it = selected.cbegin(); it != selected.cend(); ++it)
            settings_.setValue(keySetting(it.key()), it.value());
        dialog.accept();
    });
    dialog.exec();
    view_->setFocus();
}

void MainWindow::releaseInput() {
    heldKeys_.clear();
    fastForwardHeld_ = false;
    if (fastForwardAction_ && fastForwardAction_->isChecked())
        fastForwardAction_->setChecked(false);
    session_.releaseButtons();
    updateFastForward();
}

void MainWindow::updateFastForward() {
    const bool enabled = fastForwardHeld_ ||
                         (fastForwardAction_ && fastForwardAction_->isChecked());
    if (enabled)
        audio_->clear();
    session_.setFastForward(enabled);
}

void MainWindow::setPausedUi(bool paused) {
    if (pauseAction_) {
        const QSignalBlocker blocker(pauseAction_);
        pauseAction_->setChecked(paused);
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
    if (QApplication::activeModalWidget() || !isActiveWindow() || !session_.hasGame())
        return QMainWindow::eventFilter(watched, event);

    auto* keyEvent = static_cast<QKeyEvent*>(event);
    const bool down = event->type() == QEvent::KeyPress;
    if (keyEvent->isAutoRepeat())
        return QMainWindow::eventFilter(watched, event);

    if (keyEvent->key() == Qt::Key_Tab &&
        keyEvent->modifiers() == Qt::NoModifier) {
        fastForwardHeld_ = down;
        updateFastForward();
        return true;
    }
    const int key = keyEvent->key();
    if (keyEvent->modifiers() != Qt::NoModifier || !keyToButton_.contains(key))
        return QMainWindow::eventFilter(watched, event);
    if (down) {
        if (!heldKeys_.contains(key)) {
            heldKeys_.insert(key);
            session_.setButton(0, keyToButton_.value(key), true);
        }
    } else {
        heldKeys_.remove(key);
        session_.setButton(0, keyToButton_.value(key), false);
    }
    return true;
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
    if (event->type() == QEvent::ActivationChange && !isActiveWindow())
        releaseInput();
    QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    releaseInput();
    settings_.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings_.sync();
    QMainWindow::closeEvent(event);
}
