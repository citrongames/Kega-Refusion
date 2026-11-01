#include "MainWindow.h"
#include "AudioOutput.h"
#include "ControlsDialog.h"
#include "../core/EmulationSession.h"
#include "../core/RomArchive.h"
#include "../core/SegaCdBios.h"
#include "../core/Sega32xBios.h"
#include "../input/InputManager.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QAudioDevice>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QImageWriter>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QFormLayout>
#include <QCryptographicHash>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QMediaDevices>
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
#include <optional>

namespace {
QString shaderParameterPrefix(const QString& path) {
    const auto key = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("view/shaderParameters/%1/").arg(QString::fromLatin1(key));
}
QString displayName(const QString& path) { return QFileInfo(path).completeBaseName(); }
bool isCdPath(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QLatin1String("cue") || ext == QLatin1String("iso") ||
           ext == QLatin1String("chd") || ext == QLatin1String("m3u");
}
bool isSupportedDropFile(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    return QFileInfo(path).isFile() &&
           (isCdPath(path) || QStringList{QStringLiteral("md"), QStringLiteral("gen"),
               QStringLiteral("bin"), QStringLiteral("smd"), QStringLiteral("sms"),
               QStringLiteral("gg"), QStringLiteral("sg"), QStringLiteral("68k"),
               QStringLiteral("32x"), QStringLiteral("zip"), QStringLiteral("7z")}.contains(ext));
}
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
      dataPath_(dataPath), smokeTest_(smokeTest) {
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
    connect(view_, &GameView::openGameRequested, this, [this] {
        if (!loading_ && !session_.hasGame()) openGameDialog();
    });
    audio_ = new AudioOutput(this);
    audio_->setOutputDeviceId(settings_.value(QStringLiteral("audio/outputDeviceId")).toByteArray());
    input_ = new InputManager(settings_.fileName(), this);
    connect(audio_, &AudioOutput::unavailable, this, [](const QString& text) {
        qWarning("%s", qPrintable(text));
    });
    connect(input_, &InputManager::maskChanged, this, [this](unsigned port, unsigned mask) {
        session_.setButtons(port, mask);
    });
    connect(input_, &InputManager::connectionChanged, this, [this](unsigned port, bool connected) {
        session_.setControllerConnected(port, connected);
    });
    connect(input_, &InputManager::controllerTypeChanged, this, [this](unsigned port, bool sixButton) {
        session_.setControllerSixButton(port, sixButton);
    });
    for (unsigned port = 0; port < 2; ++port) {
        session_.setControllerConnected(port, input_->isConnected(port));
        session_.setControllerSixButton(port, input_->sixButton(port));
    }
    connect(input_, &InputManager::commandChanged, this, &MainWindow::handleInputCommand);
    connect(input_, &InputManager::notification, this, [](const QString& text) {
        qInfo("%s", qPrintable(text));
    });

    recentPaths_ = settings_.value(QStringLiteral("recent/files")).toStringList();
    stateSlot_ = qBound(0, settings_.value(QStringLiteral("emulation/slot"), 0).toInt(), 9);
    fastForwardFactor_ = settings_.value(QStringLiteral("emulation/fastForwardFactor"), 4).toInt();
    if (!QList<int>{0,2,3,4,6,8,12,16,32}.contains(fastForwardFactor_)) fastForwardFactor_ = 4;
    rewindSeconds_ = settings_.value(QStringLiteral("emulation/rewindSeconds"), 30).toInt();
    if (!QList<int>{0,10,30,60}.contains(rewindSeconds_)) rewindSeconds_ = 30;
    pauseOnFocusLoss_ = settings_.value(QStringLiteral("emulation/pauseOnFocusLoss"), true).toBool();
    region_ = qBound(0, settings_.value(QStringLiteral("emulation/region"), 0).toInt(), 3);
    blarggFilter_ = qBound(0, settings_.value(QStringLiteral("view/blarggFilter"), 0).toInt(), 4);
    session_.setRegion(region_);
    session_.setBlarggFilter(blarggFilter_);
    session_.setAutoFixChecksum(settings_.value(QStringLiteral("emulation/autoFixChecksum"), false).toBool());
    session_.setFastCdLoading(settings_.value(QStringLiteral("emulation/fastCdLoading"), false).toBool());
    session_.setRemoveSpriteLimit(settings_.value(QStringLiteral("emulation/removeSpriteLimit"), false).toBool());
    session_.setFastForwardFactor(fastForwardFactor_);
    session_.setRewindSeconds(rewindSeconds_);
    session_.setRewindMemoryMiB(settings_.value(QStringLiteral("emulation/rewindMemoryMiB"), 128).toInt());
    view_->setFastForwardFactor(fastForwardFactor_);
    buildMenus();
    connect(&session_, &EmulationSession::discStateChanged, this,
            [this](const QStringList& paths, int index, bool ejected) {
        discPaths_ = paths;
        discIndex_ = index;
        discEjected_ = ejected;
        updateDiscMenu();
    });
    connect(view_, &GameView::shaderFailed, this, [this](const QString& error) {
        settings_.setValue(QStringLiteral("view/shaderPreset"), QString());
        updateShaderActions();
        if (!smokeTest_) QMessageBox::warning(this, QStringLiteral("Шейдер"), error);
    });
    const QString shader = settings_.value(QStringLiteral("view/shaderPreset")).toString();
    if (!shader.isEmpty() && !applyShaderPreset(shader)) {
        settings_.setValue(QStringLiteral("view/shaderPreset"), QString());
        updateShaderActions();
    }
    const int aspect = settings_.value(QStringLiteral("view/aspect"), 0).toInt();
    view_->setAspectMode(aspect == 1 ? GameView::AspectMode::SquarePixels :
                         aspect == 2 ? GameView::AspectMode::Stretch : GameView::AspectMode::Console);
    view_->setIntegerScaling(settings_.value(QStringLiteral("view/integer"), false).toBool());
    const QByteArray geometry = settings_.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);

    connect(&session_, &EmulationSession::videoFrameReady, view_, [this] {
        QImage image; double ratio = 4.0 / 3.0;
        if (session_.takeVideoFrame(image, ratio)) view_->setFrame(image, ratio);
    });
    connect(&session_, &EmulationSession::audioSamples, audio_, [this](const QByteArray& pcm, int rate) {
        audio_->play(pcm, rate);
    }, Qt::DirectConnection);
    connect(&session_, &EmulationSession::audioReset, audio_, &AudioOutput::clear, Qt::DirectConnection);
    connect(audio_, &AudioOutput::clockFeedback, &session_, &EmulationSession::updateAudioClock, Qt::DirectConnection);
    connect(&session_, &EmulationSession::gameLoaded, this, [this](const QString& path, double, const QString& contentName) {
        setLoadingUi(false);
        currentRom_ = path;
        activeInputProfile_ = isCdPath(path) ? ConsoleProfile::MegaDrive : profileFor(QFileInfo(contentName));
        input_->setProfile(activeInputProfile_);
        currentGameName_ = displayName(contentName);
        setWindowTitle(QStringLiteral("%1 — Kega Refusion").arg(currentGameName_));
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
    connect(&session_, &EmulationSession::rewindProgress, view_, &GameView::setRewindProgress);
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
        if (loading_) input_->setProfile(activeInputProfile_);
        setLoadingUi(false);
        updateInputEnabled();
        if (!smokeTest_) QMessageBox::warning(this, QStringLiteral("Kega Refusion"), text);
    });

    inputTimer_ = new QTimer(this);
    inputTimer_->setInterval(8);
    connect(inputTimer_, &QTimer::timeout, this, &MainWindow::updateInputEnabled);
    inputTimer_->start();
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow() {
    // The direct audio producer must stop before QObject destroys AudioOutput.
    session_.shutdown();
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
    connect(openAction, &QAction::triggered, this, &MainWindow::openGameDialog);
    recentMenu_ = fileMenu->addMenu(QStringLiteral("Недавние игры"));
    updateRecentMenu();
    QMenu* segaCdMenu = fileMenu->addMenu(QStringLiteral("Sega CD"));
    QAction* biosAction = segaCdMenu->addAction(QStringLiteral("BIOS…"));
    connect(biosAction, &QAction::triggered, this, &MainWindow::editSegaCdBios);
    segaCdMenu->addSeparator();
    changeDiscAction_ = segaCdMenu->addAction(QStringLiteral("Сменить диск…"));
    connect(changeDiscAction_, &QAction::triggered, this, [this] {
        releaseInput();
        const QString initial = discIndex_ >= 0 && discIndex_ < discPaths_.size()
            ? QFileInfo(discPaths_[discIndex_]).absolutePath()
            : settings_.value(QStringLiteral("files/lastDirectory"), QDir::homePath()).toString();
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Сменить диск"), initial,
            QStringLiteral("Диски Sega CD (*.cue *.iso *.chd);;Все файлы (*)"));
        if (!path.isEmpty()) session_.changeDisc(path);
        view_->setFocus();
    });
    ejectDiscAction_ = segaCdMenu->addAction(QStringLiteral("Извлечь диск"));
    ejectDiscAction_->setCheckable(true);
    connect(ejectDiscAction_, &QAction::triggered, this, [this](bool ejected) {
        releaseInput();
        session_.setDiscEjected(ejected);
        ejectDiscAction_->setChecked(discEjected_);
        view_->setFocus();
    });
    discMenu_ = segaCdMenu->addMenu(QStringLiteral("Диски плейлиста"));
    discGroup_ = new QActionGroup(this);
    discGroup_->setExclusive(true);
    updateDiscMenu();
    QMenu* sega32xMenu = fileMenu->addMenu(QStringLiteral("32X"));
    QAction* bios32xAction = sega32xMenu->addAction(QStringLiteral("BIOS…"));
    bios32xAction->setObjectName(QStringLiteral("sega32xBiosAction"));
    connect(bios32xAction, &QAction::triggered, this, &MainWindow::editSega32xBios);
    fileMenu->addSeparator();
    screenshotAction_ = fileMenu->addAction(QStringLiteral("Сохранить скриншот"));
    screenshotAction_->setObjectName(QStringLiteral("saveScreenshot"));
    screenshotAction_->setShortcut(QKeySequence(QStringLiteral("F12")));
    screenshotAction_->setAutoRepeat(false);
    screenshotAction_->setEnabled(false);
    screenshotAction_->setToolTip(QStringLiteral("Чистое изображение игры без OSD; папка screenshots рядом с settings.ini."));
    shortcut(screenshotAction_);
    connect(screenshotAction_, &QAction::triggered, this, &MainWindow::saveScreenshot);
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
    resetAction_ = emulationMenu->addAction(QStringLiteral("Мягкий сброс"));
    resetAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    resetAction_->setAutoRepeat(false);
    resetAction_->setEnabled(false);
    shortcut(resetAction_);
    connect(resetAction_, &QAction::triggered, this, [this] {
        releaseInput();
        session_.resetGame();
    });
    hardResetAction_ = emulationMenu->addAction(QStringLiteral("Жёсткий сброс"));
    hardResetAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    hardResetAction_->setAutoRepeat(false);
    hardResetAction_->setEnabled(false);
    shortcut(hardResetAction_);
    connect(hardResetAction_, &QAction::triggered, this, [this] {
        releaseInput(); session_.hardResetGame();
    });
    QMenu* regionMenu = emulationMenu->addMenu(QStringLiteral("Регион"));
    regionMenu->setObjectName(QStringLiteral("regionMenu"));
    auto* regionGroup = new QActionGroup(this);
    const QStringList regions{QStringLiteral("Авто"), QStringLiteral("Япония (NTSC-J)"),
        QStringLiteral("США (NTSC-U)"), QStringLiteral("Европа (PAL)")};
    for (int region = 0; region < regions.size(); ++region) {
        QAction* action = regionMenu->addAction(regions[region]);
        action->setObjectName(QStringLiteral("region%1").arg(region));
        action->setCheckable(true); action->setChecked(region == region_);
        regionGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, region] {
            releaseInput(); region_ = region;
            settings_.setValue(QStringLiteral("emulation/region"), region);
            session_.setRegion(region);
        });
    }
    QAction* checksumAction = emulationMenu->addAction(QStringLiteral("Автоисправление контрольной суммы ROM"));
    checksumAction->setObjectName(QStringLiteral("autoFixChecksum"));
    checksumAction->setCheckable(true);
    checksumAction->setChecked(settings_.value(QStringLiteral("emulation/autoFixChecksum"), false).toBool());
    checksumAction->setToolTip(QStringLiteral("Для Mega Drive/Genesis. Исправляется копия в памяти; применится при следующей загрузке игры или хард-ресете."));
    connect(checksumAction, &QAction::toggled, this, [this](bool enabled) {
        settings_.setValue(QStringLiteral("emulation/autoFixChecksum"), enabled);
        session_.setAutoFixChecksum(enabled);
    });
    fastForwardAction_ = emulationMenu->addAction(QStringLiteral("Ускорение (Tab)"));
    fastForwardAction_->setCheckable(true);
    fastForwardAction_->setEnabled(false);
    connect(fastForwardAction_, &QAction::toggled, this, &MainWindow::updateFastForward);
    QMenu* speedMenu = emulationMenu->addMenu(QStringLiteral("Скорость ускорения"));
    auto* speedGroup = new QActionGroup(this);
    for (int factor : {2,3,4,6,8,12,16,32,0}) {
        QAction* action = speedMenu->addAction(factor == 0 ? QStringLiteral("Без ограничения") : QStringLiteral("×%1").arg(factor));
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
        hardResetAction_->setEnabled(true);
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
    QMenu* blarggMenu = viewMenu->addMenu(QStringLiteral("Фильтр Blargg NTSC"));
    blarggMenu->setObjectName(QStringLiteral("blarggMenu"));
    auto* blarggGroup = new QActionGroup(this);
    const QStringList filters{QStringLiteral("Выключен"), QStringLiteral("Монохромный"),
        QStringLiteral("Композитный"), QStringLiteral("S-Video"), QStringLiteral("RGB")};
    for (int filter = 0; filter < filters.size(); ++filter) {
        QAction* action = blarggMenu->addAction(filters[filter]);
        action->setObjectName(QStringLiteral("blargg%1").arg(filter));
        action->setCheckable(true); action->setChecked(filter == blarggFilter_);
        blarggGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, filter] {
            blarggFilter_ = filter;
            settings_.setValue(QStringLiteral("view/blarggFilter"), filter);
            session_.setBlarggFilter(filter);
        });
    }
    QMenu* shaderMenu = viewMenu->addMenu(QStringLiteral("Шейдер"));
    shaderMenu->setObjectName(QStringLiteral("shaderMenu"));
    auto* shaderGroup = new QActionGroup(this);
    shaderOffAction_ = shaderMenu->addAction(QStringLiteral("Выключен"));
    shaderGdaptAction_ = shaderMenu->addAction(QStringLiteral("GDAPT"));
    shaderCustomAction_ = shaderMenu->addAction(QStringLiteral("Пользовательский"));
    for (QAction* action : {shaderOffAction_, shaderGdaptAction_, shaderCustomAction_}) {
        action->setCheckable(true); shaderGroup->addAction(action);
    }
    shaderCustomAction_->setEnabled(false);
    connect(shaderOffAction_, &QAction::triggered, this, [this] { applyShaderPreset({}); });
    connect(shaderGdaptAction_, &QAction::triggered, this, [this] { applyShaderPreset(QStringLiteral(":/shaders/gdapt.glslp")); });
    shaderMenu->addSeparator();
    QAction* shaderLoad = shaderMenu->addAction(QStringLiteral("Загрузить GLSL-пресет…"));
    connect(shaderLoad, &QAction::triggered, this, [this] {
        releaseInput();
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Загрузить GLSL-пресет"),
            settings_.value(QStringLiteral("files/shaderDirectory")).toString(),
            QStringLiteral("GLSL-пресеты (*.glslp);;GLSL-шейдеры (*.glsl)"));
        if (!path.isEmpty() && applyShaderPreset(path))
            settings_.setValue(QStringLiteral("files/shaderDirectory"), QFileInfo(path).absolutePath());
        view_->setFocus();
    });
    shaderParametersAction_ = shaderMenu->addAction(QStringLiteral("Параметры…"));
    connect(shaderParametersAction_, &QAction::triggered, this, &MainWindow::editShaderParameters);
    updateShaderActions();
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
    view_->setMutedStatus(muteAction_->isChecked());
    connect(muteAction_, &QAction::toggled, this, [this](bool muted) {
        audio_->setMuted(muted);
        view_->setMutedStatus(muted);
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
    QMenu* outputMenu = soundMenu->addMenu(QStringLiteral("Устройство вывода"));
    outputMenu->setObjectName(QStringLiteral("audioOutputMenu"));
    auto* outputGroup = new QActionGroup(outputMenu);
    outputGroup->setExclusive(true);
    const auto refreshOutputs = [this, outputMenu, outputGroup] {
        outputMenu->clear();
        const QByteArray selectedId = audio_->outputDeviceId();
        const QAudioDevice defaultDevice = QMediaDevices::defaultAudioOutput();
        const QString defaultName = defaultDevice.isNull()
            ? QStringLiteral("нет устройства") : defaultDevice.description();
        QAction* defaultAction = outputMenu->addAction(
            QStringLiteral("Системное по умолчанию (%1)").arg(defaultName).replace('&', QStringLiteral("&&")));
        defaultAction->setObjectName(QStringLiteral("audioOutputDefaultAction"));
        defaultAction->setCheckable(true);
        defaultAction->setChecked(selectedId.isEmpty());
        defaultAction->setData(QByteArray());
        outputGroup->addAction(defaultAction);
        const auto outputs = QMediaDevices::audioOutputs();
        bool selectedPresent = selectedId.isEmpty();
        for (const QAudioDevice& device : outputs)
            selectedPresent = selectedPresent || device.id() == selectedId;
        if (!selectedPresent) {
            QAction* missing = outputMenu->addAction(QStringLiteral("Недоступно — используется системное"));
            missing->setCheckable(true);
            missing->setChecked(true);
            missing->setEnabled(false);
            outputGroup->addAction(missing);
        }
        if (!outputs.isEmpty()) outputMenu->addSeparator();
        for (const QAudioDevice& device : outputs) {
            QAction* action = outputMenu->addAction(device.description().replace('&', QStringLiteral("&&")));
            action->setCheckable(true);
            action->setChecked(device.id() == selectedId);
            action->setData(device.id());
            outputGroup->addAction(action);
        }
    };
    connect(outputGroup, &QActionGroup::triggered, this, [this](QAction* action) {
        const QByteArray id = action->data().toByteArray();
        audio_->setOutputDeviceId(id);
        settings_.setValue(QStringLiteral("audio/outputDeviceId"), id);
    });
    connect(outputMenu, &QMenu::aboutToShow, outputMenu, refreshOutputs);
    connect(audio_, &AudioOutput::outputDevicesChanged, outputMenu, refreshOutputs);
    refreshOutputs();

    QMenu* settingsMenu = menuBar()->addMenu(QStringLiteral("&Настройки"));
    QAction* emulatorSettingsAction = settingsMenu->addAction(QStringLiteral("Настройки эмулятора…"));
    emulatorSettingsAction->setObjectName(QStringLiteral("emulatorSettingsAction"));
    connect(emulatorSettingsAction, &QAction::triggered, this, &MainWindow::editSettings);
    QAction* controlsAction = settingsMenu->addAction(QStringLiteral("Управление…"));
    connect(controlsAction, &QAction::triggered, this, &MainWindow::editControls);
    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Справка"));
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("О программе"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this, QStringLiteral("О Kega Refusion"),
            QStringLiteral("<b>Kega Refusion</b><br>Компактный эмулятор на базе Genesis Plus GX (Libretro)."
            "<br><br>Игры: MD, GEN, BIN, SMD, SMS, GG, SG, 68K и Sega CD (CUE, ISO, CHD, M3U). "
            "Поддерживаются архивы ZIP и 7z. Поддержка 32X — экспериментальная.<br>"
            "Состояния сохраняются в выбранный слот от 0 до 9.<br>"
            "Клавиатуру и геймпады можно настроить в разделе «Управление»."));
    });
}

bool MainWindow::applyShaderPreset(const QString& path) {
    QString error;
    if (!view_->setShaderPreset(path, &error)) {
        updateShaderActions();
        if (!smokeTest_) QMessageBox::warning(this, QStringLiteral("Шейдер"), error);
        return false;
    }
    settings_.setValue(QStringLiteral("view/shaderPreset"), path);
    const QString prefix = shaderParameterPrefix(path);
    for (const auto& parameter : view_->shaderParameters()) {
        const float value = settings_.value(prefix + parameter.id, parameter.value).toFloat();
        view_->setShaderParameter(parameter.id, value);
    }
    updateShaderActions();
    return true;
}

void MainWindow::updateShaderActions() {
    if (!shaderOffAction_) return;
    const QString path = view_->shaderPreset();
    shaderOffAction_->setChecked(path.isEmpty());
    shaderGdaptAction_->setChecked(path == QLatin1String(":/shaders/gdapt.glslp"));
    shaderCustomAction_->setChecked(!path.isEmpty() && path != QLatin1String(":/shaders/gdapt.glslp"));
    shaderParametersAction_->setEnabled(!view_->shaderParameters().isEmpty());
}

void MainWindow::editShaderParameters() {
    const auto parameters = view_->shaderParameters();
    if (parameters.isEmpty()) return;
    releaseInput();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Параметры шейдера"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    const QString prefix = shaderParameterPrefix(view_->shaderPreset());
    for (const auto& parameter : parameters) {
        auto* value = new QDoubleSpinBox(&dialog);
        value->setDecimals(6);
        value->setRange(parameter.minimum, parameter.maximum);
        value->setSingleStep(parameter.step);
        value->setValue(parameter.value);
        value->setObjectName(parameter.id);
        form->addRow(parameter.label, value);
        connect(value, &QDoubleSpinBox::valueChanged, &dialog, [this, prefix, id = parameter.id](double selected) {
            if (view_->setShaderParameter(id, float(selected))) settings_.setValue(prefix + id, selected);
        });
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Закрыть"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec(); view_->setFocus();
}

void MainWindow::editSegaCdBios() {
    releaseInput();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("BIOS Sega CD"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    QLabel* statusLabels[3]{};
    const QStringList regions{QStringLiteral("Япония"), QStringLiteral("США"),
                              QStringLiteral("Европа")};
    for (int region = 1; region <= 3; ++region) {
        statusLabels[region - 1] = new QLabel(&dialog);
        form->addRow(regions[region - 1] + QStringLiteral(":"), statusLabels[region - 1]);
    }
    auto refresh = [this, &statusLabels] {
        for (int region = 1; region <= 3; ++region) {
            QString error;
            const bool installed = SegaCdBios::validate(dataPath_, region, error);
            statusLabels[region - 1]->setText(installed ? QStringLiteral("Установлен")
                                                      : QStringLiteral("Не готов"));
            statusLabels[region - 1]->setToolTip(error);
        }
    };
    refresh();
    auto* importButton = new QPushButton(QStringLiteral("Импортировать из папки…"), &dialog);
    layout->addWidget(importButton);
    connect(importButton, &QPushButton::clicked, &dialog, [this, &dialog, &refresh] {
        const QString source = QFileDialog::getExistingDirectory(
            &dialog, QStringLiteral("Папка с BIOS Sega CD"), QDir::homePath());
        if (source.isEmpty()) return;
        QString error;
        const QStringList imported = SegaCdBios::importDirectory(source, dataPath_, error);
        refresh();
        QString message;
        if (!imported.isEmpty())
            message = QStringLiteral("Импортированы: %1").arg(imported.join(QStringLiteral(", ")));
        if (!error.isEmpty()) {
            if (!message.isEmpty()) message += QStringLiteral("\n\n");
            QMessageBox::warning(&dialog, QStringLiteral("BIOS Sega CD"), message + error);
        } else {
            QMessageBox::information(&dialog, QStringLiteral("BIOS Sega CD"),
                message.isEmpty() ? QStringLiteral("Новые BIOS не найдены.") : message);
        }
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Закрыть"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
    view_->setFocus();
}

void MainWindow::editSega32xBios() {
    releaseInput();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("sega32xBiosDialog"));
    dialog.setWindowTitle(QStringLiteral("BIOS 32X"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* hint = new QLabel(QStringLiteral("Для 32X нужны три файла BIOS. "
        "Для CD32X также требуется BIOS Sega CD соответствующего региона."), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    const Sega32xBios::Piece pieces[]{Sega32xBios::Piece::M68k,
        Sega32xBios::Piece::MasterSh2, Sega32xBios::Piece::SlaveSh2};
    const QStringList labels{QStringLiteral("68000"), QStringLiteral("Master SH2"),
                             QStringLiteral("Slave SH2")};
    QLabel* statusLabels[3]{};
    for (int i = 0; i < 3; ++i) {
        statusLabels[i] = new QLabel(&dialog);
        statusLabels[i]->setObjectName(QStringLiteral("bios32xStatus%1").arg(i));
        form->addRow(labels[i] + QLatin1Char(':'), statusLabels[i]);
    }
    auto refresh = [this, &pieces, &statusLabels] {
        const QDir bios(QDir(dataPath_).filePath(QStringLiteral("bios")));
        for (int i = 0; i < 3; ++i) {
            const QString path = bios.filePath(Sega32xBios::fileName(pieces[i]));
            QString error;
            const bool installed = Sega32xBios::validateFile(path, pieces[i], error);
            statusLabels[i]->setText(installed ? QStringLiteral("Установлен") : QStringLiteral("Не готов"));
            statusLabels[i]->setToolTip(installed ? QDir::toNativeSeparators(path) : error);
        }
    };
    refresh();
    auto* importButton = new QPushButton(QStringLiteral("Импортировать из папки…"), &dialog);
    importButton->setObjectName(QStringLiteral("import32xBios"));
    layout->addWidget(importButton);
    connect(importButton, &QPushButton::clicked, &dialog, [this, &dialog, &refresh] {
        const QString source = QFileDialog::getExistingDirectory(
            &dialog, QStringLiteral("Папка с BIOS 32X"), QDir::homePath());
        if (source.isEmpty()) return;
        QString error;
        const auto imported = Sega32xBios::importDirectory(source, dataPath_, error);
        refresh();
        QString message;
        if (!imported.isEmpty()) message = QStringLiteral("Импортированы: %1").arg(imported.join(QStringLiteral(", ")));
        if (!error.isEmpty()) {
            if (!message.isEmpty()) message += QStringLiteral("\n\n");
            QMessageBox::warning(&dialog, QStringLiteral("BIOS 32X"), message + error);
        } else {
            QMessageBox::information(&dialog, QStringLiteral("BIOS 32X"),
                message.isEmpty() ? QStringLiteral("Новые BIOS не найдены или уже установлены.") : message);
        }
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Закрыть"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
    view_->setFocus();
    updateInputEnabled();
}

void MainWindow::updateDiscMenu() {
    if (!discMenu_) return;
    const bool hasDisc = !discPaths_.isEmpty();
    changeDiscAction_->setEnabled(hasDisc);
    ejectDiscAction_->setEnabled(hasDisc);
    ejectDiscAction_->setChecked(hasDisc && discEjected_);
    discMenu_->clear();
    discMenu_->setEnabled(discPaths_.size() > 1);
    for (int index = 0; index < discPaths_.size(); ++index) {
        QAction* action = discMenu_->addAction(
            QStringLiteral("%1. %2").arg(index + 1).arg(QFileInfo(discPaths_[index]).fileName()));
        action->setToolTip(discPaths_[index]);
        action->setCheckable(true);
        action->setChecked(index == discIndex_);
        discGroup_->addAction(action);
        connect(action, &QAction::triggered, this, [this, index] {
            releaseInput();
            session_.selectDisc(index);
            if (discIndex_ >= 0 && discIndex_ < discGroup_->actions().size())
                discGroup_->actions()[discIndex_]->setChecked(true);
            view_->setFocus();
        });
    }
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

void MainWindow::saveScreenshot() {
    if (!session_.hasGame() || loading_) return;
    const QImage image = view_->captureGameImage();
    auto failed = [this](const QString& error) {
        view_->showOsd(QStringLiteral("Не удалось сохранить скриншот: %1").arg(error), 4000,
                       QStringLiteral("screenshot"));
        qWarning("Screenshot failed: %s", qPrintable(error));
    };
    if (image.isNull()) {
        failed(QStringLiteral("кадр игры ещё не готов"));
        return;
    }
    const QDir directory(QDir(dataPath_).filePath(QStringLiteral("screenshots")));
    if (!QDir().mkpath(directory.absolutePath())) {
        failed(QStringLiteral("не удалось создать папку %1").arg(QDir::toNativeSeparators(directory.absolutePath())));
        return;
    }
    QString name = currentGameName_.left(80);
    for (qsizetype i = 0; i < name.size(); ++i) {
        const QChar ch = name.at(i);
        if (ch.unicode() < 32 || QStringLiteral("\\/:*?\"<>|").contains(ch))
            name[i] = QLatin1Char('_');
    }
    if (name.trimmed().isEmpty()) name = QStringLiteral("game");
    const QString stem = name + QLatin1Char('_') +
                         QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz"));
    const bool jpeg = settings_.value(QStringLiteral("screenshot/format"), QStringLiteral("png"))
                          .toString() == QLatin1String("jpeg");
    const QString extension = jpeg ? QStringLiteral("jpg") : QStringLiteral("png");
    for (int suffix = 0; suffix < 1000; ++suffix) {
        const QString filename = stem + (suffix ? QStringLiteral("-%1").arg(suffix) : QString()) +
                                 QLatin1Char('.') + extension;
        QFile file(directory.filePath(filename));
        // NewOnly also protects existing captures if the clock moves backwards
        // or another emulator process saves into the same directory.
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            if (file.exists()) continue;
            failed(file.errorString());
            return;
        }
        QImageWriter writer(&file, jpeg ? "JPEG" : "PNG");
        if (jpeg) writer.setQuality(95);
        QString error;
        if (!writer.write(image)) error = writer.errorString();
        else if (!file.flush()) error = file.errorString();
        file.close();
        if (!error.isEmpty()) {
            file.remove();
            failed(error);
            return;
        }
        view_->showOsd(QStringLiteral("Скриншот сохранён: %1").arg(filename), 2600,
                       QStringLiteral("screenshot"));
        qInfo("Screenshot saved: %s", qPrintable(QDir::toNativeSeparators(file.fileName())));
        return;
    }
    failed(QStringLiteral("не удалось подобрать свободное имя файла"));
}

void MainWindow::setLoadingUi(bool loading, const QString& fileName) {
    loading_ = loading;
    view_->setLoading(loading, fileName);
    if (screenshotAction_) screenshotAction_->setEnabled(session_.hasGame() && !loading);
}

void MainWindow::openGameDialog() {
    if (loading_) return;
    releaseInput();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Открыть игру"),
        settings_.value(QStringLiteral("files/lastDirectory"), QDir::homePath()).toString(),
        QStringLiteral("Игры Sega (*.md *.gen *.bin *.smd *.sms *.gg *.sg *.68k *.32x *.cue *.iso *.chd *.m3u *.zip *.7z);;Диски Sega CD (*.cue *.iso *.chd *.m3u);;Архивы (*.zip *.7z);;Все файлы (*)"));
    if (!path.isEmpty()) openRom(path);
    else view_->setFocus();
}

void MainWindow::openRom(const QString& path) {
    if (loading_) return;
    const QFileInfo file(path);
    if (!file.exists() || !file.isFile()) {
        const QString text = QStringLiteral("Файл не найден: %1").arg(path);
        if (!smokeTest_) QMessageBox::warning(this, QStringLiteral("Kega Refusion"), text);
        return;
    }
    releaseInput();
    setLoadingUi(true, file.fileName());
    std::optional<RomArchive::Entry> archiveEntry;
    if (RomArchive::isArchive(path)) {
        QString error;
        const auto entries = RomArchive::list(path, &error);
        if (!error.isEmpty() || entries.isEmpty()) {
            if (!smokeTest_) setLoadingUi(false);
            if (!smokeTest_) QMessageBox::warning(this, QStringLiteral("Kega Refusion"),
                error.isEmpty() ? QStringLiteral("В архиве нет поддерживаемых игр.") : error);
            // Let the session report the same failure to the smoke-test observer.
            if (smokeTest_) session_.loadGame(path);
            else { view_->setFocus(); updateInputEnabled(); }
            return;
        }
        if (entries.size() == 1) archiveEntry = entries.first();
        else if (!smokeTest_) {
            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("Выбор игры из архива"));
            dialog.resize(620, 420);
            dialog.setMinimumSize(380, 260);
            auto* layout = new QVBoxLayout(&dialog);
            auto* label = new QLabel(QStringLiteral("Выберите игру для загрузки:"), &dialog);
            layout->addWidget(label);
            auto* list = new QListWidget(&dialog);
            list->setObjectName(QStringLiteral("archiveGameList"));
            list->setAccessibleName(QStringLiteral("Игры в архиве"));
            list->setSelectionMode(QAbstractItemView::SingleSelection);
            list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
            list->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            list->setTextElideMode(Qt::ElideNone);
            for (const auto& entry : entries) {
                auto* item = new QListWidgetItem(entry.name, list);
                item->setData(Qt::UserRole, entry.index);
                item->setToolTip(QStringLiteral("%1\n%2 КиБ").arg(entry.name).arg((entry.size+1023)/1024));
            }
            list->setCurrentRow(0);
            layout->addWidget(list, 1);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            auto* loadButton = buttons->button(QDialogButtonBox::Ok);
            loadButton->setText(QStringLiteral("Загрузить"));
            loadButton->setDefault(true);
            buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
            layout->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
            list->setFocus();
            if (dialog.exec() != QDialog::Accepted || !list->currentItem()) {
                setLoadingUi(false);
                view_->setFocus();
                updateInputEnabled();
                return;
            }
            archiveEntry = entries.at(list->currentRow());
        }
    }
    // Queue the selected profile's port connections before the load command,
    // so the first emulated frame already has the correct console devices.
    input_->setProfile(isCdPath(path) ? ConsoleProfile::MegaDrive :
                       profileFor(QFileInfo(archiveEntry ? archiveEntry->name : path)));
    settings_.setValue(QStringLiteral("files/lastDirectory"), file.absolutePath());
    if (archiveEntry) session_.loadGame(file.absoluteFilePath(), *archiveEntry);
    else session_.loadGame(file.absoluteFilePath());
}

void MainWindow::editSettings() {
    releaseInput();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("emulatorSettingsDialog"));
    dialog.setWindowTitle(QStringLiteral("Настройки эмулятора — Kega Refusion"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    const bool jpeg = settings_.value(QStringLiteral("screenshot/format"), QStringLiteral("png"))
                          .toString() == QLatin1String("jpeg");
    auto* screenshotFormat = new QComboBox(&dialog);
    screenshotFormat->setObjectName(QStringLiteral("screenshotFormat"));
    screenshotFormat->addItem(QStringLiteral("PNG (без потерь)"), QStringLiteral("png"));
    screenshotFormat->addItem(QStringLiteral("JPEG"), QStringLiteral("jpeg"));
    screenshotFormat->setCurrentIndex(jpeg ? 1 : 0);
    form->addRow(QStringLiteral("Формат скриншотов:"), screenshotFormat);
    const bool fastCd = settings_.value(QStringLiteral("emulation/fastCdLoading"), false).toBool();
    auto* cdLoading = new QComboBox(&dialog);
    cdLoading->setObjectName(QStringLiteral("cdLoadingMode"));
    cdLoading->addItem(QStringLiteral("Обычная (как на консоли)"), false);
    cdLoading->addItem(QStringLiteral("Ускоренная"), true);
    cdLoading->setCurrentIndex(fastCd ? 1 : 0);
    form->addRow(QStringLiteral("Загрузка CD:"), cdLoading);
    auto* cdHelp = new QLabel(QStringLiteral(
        "Ускоренный режим убирает задержки поиска дорожек. "
        "Если игра зависает или звук рассинхронизируется, верните обычный режим."), &dialog);
    cdHelp->setWordWrap(true);
    cdHelp->setMinimumWidth(360);
    form->addRow(cdHelp);
    const bool noSpriteLimit = settings_.value(QStringLiteral("emulation/removeSpriteLimit"), false).toBool();
    auto* spriteLimit = new QComboBox(&dialog);
    spriteLimit->setObjectName(QStringLiteral("spriteLimitMode"));
    spriteLimit->addItem(QStringLiteral("Как на консоли"), false);
    spriteLimit->addItem(QStringLiteral("Снять ограничение"), true);
    spriteLimit->setCurrentIndex(noSpriteLimit ? 1 : 0);
    form->addRow(QStringLiteral("Лимит спрайтов:"), spriteLimit);
    spriteLimit->setToolTip(QStringLiteral(
        "Уменьшает мигание и исчезновение спрайтов. "
        "В отдельных играх может нарушать графические эффекты."));
    const int memoryMiB = qBound(16, settings_.value(QStringLiteral("emulation/rewindMemoryMiB"), 128).toInt(), 1024);
    auto* rewindMemory = new QSpinBox(&dialog);
    rewindMemory->setObjectName(QStringLiteral("rewindMemoryLimit"));
    rewindMemory->setRange(16, 1024);
    rewindMemory->setSingleStep(16);
    rewindMemory->setSuffix(QStringLiteral(" МиБ"));
    rewindMemory->setValue(memoryMiB);
    form->addRow(QStringLiteral("Память перемотки:"), rewindMemory);
    auto* rewindHelp = new QLabel(QStringLiteral(
        "История ограничена и временем, и памятью. При заполнении удаляются самые старые кадры. "
        "Изменение лимита очищает историю; память выделяется по мере накопления."), &dialog);
    rewindHelp->setWordWrap(true);
    form->addRow(rewindHelp);
    rewindMemory->setToolTip(QStringLiteral(
        "Лимит сохранённой истории. Рабочие буферы и прочая память эмулятора учитываются отдельно."));
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("settingsButtons"));
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("ОК"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted) {
        if (rewindMemory->value() != memoryMiB) {
            settings_.setValue(QStringLiteral("emulation/rewindMemoryMiB"), rewindMemory->value());
            session_.setRewindMemoryMiB(rewindMemory->value());
        }
        if ((screenshotFormat->currentIndex() == 1) != jpeg)
            settings_.setValue(QStringLiteral("screenshot/format"), screenshotFormat->currentData());
        const bool selectedFastCd = cdLoading->currentData().toBool();
        if (selectedFastCd != fastCd) {
            settings_.setValue(QStringLiteral("emulation/fastCdLoading"), selectedFastCd);
            session_.setFastCdLoading(selectedFastCd);
        }
        const bool selectedNoSpriteLimit = spriteLimit->currentData().toBool();
        if (selectedNoSpriteLimit != noSpriteLimit) {
            settings_.setValue(QStringLiteral("emulation/removeSpriteLimit"), selectedNoSpriteLimit);
            session_.setRemoveSpriteLimit(selectedNoSpriteLimit);
        }
    }
    view_->setFocus();
    updateInputEnabled();
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
    if (screenshotAction_) screenshotAction_->setEnabled(session_.hasGame() && !loading_);
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
    case HardResetCommand: if (hardResetAction_->isEnabled()) hardResetAction_->trigger(); break;
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
        else if (key == Qt::Key_F12) commandAction = screenshotAction_;
    } else if (modifiers == Qt::ControlModifier && key == Qt::Key_R) {
        commandAction = resetAction_;
    } else if (modifiers == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_R) {
        commandAction = hardResetAction_;
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
        key == Qt::Key_F5 || key == Qt::Key_F8 || key == Qt::Key_F12)
        return QMainWindow::eventFilter(watched, event);
    if (input_->keyEvent(key, true)) return true;
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (loading_) { event->ignore(); return; }
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile() && isSupportedDropFile(url.toLocalFile())) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent* event) {
    if (loading_) { event->ignore(); return; }
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && isSupportedDropFile(url.toLocalFile())) {
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
