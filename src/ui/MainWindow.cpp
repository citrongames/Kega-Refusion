#include "i18n/Localization.h"
#include "MainWindow.h"
#include "DebugDialogs.h"
#include "AudioOutput.h"
#include "ControlsPage.h"
#include "CheatsDialog.h"
#include "CheatSearchDialog.h"
#include "AchievementsDialog.h"
#include "AchievementPositionPicker.h"
#include "AchievementNetwork.h"
#include "NetplaySettingsPage.h"
#include "UpdateDialog.h"
#include "update/UpdateService.h"
#include "net/NetplayController.h"
#include "net/NetplayTimeline.h"
#include <algorithm>
#include <cmath>
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
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QImageWriter>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
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
#include <QProgressDialog>
#include <QStyle>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QLabel>
#include <QLineEdit>
#include <QMediaDevices>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QSlider>
#include <QStackedWidget>
#include <QScrollArea>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QWindowStateChangeEvent>
#include <optional>

namespace {
constexpr int kBiosSettingsPage = 6;
constexpr int kControlsSettingsPage = 7;
constexpr int kNetplaySettingsPage = 8;

// Small native line icons; created only when opening settings, with palette-aware selection.
QIcon settingsSectionIcon(int section, const QPalette& palette) {
    QIcon icon;
    for (const auto mode : {QIcon::Normal, QIcon::Selected}) {
        QPixmap image(48, 48); image.fill(Qt::transparent); image.setDevicePixelRatio(2);
        QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);
        const QColor color = palette.color(mode == QIcon::Selected ? QPalette::HighlightedText : QPalette::Text);
        p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath path;
        switch (section) {
        case 0: // sliders
            for (int row=0; row<3; ++row) {
                const qreal y=6+6*row, x=row==1 ? 15 : 8;
                p.drawLine(QPointF(3,y),QPointF(x-2,y)); p.drawLine(QPointF(x+2,y),QPointF(21,y));
                p.drawEllipse(QPointF(x,y),2,2);
            } break;
        case 1: // cartridge
            p.drawRoundedRect(QRectF(4,3,16,18),2,2);
            p.drawRect(QRectF(7,6,10,6));
            for(int x=8;x<=16;x+=4)p.drawLine(x,18,x,21);
            break;
        case 2: // rewind
            path.moveTo(11,5);path.lineTo(3,12);path.lineTo(11,19);path.closeSubpath();
            path.moveTo(21,5);path.lineTo(13,12);path.lineTo(21,19);path.closeSubpath();
            p.drawPath(path);break;
        case 3: // recording
            p.drawEllipse(QPointF(12,12),8,8);p.setBrush(color);p.drawEllipse(QPointF(12,12),4,4);break;
        case 4: // trophy
            path.moveTo(7,3);path.lineTo(17,3);path.lineTo(17,9);path.cubicTo(17,17,7,17,7,9);path.closeSubpath();
            p.drawPath(path);p.drawLine(12,15,12,20);p.drawLine(8,21,16,21);
            p.drawArc(QRectF(3,4,8,7),90*16,180*16);p.drawArc(QRectF(13,4,8,7),270*16,180*16);break;
        case 5: // speaker
            path.moveTo(3,9);path.lineTo(7,9);path.lineTo(12,5);path.lineTo(12,19);path.lineTo(7,15);path.lineTo(3,15);path.closeSubpath();
            p.drawPath(path);p.drawArc(QRectF(10,6,10,12),-60*16,120*16);
            p.drawArc(QRectF(9,3,14,18),-60*16,120*16);break;
        case 6: // firmware chip
            p.drawRoundedRect(QRectF(6,6,12,12),1,1);p.drawRect(QRectF(9,9,6,6));
            for(int v=8;v<=16;v+=4){p.drawLine(v,3,v,6);p.drawLine(v,18,v,21);p.drawLine(3,v,6,v);p.drawLine(18,v,21,v);}break;
        case 7: // gamepad
            path.moveTo(7,7);path.lineTo(17,7);path.cubicTo(21,7,23,19,19,19);
            path.cubicTo(17,19,16,16,14,16);path.lineTo(10,16);
            path.cubicTo(8,16,7,19,5,19);path.cubicTo(1,19,3,7,7,7);
            p.drawPath(path);p.drawLine(6,10,6,14);p.drawLine(4,12,8,12);
            p.setBrush(color);p.drawEllipse(QPointF(16,12),1,1);p.drawEllipse(QPointF(19,10),1,1);break;
        case 8: // connected peers
            p.drawLine(QPointF(7,8),QPointF(17,6));p.drawLine(QPointF(7,8),QPointF(13,18));
            p.drawLine(QPointF(17,6),QPointF(13,18));
            p.setBrush(color);p.drawEllipse(QPointF(7,8),2,2);
            p.drawEllipse(QPointF(17,6),2,2);p.drawEllipse(QPointF(13,18),2,2);break;
        }
        p.end();icon.addPixmap(image,mode);
    }
    return icon;
}

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
    view_->setMovieCounterAsTime(settings_.value(QStringLiteral("recording/counterUnit"),
        QStringLiteral("time")).toString() != QLatin1String("frames"));
    view_->setAcceptDrops(true);
    view_->installEventFilter(this);
    setCentralWidget(view_);
    connect(view_, &GameView::openGameRequested, this, [this] {
        if (!loading_ && !session_.hasGame()) openGameDialog();
    });
    audio_ = new AudioOutput(this);
    audio_->setLatencyProfile(AudioQueue::normalizeProfile(
        settings_.value(QStringLiteral("audio/latencyProfile"), 0).toInt()));
    audio_->setOutputDeviceId(settings_.value(QStringLiteral("audio/outputDeviceId")).toByteArray());
    input_ = new InputManager(settings_.fileName(), this);
    connect(audio_, &AudioOutput::unavailable, this, [](const QString& text) {
        qWarning("%s", qPrintable(text));
    });
    connect(input_, &InputManager::maskChanged, this, [this](unsigned port, unsigned mask) {
        if (port < padMasks_.size()) { padMasks_[port] = mask; session_.setButtons(port,mask | (port < gunMasks_.size() ? gunMasks_[port] : 0)); }
    });
    connect(input_, &InputManager::connectionChanged, this, [this](unsigned port, bool connected) {
        if (!applyingSessionControllers_) session_.setControllerConnected(port, connected);
    });
    connect(input_, &InputManager::controllerTypeChanged, this, [this](unsigned port, bool sixButton) {
        if (!applyingSessionControllers_) session_.setControllerSixButton(port, sixButton);
    });
    connect(input_, &InputManager::adapterChanged, this, [this](int adapter) {
        if (!applyingSessionControllers_) session_.setControllerAdapter(ControllerAdapter(adapter));
    });
    session_.setControllerAdapter(input_->adapter());
    for (unsigned port = 0; port < MaxControllerPlayers; ++port) {
        session_.setControllerConnected(port, input_->isConnected(port));
        session_.setControllerSixButton(port, input_->sixButton(port));
    }
    connect(input_, &InputManager::configurationChanged, this, &MainWindow::updateGunConfiguration);
    connect(&session_, &EmulationSession::jcartChanged, input_, &InputManager::setJCart);
    view_->setMouseCaptureEnabled(input_->mouseCaptureEnabled());
    connect(input_, &InputManager::mouseCaptureChanged, view_, &GameView::setMouseCaptureEnabled);
    connect(input_, &InputManager::mouseMotion, this, &MainWindow::deliverMouseMotion);
    connect(input_, &InputManager::paddleMoved, this, [this](unsigned player, int position) {
        session_.setPaddlePosition(player, position);
    });
    connect(view_, &GameView::analogMouseMotion, this, &MainWindow::deliverMouseMotion);
    connect(view_, &GameView::analogPaddleMoved, this, [this](unsigned player, int position) {
        session_.setPaddlePosition(player, position);
    });
    connect(view_, &GameView::analogPointerReset, this, [this] {
        mouseMotionRemainder_.fill(QPointF());
        session_.clearMouseMotion();
    });
    connect(input_, &InputManager::aimMoved, this, [this](unsigned player, double dx, double dy) {
        if (player >= 3 || input_->aimWithMouse(player)) return;
        updateGunAim(player,gunAims_[player] + QPointF(dx,dy),false);
    });
    connect(view_, &GameView::gunAimChanged, this, [this](unsigned player,double x,double y,bool offscreen) {
        updateGunAim(player,QPointF(x,y),offscreen);
    });
    connect(view_, &GameView::gunButtonsChanged, this, [this](unsigned player,unsigned mask) {
        if (player >= 3) return;
        mask = input_->mouseButtonMask(player, mask);
        gunMasks_[player] = mask;
        session_.setButtons(player,padMasks_[player] | mask);
    });
    connect(input_, &InputManager::commandChanged, this, &MainWindow::handleInputCommand);
    connect(input_, &InputManager::notification, this, [](const QString& text) {
        qInfo("%s", qPrintable(text));
    });

    recentPaths_ = settings_.value(QStringLiteral("recent/files")).toStringList();
    stateSlot_ = qBound(0, settings_.value(QStringLiteral("emulation/slot"), 0).toInt(), 9);
    fastForwardFactor_ = settings_.value(QStringLiteral("emulation/fastForwardFactor"), 4).toInt();
    if (!QList<int>{0,2,3,4,6,8,12,16,32}.contains(fastForwardFactor_)) fastForwardFactor_ = 4;
    // Preserve an explicitly disabled legacy history, but discard its duration.
    rewindEnabled_ = settings_.value(QStringLiteral("emulation/rewindEnabled"),
        settings_.value(QStringLiteral("emulation/rewindSeconds"), 30).toInt() != 0).toBool();
    if (settings_.contains(QStringLiteral("emulation/rewindSeconds"))) {
        settings_.setValue(QStringLiteral("emulation/rewindEnabled"), rewindEnabled_);
        settings_.remove(QStringLiteral("emulation/rewindSeconds"));
    }
    pauseOnFocusLoss_ = settings_.value(QStringLiteral("emulation/pauseOnFocusLoss"), true).toBool();
    hideAchievementAvailability_ = settings_.value(QStringLiteral("achievements/hideAvailabilityNotices"), true).toBool();
    achievementNotifications_ = settings_.value(QStringLiteral("achievements/notifications"), true).toBool();
    achievementSound_ = settings_.value(QStringLiteral("achievements/sound"), true).toBool();
    smsFmEnabled_ = settings_.value(QStringLiteral("audio/smsFmEnabled"), true).toBool();
    achievementPosition_ = qBound(0, settings_.value(QStringLiteral("achievements/position"), 0).toInt(), 5);
    view_->setAchievementPosition(achievementPosition_);
    region_ = qBound(0, settings_.value(QStringLiteral("emulation/region"), 0).toInt(), 3);
    blarggFilter_ = qBound(0, settings_.value(QStringLiteral("view/blarggFilter"), 0).toInt(), 4);
    if (blarggFilter_ == 1) {
        blarggFilter_ = 0;
        settings_.setValue(QStringLiteral("view/blarggFilter"), 0);
    }
    session_.setRegion(region_);
    session_.setBlarggFilter(blarggFilter_);
    session_.setSmsFmEnabled(smsFmEnabled_);
    session_.setAutoFixChecksum(settings_.value(QStringLiteral("emulation/autoFixChecksum"), false).toBool());
    session_.setFastCdLoading(settings_.value(QStringLiteral("emulation/fastCdLoading"), false).toBool());
    session_.setRemoveSpriteLimit(settings_.value(QStringLiteral("emulation/removeSpriteLimit"), false).toBool());
    session_.setExpansionDevices(settings_.value(QStringLiteral("emulation/attachCd"), false).toBool(),
                                 settings_.value(QStringLiteral("emulation/attach32x"), false).toBool());
    connect(&session_, &EmulationSession::performanceMetricsChanged,
            view_, &GameView::setPerformanceMetrics);
    session_.setFastForwardFactor(fastForwardFactor_);
    session_.setRewindEnabled(rewindEnabled_);
    session_.setRewindMemoryMiB(settings_.value(QStringLiteral("emulation/rewindMemoryMiB"), 128).toInt());
    view_->setFastForwardFactor(fastForwardFactor_);
    netplay_ = new NetplayController(session_, dataPath_, settings_, this);
    connect(netplay_, &NetplayController::openRomRequested, this, &MainWindow::openRom);
    connect(netplay_, &NetplayController::notice, this, [this](const QString& text) {
        view_->showOsd(Localization::text(text), 3000, QStringLiteral("netplay"));
    });
    connect(netplay_, &NetplayController::sessionEnded, this, [this](const QString& message) {
        auto* dialog = new QMessageBox(QMessageBox::Information,
            Localization::text(QStringLiteral("Онлайн-сессия завершена")), message, QMessageBox::Ok, this);
        dialog->setTextFormat(Qt::PlainText);
        dialog->setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
    });
    connect(netplay_, &NetplayController::connectionWaitingChanged, view_, &GameView::setConnectionWaiting);
    connect(netplay_, &NetplayController::readinessWaitingChanged, view_, &GameView::setReadinessWaiting);
    connect(netplay_, &NetplayController::activeChanged, this, [this](bool active) {
        if (netplayLeaveAction_) netplayLeaveAction_->setEnabled(active);
        updateMovieActions();
        updateGunPresentation();
        updateInputEnabled();
    });
    connect(&session_, &EmulationSession::networkRoleChanged, this, [this](int) { updateGunPresentation(); });
    debugAudioGains_.fill(100);
    buildMenus();
    updateService_ = new UpdateService(this);
    connect(updateService_, &UpdateService::checked, this,
            [this](const QString& version, const QString& notes) {
        checkingForUpdates_ = false;
        availableUpdateVersion_ = version;
        availableUpdateNotes_ = notes;
        updateError_.clear();
        // Background checks only open a window when there is an actual update.
        if (!version.isEmpty() || updateCheckInteractive_) showUpdateDialog(updateCheckInteractive_);
        else refreshUpdateDialog();
    });
    connect(updateService_, &UpdateService::failed, this, [this](const QString& message) {
        checkingForUpdates_ = false;
        installingUpdate_ = false;
        updateError_ = message;
        if (updateCheckInteractive_) showUpdateDialog();
        else refreshUpdateDialog();
    });
    connect(updateService_, &UpdateService::progress, this, [this](qint64 received, qint64 total) {
        updateBytesReceived_ = received;
        updateBytesTotal_ = total;
        if (updateDialog_ && installingUpdate_ && !restartingUpdate_)
            updateDialog_->setProgress(received, total);
    });
    connect(updateService_, &UpdateService::restarting, this, [this] {
        restartingUpdate_ = true;
        refreshUpdateDialog();
        close(); // The ordinary close path finalizes recordings, settings and saves.
    });
    connect(&session_, &EmulationSession::debugControlsChanged, this,
            [this](quint32 videoAvailable, quint64 audioAvailable) {
        const bool changed = debugVideoAvailable_ != videoAvailable ||
                             debugAudioAvailable_ != audioAvailable;
        debugVideoAvailable_ = videoAvailable;
        debugAudioAvailable_ = audioAvailable;
        if (!videoAvailable && !audioAvailable) {
            debugVideoDisabled_ = 0;
            debugAudioDisabled_ = 0;
            debugAudioGains_.fill(100);
        } else {
            debugVideoDisabled_ &= videoAvailable;
            debugAudioDisabled_ &= audioAvailable;
        }
        if (changed || (!videoAvailable && !audioAvailable)) updateDebugDialogs();
    });
    connect(&session_, &EmulationSession::masterSystemChanged,
            smsFmAction_, &QAction::setVisible);
    for (QAction* action : menuBar()->actions()) {
        QMenu* menu = action->menu();
        if (!menu) continue;
        connect(menu, &QMenu::aboutToShow, this, [this] {
            menuPauseActive_ = true;
            if (!netplay_->active() && !videoExporting_) session_.setBackgroundPaused(true);
            releaseInput();
        });
        connect(menu, &QMenu::aboutToHide, this, [this] {
            // A neighboring top menu may open while this one is closing.
            QTimer::singleShot(0, this, [this] {
                menuPauseActive_ = false;
                for (QAction* action : menuBar()->actions()) {
                    if (QMenu* menu = action->menu(); menu && menu->isVisible()) {
                        menuPauseActive_ = true;
                        break;
                    }
                }
                updateInputEnabled();
            });
        });
    }
    updateGunConfiguration();
    connect(&session_, &EmulationSession::movieHardwareChanged, this,
            [this](const QByteArray& controllersJson) {
        // A nonempty snapshot already belongs to the core; an empty one restores local settings.
        const QScopedValueRollback<bool> applying(applyingSessionControllers_, !controllersJson.isEmpty());
        input_->setMovieControllers(controllersJson);
    });
    connect(&session_, &EmulationSession::movieStatusChanged, this,
            [this](int mode, const QString& path, quint64 frame, quint64 length, double fps) {
        const bool completed = moviePending_ && (mode == moviePendingMode_ ||
                                (moviePendingMode_ == 2 && mode == 3));
        const bool changed = movieMode_ != mode || moviePath_ != path;
        if (completed) moviePending_ = false;
        movieMode_ = mode;
        moviePath_ = path;
        view_->setMovieStatus(mode, frame, length, fps);
        if (completed || changed) updateMovieActions();
        if (closeAfterMovie_ && mode == 0) {
            closeAfterMovie_ = false;
            QTimer::singleShot(0, this, [this] { close(); });
        }
    });
    connect(&session_, &EmulationSession::movieCompatibilityWarning, this,
            [this](const QString& message) {
        view_->showOsd(message, 10000, QStringLiteral("recordingCompatibility"));
    });
    connect(&session_, &EmulationSession::videoRecordingChanged, this,
            [this](bool active, bool exporting, const QString& path) {
        const bool started = active && !videoActive_;
        videoPending_ = false;
        videoActive_ = active;
        // Keep the export lock until the writer reports finalization.
        if (active) videoExporting_ = exporting;
        updateMovieActions();
        updateDebugDialogs();
        updateInputEnabled();
        if (started && !exporting)
            view_->showOsd(Localization::text(QStringLiteral("Идёт запись видео: %1"))
                               .arg(QFileInfo(path).fileName()), 3000, QStringLiteral("video"));
    });
    connect(&session_, &EmulationSession::videoExportProgress, this,
            [this](quint64 done, quint64 total) {
        if (!videoProgress_ || videoStopping_) return;
        if (!total) {
            videoProgress_->setRange(0, 0);
            return;
        }
        videoProgress_->setRange(0, 1000);
        videoProgress_->setValue(int(std::min<long double>(1000,
            static_cast<long double>(done) * 1000 / total)));
        videoProgress_->setLabelText(Localization::text(QStringLiteral("Экспорт: %1 / %2 кадров"))
                                        .arg(done).arg(total));
    });
    connect(&session_, &EmulationSession::videoRecordingFinished, this,
            [this](const QString& path, const QString& error) {
        const bool exporting = videoExporting_;
        const bool stopped = videoStopping_;
        videoActive_ = videoPending_ = videoExporting_ = videoStopping_ = false;
        if (videoProgress_) {
            videoProgress_->close();
            videoProgress_->deleteLater();
            videoProgress_ = nullptr;
        }
        updateMovieActions();
        updateDebugDialogs();
        updateInputEnabled();
        if (!error.isEmpty()) {
            qWarning("Video recording failed: %s", qPrintable(error));
            if (!smokeTest_) {
                auto* dialog = new QMessageBox(QMessageBox::Warning,
                    Localization::text(QStringLiteral("Видео")), error, QMessageBox::Ok, this);
                dialog->setTextFormat(Qt::PlainText);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->open();
            }
        } else if (!path.isEmpty()) {
            const QString text = exporting && stopped
                ? Localization::text(QStringLiteral("Экспорт остановлен. Готовая часть сохранена: %1"))
                : Localization::text(QStringLiteral("Видео сохранено: %1"));
            view_->showOsd(text.arg(QDir::toNativeSeparators(path)), 5000, QStringLiteral("video"));
        }
        if (closeAfterVideo_) {
            closeAfterVideo_ = false;
            QTimer::singleShot(0, this, [this] { close(); });
        }
    });
    achievementNetwork_ = new AchievementNetwork(dataPath_, this);
    connect(&session_, &EmulationSession::achievementHttpRequest,
            achievementNetwork_, &AchievementNetwork::perform);
    connect(achievementNetwork_, &AchievementNetwork::response,
            &session_, &EmulationSession::achievementResponse);
    connect(&session_, &EmulationSession::achievementsChanged, this,
            [this](const AchievementsState& state) {
        const bool changedGame = state.gameId != achievementsState_.gameId;
        if (!state.enabled || changedGame) view_->clearAchievementNotices();
        if (!state.enabled || !state.gameId) {
            achievementNetwork_->prefetchBadges({});
        } else if (changedGame || (achievementsState_.disconnected && !state.disconnected)) {
            QStringList badges;
            badges.reserve(state.entries.size() * 2);
            for (const auto& entry : state.entries) {
                if (!entry.badgeUrl.isEmpty()) badges.append(entry.badgeUrl);
                if (!entry.lockedBadgeUrl.isEmpty()) badges.append(entry.lockedBadgeUrl);
            }
            achievementNetwork_->prefetchBadges(badges);
        }
        achievementsState_ = state;
    });
    connect(view_, &GameView::achievementBadgeRequested,
            achievementNetwork_, &AchievementNetwork::requestBadge);
    connect(achievementNetwork_, &AchievementNetwork::badgeReady,
            view_, &GameView::setAchievementBadge);
    connect(view_, &GameView::achievementPopupStarted, this, [this](bool sound) {
        if (sound && achievementSound_ && achievementNotifications_ && osdAction_->isChecked() &&
            !muteAction_->isChecked() && !pauseAction_->isChecked() && !fastForwardActive_)
            audio_->playAchievementSound();
    });
    connect(&session_, &EmulationSession::achievementNotice, this,
            [this](const AchievementNotice& notice) {
        if (!achievementNotifications_ || !osdAction_->isChecked() ||
            (hideAchievementAvailability_ && notice.availabilityNotice) ||
            (notice.kind == AchievementNotice::Completed && !notice.hardcore)) return;
        if (notice.kind == AchievementNotice::LoadFailed) {
            view_->showOsd(notice.title, 3500, QStringLiteral("achievementLoadFailed"));
            return;
        }
        QString heading, title = notice.title;
        if (notice.kind == AchievementNotice::GameReady) {
            heading = Localization::text(QStringLiteral("ИГРА РАСПОЗНАНА"));
        } else if (notice.kind == AchievementNotice::Unlocked) {
            heading = Localization::text(QStringLiteral("ДОСТИЖЕНИЕ ОТКРЫТО"));
            if (notice.rarity > 0.0) heading += QStringLiteral(" · %1%").arg(notice.rarity, 0, 'f', 2);
            title += Localization::text(QStringLiteral(" · %1 очк.")).arg(notice.points);
        } else if (notice.kind == AchievementNotice::Completed) {
            heading = Localization::text(QStringLiteral("МАСТЕРСТВО"));
        }
        view_->showAchievementNotice(heading, title, notice.description, notice.badgeUrl,
                                     notice.kind != AchievementNotice::GameReady);
    });
    connect(&session_, &EmulationSession::achievementsHardcoreChanged, this,
            [this](bool enabled) {
        if (achievementsHardcore_ == enabled) return;
        achievementsHardcore_ = enabled;
        if (enabled && debugVideoDisabled_) {
            debugVideoDisabled_ = 0;
            applyDebugMasks();
        }
        updateDebugDialogs();
        if (enabled) {
            frameAdvanceHeld_ = frameAdvancePadHeld_ = false;
            session_.setFrameAdvanceHeld(false);
            frameBackHeld_ = frameBackPadHeld_ = false;
            session_.setFrameBackHeld(false);
            rewindHeld_ = rewindPadHeld_ = false;
            if (rewindAction_->isChecked()) rewindAction_->setChecked(false);
            updateRewind();
        }
        updateMovieActions();
        updateInputEnabled();
    });
    session_.configureAchievements(
        settings_.value(QStringLiteral("achievements/enabled"), true).toBool(),
        AchievementsHardcoreApproved &&
            settings_.value(QStringLiteral("achievements/hardcore"), false).toBool());
    connect(&session_, &EmulationSession::cheatsChanged, this,
            [this](const CheatList& entries,int platform,bool supported,const QString& detail) {
        cheats_ = entries; cheatPlatform_ = Cheats::Platform(platform); cheatsSupported_ = supported;
        cheatsAction_->setEnabled(session_.hasGame() && supported && !loading_ &&
                                  movieMode_ == 0 && !videoExporting_ && !achievementsHardcore_);
        cheatsAction_->setToolTip(detail);
    });
    connect(&session_, &EmulationSession::discStateChanged, this,
            [this](const QStringList& paths, int index, bool ejected) {
        discPaths_ = paths;
        discIndex_ = index;
        discEjected_ = ejected;
        updateDiscMenu();
    });
    connect(view_, &GameView::spriteOwnershipRequested,
            &session_, &EmulationSession::setSpriteOwnershipEnabled);
    connect(view_, &GameView::shaderFailed, this, [this](const QString& error) {
        settings_.setValue(QStringLiteral("view/shaderPreset"), QString());
        updateShaderActions();
        if (!smokeTest_) QMessageBox::warning(this, Localization::text(QStringLiteral("Шейдер")), error);
    });
    QString shader = settings_.value(QStringLiteral("view/shaderPreset")).toString();
    if (shader == QLatin1String(":/shaders/gdapt.glslp")) {
        // Only the retired built-in resource migrates. External presets retain
        // their own paths and parameters, even if their filename is gdapt.glslp.
        settings_.remove(shaderParameterPrefix(shader));
        shader = QStringLiteral(":/shaders/refusion-dither.glslp");
    }
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
        QImage image, spriteOwnership; double ratio = 4.0 / 3.0;
        if (session_.takeVideoFrame(image, ratio, &spriteOwnership))
            view_->setFrame(image, ratio, spriteOwnership);
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
        updateGunConfiguration();
        currentGameName_ = displayName(contentName);
        setWindowTitle(QStringLiteral("%1 — Kega Refusion").arg(currentGameName_));
        recentPaths_.removeAll(path);
        recentPaths_.prepend(path);
        while (recentPaths_.size() > 10) recentPaths_.removeLast();
        settings_.setValue(QStringLiteral("recent/files"), recentPaths_);
        updateRecentMenu();
        view_->setFocus();
        updateInputEnabled();
        updateMovieActions();
    });
    connect(&session_, &EmulationSession::gameClosed, this, [this] {
        setLoadingUi(false);
        currentRom_.clear(); currentGameName_.clear();
        padMasks_.fill(0); gunMasks_.fill(0);
        view_->clearGame();
        movieMode_ = 0;
        moviePending_ = false;
        moviePath_.clear();
        updateMovieActions();
        setWindowTitle(QStringLiteral("Kega Refusion"));
        for (auto* action : {pauseAction_,resetAction_,hardResetAction_,fastForwardAction_,
                             rewindAction_,saveAction_,loadAction_,saveFileAction_,loadFileAction_}) action->setEnabled(false);
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
        view_->showOsd(Localization::text(text), 2000, QStringLiteral("action"));
    });
    connect(&session_, &EmulationSession::errorOccurred, this, [this](const QString& text) {
        if (loading_) input_->setProfile(activeInputProfile_);
        setLoadingUi(false);
        moviePending_ = false;
        closeAfterMovie_ = false;
        updateMovieActions();
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
    if (volumeSaveTimer_ && volumeSaveTimer_->isActive()) flushVolumePreference();
    if (netplay_) netplay_->leaveSession();
    // The direct audio producer must stop before QObject destroys AudioOutput.
    session_.shutdown();
    netplay_->finishTemporarySessions();
}

void MainWindow::buildMenus() {
    auto shortcut = [this](QAction* action) {
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
    };
    QMenu* fileMenu = menuBar()->addMenu(Localization::text(QStringLiteral("&Файл")));
    openGameAction_ = fileMenu->addAction(Localization::text(QStringLiteral("Открыть игру…")));
    openGameAction_->setShortcut(QKeySequence::Open);
    shortcut(openGameAction_);
    connect(openGameAction_, &QAction::triggered, this, &MainWindow::openGameDialog);
    closeGameAction_ = fileMenu->addAction(Localization::text(QStringLiteral("Закрыть игру")));
    closeGameAction_->setEnabled(false);
    connect(closeGameAction_, &QAction::triggered, this, [this] {
        if (!session_.hasGame() || loading_ || videoExporting_) return;
        releaseInput();
        loading_ = true; // Block new opens until the worker has flushed and unloaded.
        updateMovieActions();
        updateInputEnabled();
        session_.closeGame();
    });
    recentMenu_ = fileMenu->addMenu(Localization::text(QStringLiteral("Недавние игры")));
    updateRecentMenu();
    movieMenu_ = fileMenu->addMenu(Localization::text(QStringLiteral("Запись управления")));
    movieMenu_->setObjectName(QStringLiteral("movieMenu"));
    movieRecordAction_ = movieMenu_->addAction(Localization::text(QStringLiteral("Начать запись…")));
    movieRecordAction_->setObjectName(QStringLiteral("movieRecord"));
    connect(movieRecordAction_, &QAction::triggered, this, [this] {
        const QString path = chooseMovieSavePath(false);
        if (path.isEmpty()) return;
        moviePending_ = true;
        moviePendingMode_ = 1;
        updateMovieActions();
        session_.startMovieRecording(path);
    });
    movieResumeAction_ = movieMenu_->addAction(Localization::text(QStringLiteral("Продолжить запись…")));
    movieResumeAction_->setObjectName(QStringLiteral("movieResume"));
    connect(movieResumeAction_, &QAction::triggered, this, [this] {
        releaseInput();
        const QString directory = QDir(dataPath_).filePath(QStringLiteral("movies"));
        const QString path = QFileDialog::getOpenFileName(this, Localization::text(QStringLiteral("Продолжить запись управления")),
            directory, Localization::text(QStringLiteral("Записи управления (*.krm);;Все файлы (*)")));
        view_->setFocus();
        if (path.isEmpty()) return;
        moviePending_ = true;
        moviePendingMode_ = 1;
        updateMovieActions();
        session_.resumeMovieRecording(path);
    });
    moviePlayAction_ = movieMenu_->addAction(Localization::text(QStringLiteral("Воспроизвести…")));
    moviePlayAction_->setObjectName(QStringLiteral("moviePlay"));
    connect(moviePlayAction_, &QAction::triggered, this, [this] {
        releaseInput();
        const QString directory = QDir(dataPath_).filePath(QStringLiteral("movies"));
        const QString path = QFileDialog::getOpenFileName(this, Localization::text(QStringLiteral("Воспроизвести запись управления")),
            directory, Localization::text(QStringLiteral("Записи управления (*.krm);;Все файлы (*)")));
        view_->setFocus();
        if (path.isEmpty()) return;
        moviePending_ = true;
        moviePendingMode_ = 2;
        updateMovieActions();
        session_.playMovie(path);
    });
    movieMenu_->addSeparator();
    movieTakeoverAction_ = movieMenu_->addAction(Localization::text(QStringLiteral("Перехватить управление")));
    movieTakeoverAction_->setObjectName(QStringLiteral("movieTakeover"));
    connect(movieTakeoverAction_, &QAction::triggered, this, [this] {
        releaseInput();
        moviePending_ = true;
        moviePendingMode_ = 0;
        updateMovieActions();
        session_.takeOverMovie();
        view_->setFocus();
    });
    movieRerecordAction_ = movieMenu_->addAction(Localization::text(QStringLiteral("Записывать с этого места…")));
    movieRerecordAction_->setObjectName(QStringLiteral("movieRerecord"));
    connect(movieRerecordAction_, &QAction::triggered, this, [this] {
        const QString path = chooseMovieSavePath(true);
        if (path.isEmpty()) return;
        moviePending_ = true;
        moviePendingMode_ = 1;
        updateMovieActions();
        session_.rerecordMovie(path);
    });
    movieStopAction_ = movieMenu_->addAction(Localization::text(QStringLiteral("Остановить")));
    movieStopAction_->setObjectName(QStringLiteral("movieStop"));
    connect(movieStopAction_, &QAction::triggered, this, [this] {
        releaseInput();
        moviePending_ = true;
        moviePendingMode_ = 0;
        updateMovieActions();
        session_.stopMovie();
        view_->setFocus();
    });
    videoMenu_ = fileMenu->addMenu(Localization::text(QStringLiteral("Видео")));
    videoMenu_->setObjectName(QStringLiteral("videoMenu"));
    videoStartAction_ = videoMenu_->addAction(Localization::text(QStringLiteral("Начать запись видео…")));
    videoStartAction_->setObjectName(QStringLiteral("videoRecord"));
    connect(videoStartAction_, &QAction::triggered, this, &MainWindow::startVideoCapture);
    videoExportAction_ = videoMenu_->addAction(Localization::text(QStringLiteral("Экспортировать запись управления…")));
    videoExportAction_->setObjectName(QStringLiteral("videoExport"));
    videoExportAction_->setToolTip(Localization::text(QStringLiteral("Откройте игру, для которой сохранена запись управления.")));
    connect(videoExportAction_, &QAction::triggered, this, &MainWindow::exportInputVideo);
    videoMenu_->addSeparator();
    videoStopAction_ = videoMenu_->addAction(Localization::text(QStringLiteral("Остановить запись видео")));
    videoStopAction_->setObjectName(QStringLiteral("videoStop"));
    connect(videoStopAction_, &QAction::triggered, this, &MainWindow::stopVideoCapture);
    updateMovieActions();
    QMenu* segaCdMenu = fileMenu->addMenu(QStringLiteral("Sega CD"));
    changeDiscAction_ = segaCdMenu->addAction(Localization::text(QStringLiteral("Сменить диск…")));
    connect(changeDiscAction_, &QAction::triggered, this, [this] {
        releaseInput();
        const QString initial = discIndex_ >= 0 && discIndex_ < discPaths_.size()
            ? QFileInfo(discPaths_[discIndex_]).absolutePath()
            : settings_.value(QStringLiteral("files/lastDirectory"), QDir::homePath()).toString();
        const QString path = QFileDialog::getOpenFileName(this, Localization::text(QStringLiteral("Сменить диск")), initial,
            Localization::text(QStringLiteral("Диски Sega CD (*.cue *.iso *.chd);;Все файлы (*)")));
        if (!path.isEmpty()) session_.changeDisc(path);
        view_->setFocus();
    });
    ejectDiscAction_ = segaCdMenu->addAction(Localization::text(QStringLiteral("Извлечь диск")));
    ejectDiscAction_->setCheckable(true);
    connect(ejectDiscAction_, &QAction::triggered, this, [this](bool ejected) {
        releaseInput();
        session_.setDiscEjected(ejected);
        ejectDiscAction_->setChecked(discEjected_);
        view_->setFocus();
    });
    discMenu_ = segaCdMenu->addMenu(Localization::text(QStringLiteral("Диски плейлиста")));
    discGroup_ = new QActionGroup(this);
    discGroup_->setExclusive(true);
    updateDiscMenu();
    fileMenu->addSeparator();
    screenshotAction_ = fileMenu->addAction(Localization::text(QStringLiteral("Сохранить скриншот")));
    screenshotAction_->setObjectName(QStringLiteral("saveScreenshot"));
    screenshotAction_->setShortcut(QKeySequence(QStringLiteral("F12")));
    screenshotAction_->setAutoRepeat(false);
    screenshotAction_->setEnabled(false);
    screenshotAction_->setToolTip(Localization::text(QStringLiteral("Чистое изображение игры без OSD; папка screenshots рядом с settings.ini.")));
    shortcut(screenshotAction_);
    connect(screenshotAction_, &QAction::triggered, this, &MainWindow::saveScreenshot);
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(Localization::text(QStringLiteral("Выход")));
    quitAction->setShortcut(QKeySequence::Quit);
    shortcut(quitAction);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu* emulationMenu = menuBar()->addMenu(Localization::text(QStringLiteral("&Эмуляция")));
    resetAction_ = emulationMenu->addAction(Localization::text(QStringLiteral("Сброс")));
    resetAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    resetAction_->setAutoRepeat(false);
    resetAction_->setEnabled(false);
    shortcut(resetAction_);
    connect(resetAction_, &QAction::triggered, this, [this] {
        releaseInput();
        session_.resetGame();
    });
    hardResetAction_ = emulationMenu->addAction(Localization::text(QStringLiteral("Перезапуск питания")));
    hardResetAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    hardResetAction_->setAutoRepeat(false);
    hardResetAction_->setEnabled(false);
    shortcut(hardResetAction_);
    connect(hardResetAction_, &QAction::triggered, this, [this] {
        releaseInput(); session_.hardResetGame();
    });
    emulationMenu->addSeparator();
    pauseAction_ = emulationMenu->addAction(Localization::text(QStringLiteral("Пауза")));
    pauseAction_->setCheckable(true);
    pauseAction_->setShortcut(QKeySequence(QStringLiteral("P")));
    pauseAction_->setAutoRepeat(false);
    pauseAction_->setEnabled(false);
    shortcut(pauseAction_);
    connect(pauseAction_, &QAction::toggled, this, [this](bool paused) {
        session_.setPaused(paused);
    });
    frameAdvanceAction_ = emulationMenu->addAction(Localization::text(QStringLiteral("Следующий кадр")));
    frameAdvanceAction_->setShortcut(QKeySequence(Qt::Key_Period));
    frameAdvanceAction_->setAutoRepeat(false);
    frameAdvanceAction_->setEnabled(false);
    shortcut(frameAdvanceAction_);
    connect(frameAdvanceAction_, &QAction::triggered, this, [this] { session_.advanceFrame(); });
    frameBackAction_ = emulationMenu->addAction(Localization::text(QStringLiteral("Предыдущий кадр")));
    frameBackAction_->setShortcut(QKeySequence(Qt::Key_Comma));
    frameBackAction_->setAutoRepeat(false);
    frameBackAction_->setEnabled(false);
    shortcut(frameBackAction_);
    connect(frameBackAction_, &QAction::triggered, this, [this] { session_.backFrame(); });
    QAction* achievementsAction = emulationMenu->addAction(Localization::text(QStringLiteral("Достижения…")));
    achievementsAction->setObjectName(QStringLiteral("achievementsAction"));
    connect(achievementsAction, &QAction::triggered, this, &MainWindow::showAchievements);
    regionMenu_ = emulationMenu->addMenu(Localization::text(QStringLiteral("Регион")));
    regionMenu_->setObjectName(QStringLiteral("regionMenu"));
    auto* regionGroup = new QActionGroup(this);
    const QStringList regions{Localization::text(QStringLiteral("Авто")), Localization::text(QStringLiteral("Япония (NTSC-J)")),
        Localization::text(QStringLiteral("США (NTSC-U)")), Localization::text(QStringLiteral("Европа (PAL)"))};
    for (int region = 0; region < regions.size(); ++region) {
        QAction* action = regionMenu_->addAction(regions[region]);
        action->setObjectName(QStringLiteral("region%1").arg(region));
        action->setCheckable(true); action->setChecked(region == region_);
        regionGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, region] {
            releaseInput(); region_ = region;
            settings_.setValue(QStringLiteral("emulation/region"), region);
            session_.setRegion(region);
        });
    }
    fastForwardAction_ = emulationMenu->addAction(Localization::text(QStringLiteral("Ускорение (Tab)")));
    fastForwardAction_->setCheckable(true);
    fastForwardAction_->setEnabled(false);
    connect(fastForwardAction_, &QAction::toggled, this, &MainWindow::updateFastForward);
    rewindAction_ = emulationMenu->addAction(Localization::text(QStringLiteral("Перемотка назад (R)")));
    rewindAction_->setCheckable(true); rewindAction_->setEnabled(false);
    connect(rewindAction_, &QAction::toggled, this, &MainWindow::updateRewind);
    QAction* focusPause = emulationMenu->addAction(Localization::text(QStringLiteral("Пауза при потере фокуса")));
    focusPause->setCheckable(true); focusPause->setChecked(pauseOnFocusLoss_);
    connect(focusPause, &QAction::toggled, this, [this](bool enabled) {
        pauseOnFocusLoss_ = enabled;
        settings_.setValue(QStringLiteral("emulation/pauseOnFocusLoss"), enabled);
        updateInputEnabled();
    });
    emulationMenu->addSeparator();
    saveAction_ = emulationMenu->addAction(Localization::text(QStringLiteral("Сохранить состояние")));
    saveAction_->setShortcut(QKeySequence(QStringLiteral("F5")));
    saveAction_->setAutoRepeat(false);
    saveAction_->setEnabled(false);
    shortcut(saveAction_);
    connect(saveAction_, &QAction::triggered, this, [this] {
        session_.saveState(stateSlot_);
    });
    loadAction_ = emulationMenu->addAction(Localization::text(QStringLiteral("Загрузить состояние")));
    loadAction_->setShortcut(QKeySequence(QStringLiteral("F8")));
    loadAction_->setAutoRepeat(false);
    loadAction_->setEnabled(false);
    shortcut(loadAction_);
    connect(loadAction_, &QAction::triggered, this, [this] {
        releaseInput();
        session_.loadState(stateSlot_);
    });
    saveFileAction_ = emulationMenu->addAction(Localization::text(QStringLiteral("Сохранить в файл…")));
    saveFileAction_->setObjectName(QStringLiteral("saveStateFileAction"));
    saveFileAction_->setEnabled(false);
    connect(saveFileAction_, &QAction::triggered, this, [this] {
        if (!session_.hasGame() || loading_ || moviePending_ || session_.networkClient()) return;
        releaseInput();
        QString stem = currentGameName_.left(80);
        for (qsizetype i = 0; i < stem.size(); ++i)
            if (stem.at(i).unicode() < 32 || QStringLiteral("\\/:*?\"<>|").contains(stem.at(i)))
                stem[i] = QLatin1Char('_');
        if (stem.trimmed().isEmpty()) stem = QStringLiteral("game");
        const QString directory = settings_.value(QStringLiteral("emulation/stateFileDirectory"),
            QFileInfo(currentRom_).absolutePath()).toString();
        QString path = QFileDialog::getSaveFileName(this,
            Localization::text(QStringLiteral("Сохранить в файл")),
            QDir(directory).filePath(stem + QStringLiteral(".krs")),
            Localization::text(QStringLiteral("Сохранения Kega Refusion (*.krs);;Все файлы (*)")));
        view_->setFocus();
        if (path.isEmpty()) return;
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".krs");
        settings_.setValue(QStringLiteral("emulation/stateFileDirectory"), QFileInfo(path).absolutePath());
        session_.saveStateToFile(QFileInfo(path).absoluteFilePath());
    });
    loadFileAction_ = emulationMenu->addAction(Localization::text(QStringLiteral("Загрузить из файла…")));
    loadFileAction_->setObjectName(QStringLiteral("loadStateFileAction"));
    loadFileAction_->setEnabled(false);
    connect(loadFileAction_, &QAction::triggered, this, [this] {
        if (!session_.hasGame() || loading_ || moviePending_ || achievementsHardcore_ || session_.networkClient()) return;
        releaseInput();
        const QString directory = settings_.value(QStringLiteral("emulation/stateFileDirectory"),
            QFileInfo(currentRom_).absolutePath()).toString();
        const QString path = QFileDialog::getOpenFileName(this,
            Localization::text(QStringLiteral("Загрузить из файла")), directory,
            Localization::text(QStringLiteral("Сохранения Kega Refusion (*.krs);;Все файлы (*)")));
        view_->setFocus();
        if (path.isEmpty()) return;
        settings_.setValue(QStringLiteral("emulation/stateFileDirectory"), QFileInfo(path).absolutePath());
        session_.loadStateFromFile(QFileInfo(path).absoluteFilePath());
    });
    QMenu* slotMenu = emulationMenu->addMenu(Localization::text(QStringLiteral("Слот сохранения")));
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
        rewindAction_->setEnabled(rewindEnabled_);
        saveAction_->setEnabled(true);
        loadAction_->setEnabled(true);
        updateMovieActions();
    });

    QMenu* viewMenu = menuBar()->addMenu(Localization::text(QStringLiteral("&Вид")));
    QMenu* aspectMenu = viewMenu->addMenu(Localization::text(QStringLiteral("Соотношение сторон")));
    QActionGroup* aspectGroup = new QActionGroup(this);
    aspectGroup->setExclusive(true);
    struct AspectChoice { QString label; GameView::AspectMode mode; int value; };
    const AspectChoice choices[] = {
        {Localization::text(QStringLiteral("Как на консоли")), GameView::AspectMode::Console, 0},
        {Localization::text(QStringLiteral("Квадратные пиксели")), GameView::AspectMode::SquarePixels, 1},
        {Localization::text(QStringLiteral("Растянуть")), GameView::AspectMode::Stretch, 2},
    };
    const int aspect = settings_.value(QStringLiteral("view/aspect"), 0).toInt();
    for (const auto& choice : choices) {
        QAction* action = aspectMenu->addAction(choice.label);
        action->setCheckable(true);
        action->setChecked(choice.value == aspect);
        aspectGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, choice] {
            view_->setAspectMode(choice.mode);
            settings_.setValue(QStringLiteral("view/aspect"), choice.value);
        });
    }
    QAction* integerAction = viewMenu->addAction(Localization::text(QStringLiteral("Целочисленный масштаб")));
    integerAction->setCheckable(true);
    integerAction->setChecked(settings_.value(QStringLiteral("view/integer"), false).toBool());
    connect(integerAction, &QAction::toggled, this, [this](bool enabled) {
        view_->setIntegerScaling(enabled);
        settings_.setValue(QStringLiteral("view/integer"), enabled);
    });
    QMenu* shaderMenu = viewMenu->addMenu(Localization::text(QStringLiteral("Шейдер")));
    shaderMenu->setObjectName(QStringLiteral("shaderMenu"));
    auto* shaderGroup = new QActionGroup(this);
    shaderOffAction_ = shaderMenu->addAction(Localization::text(QStringLiteral("Выключен")));
    shaderOffAction_->setObjectName(QStringLiteral("shaderOff"));
    shaderOffAction_->setCheckable(true);
    shaderGroup->addAction(shaderOffAction_);
    const std::array<int, 3> blarggFilters{2, 3, 4};
    const std::array<QString, 3> blarggLabels{
        Localization::text(QStringLiteral("Композитный (Blargg)")),
        Localization::text(QStringLiteral("S-Video (Blargg)")),
        Localization::text(QStringLiteral("RGB (Blargg)"))
    };
    for (unsigned index = 0; index < blarggFilters.size(); ++index) {
        const int filter = blarggFilters[index];
        QAction* action = shaderMenu->addAction(blarggLabels[index]);
        action->setObjectName(QStringLiteral("blargg%1").arg(filter));
        action->setCheckable(true);
        shaderGroup->addAction(action);
        blarggActions_[filter] = action;
        connect(action, &QAction::triggered, this, [this, filter] {
            if (!view_->setShaderPreset({})) {
                updateShaderActions();
                return;
            }
            settings_.setValue(QStringLiteral("view/shaderPreset"), QString());
            blarggFilter_ = filter;
            settings_.setValue(QStringLiteral("view/blarggFilter"), filter);
            session_.setBlarggFilter(filter);
            updateShaderActions();
        });
    }
    shaderMenu->addSeparator();
    shaderDitherAction_ = shaderMenu->addAction(Localization::text(QStringLiteral("Сглаживание дизеринга")));
    shaderDitherAction_->setObjectName(QStringLiteral("shaderDither"));
    shaderCustomAction_ = shaderMenu->addAction(Localization::text(QStringLiteral("Пользовательский")));
    shaderCustomAction_->setObjectName(QStringLiteral("shaderCustom"));
    for (QAction* action : {shaderDitherAction_, shaderCustomAction_}) {
        action->setCheckable(true); shaderGroup->addAction(action);
    }
    shaderCustomAction_->setEnabled(false);
    connect(shaderOffAction_, &QAction::triggered, this, [this] { applyShaderPreset({}); });
    connect(shaderDitherAction_, &QAction::triggered, this, [this] { applyShaderPreset(QStringLiteral(":/shaders/refusion-dither.glslp")); });
    shaderMenu->addSeparator();
    QAction* shaderLoad = shaderMenu->addAction(Localization::text(QStringLiteral("Загрузить GLSL-пресет…")));
    connect(shaderLoad, &QAction::triggered, this, [this] {
        releaseInput();
        const QString path = QFileDialog::getOpenFileName(this, Localization::text(QStringLiteral("Загрузить GLSL-пресет")),
            settings_.value(QStringLiteral("files/shaderDirectory"),
                QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("shaders"))).toString(),
            Localization::text(QStringLiteral("GLSL-пресеты (*.glslp);;GLSL-шейдеры (*.glsl)")));
        if (!path.isEmpty() && applyShaderPreset(path))
            settings_.setValue(QStringLiteral("files/shaderDirectory"), QFileInfo(path).absolutePath());
        view_->setFocus();
    });
    shaderParametersAction_ = shaderMenu->addAction(Localization::text(QStringLiteral("Параметры…")));
    shaderParametersAction_->setObjectName(QStringLiteral("shaderParameters"));
    connect(shaderParametersAction_, &QAction::triggered, this, &MainWindow::editShaderParameters);
    updateShaderActions();
    viewMenu->addSeparator();
    osdAction_ = viewMenu->addAction(Localization::text(QStringLiteral("Экранные сообщения (OSD)")));
    osdAction_->setObjectName(QStringLiteral("osdAction"));
    osdAction_->setCheckable(true);
    osdAction_->setChecked(settings_.value(QStringLiteral("view/osd"), true).toBool());
    connect(osdAction_, &QAction::toggled, this, [this](bool enabled) {
        settings_.setValue(QStringLiteral("view/osd"), enabled);
        view_->setOsdEnabled(enabled);
    });
    fpsAction_ = viewMenu->addAction(Localization::text(QStringLiteral("FPS эмуляции")));
    fpsAction_->setObjectName(QStringLiteral("emulationFpsAction"));
    fpsAction_->setCheckable(true);
    fpsAction_->setChecked(settings_.value(QStringLiteral("view/emulationFps"), false).toBool());
    connect(fpsAction_, &QAction::toggled, this, [this](bool enabled) {
        settings_.setValue(QStringLiteral("view/emulationFps"), enabled);
        session_.setPerformanceMetricsEnabled(enabled);
        view_->setPerformanceMetricsEnabled(enabled);
    });
    session_.setPerformanceMetricsEnabled(fpsAction_->isChecked());
    view_->setPerformanceMetricsEnabled(fpsAction_->isChecked());
    gameFpsAction_ = viewMenu->addAction(Localization::text(QStringLiteral("FPS игры (оценка по опросу управления)")));
    gameFpsAction_->setObjectName(QStringLiteral("gameFpsAction"));
    gameFpsAction_->setCheckable(true);
    gameFpsAction_->setChecked(settings_.value(QStringLiteral("view/gameFps"), false).toBool());
    connect(gameFpsAction_, &QAction::toggled, this, [this](bool enabled) {
        settings_.setValue(QStringLiteral("view/gameFps"), enabled);
        session_.setGameFpsEnabled(enabled);
        view_->setGameFpsEnabled(enabled);
    });
    session_.setGameFpsEnabled(gameFpsAction_->isChecked());
    view_->setGameFpsEnabled(gameFpsAction_->isChecked());
    viewMenu->addSeparator();
    fullscreenAction_ = viewMenu->addAction(Localization::text(QStringLiteral("Полный экран")));
    fullscreenAction_->setShortcut(QKeySequence(QStringLiteral("Alt+Return")));
    fullscreenAction_->setAutoRepeat(false);
    fullscreenAction_->setCheckable(true);
    shortcut(fullscreenAction_);
    connect(fullscreenAction_, &QAction::toggled, this, &MainWindow::setFullscreenMode);

    QMenu* soundMenu = menuBar()->addMenu(Localization::text(QStringLiteral("&Звук")));
    muteAction_ = soundMenu->addAction(Localization::text(QStringLiteral("Без звука")));
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
    smsFmAction_ = soundMenu->addAction(Localization::text(QStringLiteral("FM-синтезатор (Master System)")));
    smsFmAction_->setObjectName(QStringLiteral("smsFmAction"));
    smsFmAction_->setCheckable(true);
    smsFmAction_->setChecked(smsFmEnabled_);
    smsFmAction_->setVisible(false);
    smsFmAction_->setToolTip(Localization::text(QStringLiteral(
        "Применяется при следующей загрузке игры или перезапуске питания.")));
    connect(smsFmAction_, &QAction::toggled, this, &MainWindow::setSmsFmEnabled);
    QWidget* volumePanel = new QWidget(soundMenu);
    QVBoxLayout* volumeLayout = new QVBoxLayout(volumePanel);
    volumeLayout->setContentsMargins(12, 6, 12, 8);
    volumeLayout->addWidget(new QLabel(Localization::text(QStringLiteral("Громкость")), volumePanel));
    volumeSlider_ = new QSlider(Qt::Horizontal, volumePanel);
    volumeSlider_->setObjectName(QStringLiteral("masterVolumeSlider"));
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setMinimumWidth(160);
    volumeSlider_->setValue(qBound(0, settings_.value(QStringLiteral("audio/volume"), 80).toInt(), 100));
    savedVolume_ = volumeSlider_->value();
    audio_->setVolume(savedVolume_);
    volumeLayout->addWidget(volumeSlider_);
    QWidgetAction* volumeAction = new QWidgetAction(soundMenu);
    volumeAction->setDefaultWidget(volumePanel);
    soundMenu->addAction(volumeAction);
    volumeSaveTimer_ = new QTimer(this);
    volumeSaveTimer_->setSingleShot(true);
    volumeSaveTimer_->setInterval(400);
    connect(volumeSaveTimer_, &QTimer::timeout, this, &MainWindow::flushVolumePreference);
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
        audio_->setVolume(value);
        view_->showOsd(Localization::text(QStringLiteral("Громкость: %1%"))
                           .arg(value), 1200, QStringLiteral("volume"));
        volumeSaveTimer_->start();
    });
    QMenu* onlineMenu = menuBar()->addMenu(Localization::text(QStringLiteral("&Сетевая игра")));
    onlineMenu->setObjectName(QStringLiteral("onlineMenu"));
    QAction* createSessionAction = onlineMenu->addAction(Localization::text(QStringLiteral("Создать сеанс…")));
    createSessionAction->setObjectName(QStringLiteral("netplayCreateAction"));
    connect(createSessionAction, &QAction::triggered, netplay_, &NetplayController::createSession);
    QAction* joinSessionAction = onlineMenu->addAction(Localization::text(QStringLiteral("Присоединиться…")));
    joinSessionAction->setObjectName(QStringLiteral("netplayJoinAction"));
    connect(joinSessionAction, &QAction::triggered, netplay_, &NetplayController::joinSession);
    createSessionAction->setEnabled(!netplay_->active());
    joinSessionAction->setEnabled(!netplay_->active());
    connect(netplay_, &NetplayController::activeChanged, this,
            [createSessionAction, joinSessionAction](bool active) {
        createSessionAction->setEnabled(!active);
        joinSessionAction->setEnabled(!active);
    });
    QAction* showLobbyAction = onlineMenu->addAction(Localization::text(QStringLiteral("Лобби")));
    showLobbyAction->setObjectName(QStringLiteral("netplayLobbyAction"));
    connect(showLobbyAction, &QAction::triggered, netplay_, &NetplayController::showLobby);
    onlineMenu->addSeparator();
    netplayLeaveAction_ = onlineMenu->addAction(Localization::text(QStringLiteral("Выйти из сеанса")));
    netplayLeaveAction_->setObjectName(QStringLiteral("netplayLeaveAction"));
    netplayLeaveAction_->setEnabled(netplay_->active());
    connect(netplayLeaveAction_, &QAction::triggered, netplay_, &NetplayController::leaveSession);
    auto* cheatsMenu = menuBar()->addMenu(Localization::text(QStringLiteral("Читы")));
    cheatsMenu->setObjectName(QStringLiteral("cheatsMenu"));
    cheatsAction_ = cheatsMenu->addAction(Localization::text(QStringLiteral("Коды / Game Genie…")));
    cheatsAction_->setEnabled(false);
    connect(cheatsAction_, &QAction::triggered, this, &MainWindow::editCheats);
    cheatSearchAction_ = cheatsMenu->addAction(Localization::text(QStringLiteral("Поиск читов…")));
    cheatSearchAction_->setEnabled(false);
    connect(cheatSearchAction_, &QAction::triggered, this, &MainWindow::showCheatSearch);
    connect(cheatsAction_, &QAction::changed, this, [this] {
        const bool enabled=cheatsAction_->isEnabled();
        cheatSearchAction_->setEnabled(enabled);
        if (!enabled && cheatSearchDialog_) cheatSearchDialog_->close();
    });
    QMenu* settingsMenu = menuBar()->addMenu(Localization::text(QStringLiteral("&Настройки")));
    QAction* emulatorSettingsAction = settingsMenu->addAction(Localization::text(QStringLiteral("Настройки эмулятора…")));
    emulatorSettingsAction->setObjectName(QStringLiteral("emulatorSettingsAction"));
    connect(emulatorSettingsAction, &QAction::triggered, this, [this] { editSettings(); });
    QAction* controlsAction = settingsMenu->addAction(Localization::text(QStringLiteral("Управление…")));
    connect(controlsAction, &QAction::triggered, this, &MainWindow::editControls);
    debugMenu_ = menuBar()->addMenu(Localization::text(QStringLiteral("&Отладка")));
    debugMenu_->setObjectName(QStringLiteral("debugMenu"));
    debugMenu_->menuAction()->setVisible(false);
    debugGraphicsAction_ = debugMenu_->addAction(Localization::text(QStringLiteral("Графика…")));
    debugGraphicsAction_->setObjectName(QStringLiteral("debugGraphicsAction"));
    connect(debugGraphicsAction_, &QAction::triggered, this, &MainWindow::showDebugGraphics);
    debugAudioAction_ = debugMenu_->addAction(Localization::text(QStringLiteral("Звуковые каналы…")));
    debugAudioAction_->setObjectName(QStringLiteral("debugAudioAction"));
    connect(debugAudioAction_, &QAction::triggered, this, &MainWindow::showDebugAudio);
    exportBlockedMenus_ = {emulationMenu, onlineMenu, settingsMenu, debugMenu_};
    updateVideoActions();
    QMenu* helpMenu = menuBar()->addMenu(Localization::text(QStringLiteral("&Справка")));
    auto* updatesAction = helpMenu->addAction(Localization::text(QStringLiteral("Проверить обновления…")));
    updatesAction->setObjectName(QStringLiteral("checkUpdatesAction"));
    connect(updatesAction, &QAction::triggered, this, [this] { checkForUpdates(); });
    helpMenu->addSeparator();
    QAction* aboutAction = helpMenu->addAction(Localization::text(QStringLiteral("О программе")));
    aboutAction->setObjectName(QStringLiteral("aboutAction"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox about(QMessageBox::Information, Localization::text(QStringLiteral("О Kega Refusion")),
            Localization::text(QStringLiteral("<b>Kega Refusion</b><br>Компактный эмулятор на базе Genesis Plus GX (Libretro)."
            "<br><br>Игры: MD, GEN, BIN, SMD, SMS, GG, SG, 68K и Sega CD (CUE, ISO, CHD, M3U). "
            "Поддерживаются архивы ZIP и 7z. Поддержка 32X — экспериментальная.")) +
            QStringLiteral("<br><br>") + Localization::text(QStringLiteral("Версия: %1"))
                .arg(QApplication::applicationVersion().toHtmlEscaped()) +
            QStringLiteral("<br><br>") + Localization::text(QStringLiteral("Авторы:")) +
            QStringLiteral("<br>vova791 — Салата Владимир<br>Codex ChatGPT<br><br>") +
            Localization::text(QStringLiteral("Оригинальный код: GPLv3+ с исключением для ядра. Без гарантий.")) +
            QStringLiteral("<br><a href=\"https://github.com/citrongames/Kega-Refusion/blob/master/LICENSING.md\">") +
            Localization::text(QStringLiteral("Лицензии и условия распространения")) +
            QStringLiteral("</a>"), QMessageBox::Ok, this);
        about.setObjectName(QStringLiteral("aboutDialog"));
        about.setIconPixmap(windowIcon().pixmap(64,64));
        about.setTextFormat(Qt::RichText);
        about.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                             Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
        about.exec();
    });
}

void MainWindow::checkForUpdates(bool interactive) {
    if (!interactive && !settings_.value(QStringLiteral("updates/checkOnStartup"), true).toBool()) return;
    if (installingUpdate_ || restartingUpdate_) {
        if (interactive) showUpdateDialog();
        return;
    }
    if (checkingForUpdates_) {
        if (interactive) {
            updateCheckInteractive_ = true;
            showUpdateDialog();
        }
        return;
    }
    updateCheckInteractive_ = interactive;
    checkingForUpdates_ = true;
    availableUpdateVersion_.clear();
    availableUpdateNotes_.clear();
    updateError_.clear();
    if (interactive) showUpdateDialog();
    else refreshUpdateDialog();
    updateService_->check();
}

void MainWindow::showUpdateDialog(bool activate) {
    if (!updateDialog_) {
        updateDialog_ = new UpdateDialog(this);
        updateDialog_->setCheckOnStartup(settings_.value(QStringLiteral("updates/checkOnStartup"), true).toBool());
        connect(updateDialog_, &UpdateDialog::checkOnStartupChanged, this, [this](bool enabled) {
            if (settings_.value(QStringLiteral("updates/checkOnStartup"), true).toBool() != enabled)
                settings_.setValue(QStringLiteral("updates/checkOnStartup"), enabled);
        });
        connect(updateDialog_, &UpdateDialog::checkRequested, this, [this] { checkForUpdates(); });
        connect(updateDialog_, &UpdateDialog::installRequested, this, [this] {
            if (checkingForUpdates_ || installingUpdate_ || restartingUpdate_ || availableUpdateVersion_.isEmpty()) return;
            updateCheckInteractive_ = true;
            installingUpdate_ = true;
            updateError_.clear();
            updateBytesReceived_ = updateBytesTotal_ = 0;
            refreshUpdateDialog();
            updateService_->install();
        });
    }
    refreshUpdateDialog();
    updateDialog_->setAttribute(Qt::WA_ShowWithoutActivating, !activate);
    updateDialog_->show();
    if (activate) {
        updateDialog_->raise();
        updateDialog_->activateWindow();
    }
}

void MainWindow::refreshUpdateDialog() {
    if (!updateDialog_) return;
    if (checkingForUpdates_) {
        updateDialog_->setChecking();
        return;
    }
    if (!availableUpdateVersion_.isEmpty())
        updateDialog_->setRelease(availableUpdateVersion_, availableUpdateNotes_);
    else updateDialog_->setNoUpdate();
    if (restartingUpdate_) updateDialog_->setRestarting();
    else if (installingUpdate_) updateDialog_->setProgress(updateBytesReceived_, updateBytesTotal_);
    else if (!updateError_.isEmpty()) updateDialog_->setError(updateError_, !availableUpdateVersion_.isEmpty());
}

void MainWindow::applyDebugMasks() {
    debugVideoDisabled_ &= debugVideoAvailable_;
    debugAudioDisabled_ &= debugAudioAvailable_;
    if (achievementsHardcore_) debugVideoDisabled_ = 0;
    updateDebugDialogs();
    if (!loading_ && (debugVideoAvailable_ || debugAudioAvailable_))
        session_.setDebugMasks(debugVideoDisabled_, debugAudioDisabled_);
}

void MainWindow::updateDebugDialogs() {
    if (debugMenu_) debugMenu_->menuAction()->setVisible(
        debugVideoAvailable_ || debugAudioAvailable_);
    if (debugGraphicsAction_) {
        debugGraphicsAction_->setVisible(debugVideoAvailable_ != 0);
        debugGraphicsAction_->setEnabled(!loading_ && !videoExporting_ && !achievementsHardcore_);
    }
    if (debugAudioAction_) {
        debugAudioAction_->setVisible(debugAudioAvailable_ != 0);
        debugAudioAction_->setEnabled(!loading_ && !videoExporting_);
    }
    if (debugGraphicsDialog_)
        debugGraphicsDialog_->setControls(debugVideoAvailable_, debugVideoDisabled_,
                                          achievementsHardcore_, !loading_ && !videoExporting_, debugBackdropColor_);
    if (debugAudioDialog_)
        debugAudioDialog_->setControls(debugAudioAvailable_, debugAudioDisabled_,
                                       !loading_ && !videoExporting_, debugAudioGains_);
}

void MainWindow::showDebugGraphics() {
    if (!debugGraphicsDialog_) {
        debugGraphicsDialog_ = new DebugGraphicsDialog(this);
        connect(debugGraphicsDialog_, &DebugGraphicsDialog::maskChanged, this,
                [this](quint32 disabled) {
            if (loading_) return;
            debugVideoDisabled_ = disabled;
            applyDebugMasks();
        });
        connect(debugGraphicsDialog_, &DebugGraphicsDialog::backdropChanged, this,
                [this](quint32 rgb) {
            if (loading_) return;
            debugBackdropColor_ = QColor::fromRgb(rgb);
            session_.setDebugBackdrop(rgb);
            updateDebugDialogs();
        });
    }
    updateDebugDialogs();
    debugGraphicsDialog_->show();
    debugGraphicsDialog_->raise();
    debugGraphicsDialog_->activateWindow();
}

void MainWindow::showDebugAudio() {
    if (!debugAudioDialog_) {
        debugAudioDialog_ = new DebugAudioDialog(this);
        connect(debugAudioDialog_, &DebugAudioDialog::maskChanged, this,
                [this](quint64 disabled) {
            if (loading_) return;
            debugAudioDisabled_ = disabled;
            applyDebugMasks();
        });
        connect(debugAudioDialog_, &DebugAudioDialog::gainChanged, this,
                [this](unsigned channel, unsigned percent) {
            if (loading_ || channel >= debugAudioGains_.size() ||
                !(debugAudioAvailable_ & (quint64(1) << channel))) return;
            debugAudioGains_[channel] = percent;
            session_.setDebugAudioGain(channel, percent);
        });
    }
    updateDebugDialogs();
    debugAudioDialog_->show();
    debugAudioDialog_->raise();
    debugAudioDialog_->activateWindow();
}

bool MainWindow::applyShaderPreset(const QString& path) {
    QString error;
    if (!view_->setShaderPreset(path, &error)) {
        updateShaderActions();
        if (!smokeTest_) QMessageBox::warning(this, Localization::text(QStringLiteral("Шейдер")), error);
        return false;
    }
    settings_.setValue(QStringLiteral("view/shaderPreset"), path);
    if (blarggFilter_) {
        blarggFilter_ = 0;
        settings_.setValue(QStringLiteral("view/blarggFilter"), 0);
        session_.setBlarggFilter(0);
    }
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
    if (blarggFilter_ >= 2 && blarggFilter_ <= 4)
        blarggActions_[blarggFilter_]->setChecked(true);
    else if (path.isEmpty()) shaderOffAction_->setChecked(true);
    else if (path == QLatin1String(":/shaders/refusion-dither.glslp"))
        shaderDitherAction_->setChecked(true);
    else shaderCustomAction_->setChecked(true);
    shaderParametersAction_->setEnabled(!view_->shaderParameters().isEmpty());
}

void MainWindow::editShaderParameters() {
    const auto parameters = view_->shaderParameters();
    if (parameters.isEmpty()) return;
    releaseInput();
    QDialog dialog(this, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    dialog.setObjectName(QStringLiteral("shaderParametersDialog"));
    dialog.setWindowTitle(Localization::text(QStringLiteral("Параметры шейдера")));
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
        form->addRow(Localization::text(parameter.label), value);
        connect(value, &QDoubleSpinBox::valueChanged, &dialog, [this, prefix, id = parameter.id](double selected) {
            if (view_->setShaderParameter(id, float(selected))) settings_.setValue(prefix + id, selected);
        });
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(Localization::text(QStringLiteral("Закрыть")));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec(); view_->setFocus();
}

void MainWindow::updateDiscMenu() {
    if (!discMenu_) return;
    const bool hasDisc = !discPaths_.isEmpty();
    const bool discCommandsEnabled = hasDisc && movieMode_ == 0 && !moviePending_ && !videoExporting_;
    changeDiscAction_->setEnabled(discCommandsEnabled);
    ejectDiscAction_->setEnabled(discCommandsEnabled);
    ejectDiscAction_->setChecked(hasDisc && discEjected_);
    discMenu_->clear();
    discMenu_->setEnabled(discPaths_.size() > 1 && movieMode_ == 0 && !moviePending_ && !videoExporting_);
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
    recentMenu_->setEnabled(!existing.isEmpty() && !videoExporting_);
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
        view_->showOsd(Localization::text(QStringLiteral("Не удалось сохранить скриншот: %1")).arg(error), 4000,
                       QStringLiteral("screenshot"));
        qWarning("Screenshot failed: %s", qPrintable(error));
    };
    if (image.isNull()) {
        failed(Localization::text(QStringLiteral("кадр игры ещё не готов")));
        return;
    }
    const QDir directory(QDir(dataPath_).filePath(QStringLiteral("screenshots")));
    if (!QDir().mkpath(directory.absolutePath())) {
        failed(Localization::text(QStringLiteral("не удалось создать папку %1")).arg(QDir::toNativeSeparators(directory.absolutePath())));
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
        view_->showOsd(Localization::text(QStringLiteral("Скриншот сохранён: %1")).arg(filename), 2600,
                       QStringLiteral("screenshot"));
        qInfo("Screenshot saved: %s", qPrintable(QDir::toNativeSeparators(file.fileName())));
        return;
    }
    failed(Localization::text(QStringLiteral("не удалось подобрать свободное имя файла")));
}

void MainWindow::flushVolumePreference() {
    if (!volumeSaveTimer_ || !volumeSlider_) return;
    volumeSaveTimer_->stop();
    const int value = volumeSlider_->value();
    if (value == savedVolume_) return;
    settings_.setValue(QStringLiteral("audio/volume"), value);
    savedVolume_ = value;
}

void MainWindow::setLoadingUi(bool loading, const QString& fileName) {
    loading_ = loading;
    updateDebugDialogs();
    if (cheatsAction_) cheatsAction_->setEnabled(session_.hasGame() && cheatsSupported_ && !loading &&
                                                  movieMode_ == 0 && !videoExporting_ && !achievementsHardcore_);
    updateMovieActions();
    updateFrameActions();
    if (loading) view_->setLightgunInputEnabled(false);
    view_->setLoading(loading, fileName);
    if (screenshotAction_) screenshotAction_->setEnabled(session_.hasGame() && !loading);
}

void MainWindow::openGameDialog() {
    if (loading_ || videoExporting_) return;
    releaseInput();
    const QString path = QFileDialog::getOpenFileName(
        this, Localization::text(QStringLiteral("Открыть игру")),
        settings_.value(QStringLiteral("files/lastDirectory"), QDir::homePath()).toString(),
        Localization::text(QStringLiteral("Игры Sega (*.md *.gen *.bin *.smd *.sms *.gg *.sg *.68k *.32x *.cue *.iso *.chd *.m3u *.zip *.7z);;Диски Sega CD (*.cue *.iso *.chd *.m3u);;Архивы (*.zip *.7z);;Все файлы (*)")));
    if (!path.isEmpty()) openRom(path);
    else view_->setFocus();
}

void MainWindow::openRom(const QString& path) {
    if (loading_ || videoExporting_) return;
    const QFileInfo file(path);
    if (!file.exists() || !file.isFile()) {
        const QString text = Localization::text(QStringLiteral("Файл не найден: %1")).arg(path);
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
                error.isEmpty() ? Localization::text(QStringLiteral("В архиве нет поддерживаемых игр.")) : error);
            // Let the session report the same failure to the smoke-test observer.
            if (smokeTest_) session_.loadGame(path);
            else { view_->setFocus(); updateInputEnabled(); }
            return;
        }
        if (entries.size() == 1) archiveEntry = entries.first();
        else if (!smokeTest_) {
            QDialog dialog(this, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
            dialog.setWindowTitle(Localization::text(QStringLiteral("Выбор игры из архива")));
            dialog.resize(620, 420);
            dialog.setMinimumSize(380, 260);
            auto* layout = new QVBoxLayout(&dialog);
            auto* label = new QLabel(Localization::text(QStringLiteral("Выберите игру для загрузки:")), &dialog);
            layout->addWidget(label);
            auto* list = new QListWidget(&dialog);
            list->setObjectName(QStringLiteral("archiveGameList"));
            list->setAccessibleName(Localization::text(QStringLiteral("Игры в архиве")));
            list->setSelectionMode(QAbstractItemView::SingleSelection);
            list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
            list->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            list->setTextElideMode(Qt::ElideNone);
            for (const auto& entry : entries) {
                auto* item = new QListWidgetItem(entry.name, list);
                item->setData(Qt::UserRole, entry.index);
                item->setToolTip(Localization::text(QStringLiteral("%1\n%2 КиБ")).arg(entry.name).arg((entry.size+1023)/1024));
            }
            list->setCurrentRow(0);
            layout->addWidget(list, 1);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            auto* loadButton = buttons->button(QDialogButtonBox::Ok);
            loadButton->setText(Localization::text(QStringLiteral("Загрузить")));
            loadButton->setDefault(true);
            buttons->button(QDialogButtonBox::Cancel)->setText(Localization::text(QStringLiteral("Отмена")));
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

void MainWindow::editSettings(int initialPage) {
    if (videoExporting_) return;
    releaseInput();
    input_->setEnabled(false);
    QDialog dialog(this, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    dialog.setObjectName(QStringLiteral("emulatorSettingsDialog"));
    dialog.setWindowTitle(Localization::text(QStringLiteral("Настройки эмулятора — Kega Refusion")));
    const QSize workArea = dialog.screen()->availableGeometry().size();
    const QSize target = QSize(1100, 720).boundedTo(workArea - QSize(48, 72));
    dialog.setMinimumSize(QSize(800, 560).boundedTo(target));
    dialog.resize(target);
    dialog.setSizeGripEnabled(true);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(14);
    auto* content = new QHBoxLayout;
    content->setSpacing(18);
    auto* navigation = new QListWidget(&dialog);
    navigation->setObjectName(QStringLiteral("settingsNavigation"));
    navigation->setFixedWidth(236);
    navigation->setIconSize(QSize(24, 24));
    navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation->setStyleSheet(QStringLiteral(
        "QListWidget { outline: none; padding: 4px; } "
        "QListWidget::item { min-height: 44px; padding: 2px 12px; border-radius: 5px; } "
        "QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }"));
    navigation->setSpacing(4);
    navigation->addItems({Localization::text(QStringLiteral("Общие")), Localization::text(QStringLiteral("Игры")),
                          Localization::text(QStringLiteral("Перемотка")), Localization::text(QStringLiteral("Запись")),
                          Localization::text(QStringLiteral("Достижения")), Localization::text(QStringLiteral("Звук")), QStringLiteral("BIOS"),
                          Localization::text(QStringLiteral("Управление")),
                          Localization::text(QStringLiteral("Сетевая игра"))});
    for (int row = 0; row < navigation->count(); ++row)
        navigation->item(row)->setIcon(settingsSectionIcon(row, dialog.palette()));
    auto* pages = new QStackedWidget(&dialog);
    pages->setObjectName(QStringLiteral("settingsPages"));
    content->addWidget(navigation);
    content->addWidget(pages, 1);
    layout->addLayout(content, 1);

    const auto addPage = [pages](const QString& title, const QString& description) {
        auto* page = new QWidget(pages);
        auto* pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(14, 8, 14, 8);
        pageLayout->setSpacing(14);
        auto* heading = new QLabel(title, page);
        auto headingFont = heading->font();
        headingFont.setPointSize(headingFont.pointSize() + 3);
        headingFont.setBold(true);
        heading->setFont(headingFont);
        pageLayout->addWidget(heading);
        auto* summary = new QLabel(description, page);
        summary->setWordWrap(true);
        pageLayout->addWidget(summary);
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        form->setHorizontalSpacing(20);
        form->setVerticalSpacing(14);
        pageLayout->addLayout(form);
        pageLayout->addStretch();
        auto* scroll = new QScrollArea(pages);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        pages->addWidget(scroll);
        return form;
    };

    auto* general = addPage(Localization::text(QStringLiteral("Общие")),
        Localization::text(QStringLiteral("Изображение и скорость работы эмулятора.")));
    auto* language = new QComboBox(&dialog);
    language->setObjectName(QStringLiteral("interfaceLanguage"));
    language->addItem(Localization::text(QStringLiteral("Как в системе")), QStringLiteral("system"));
    for (const auto& entry : Localization::availableLanguages()) language->addItem(entry.name, entry.code);
    const QString selectedLanguage = Localization::selectedLanguage();
    const int languageIndex = language->findData(selectedLanguage);
    language->setCurrentIndex(languageIndex >= 0 ? languageIndex : 0);
    general->addRow(Localization::text(QStringLiteral("Язык интерфейса:")), language);
    auto* languageHelp = new QLabel(Localization::text(QStringLiteral(
        "Изменение языка применяется после перезапуска. Если перевода нет, используется английский.")), &dialog);
    languageHelp->setWordWrap(true);
    general->addRow(languageHelp);
    const bool checkUpdatesOnStartup = settings_.value(QStringLiteral("updates/checkOnStartup"), true).toBool();
    auto* startupUpdates = new QCheckBox(Localization::text(QStringLiteral("Проверять обновления при запуске")), &dialog);
    startupUpdates->setObjectName(QStringLiteral("checkUpdatesOnStartup"));
    startupUpdates->setChecked(checkUpdatesOnStartup);
    general->addRow(startupUpdates);
    auto* emulationFps = new QCheckBox(Localization::text(QStringLiteral("Показывать FPS эмуляции")), &dialog);
    emulationFps->setObjectName(QStringLiteral("emulationFps"));
    emulationFps->setChecked(fpsAction_->isChecked());
    general->addRow(emulationFps);
    auto* gameFps = new QCheckBox(Localization::text(QStringLiteral("Показывать FPS игры — оценку по опросу управления")), &dialog);
    gameFps->setObjectName(QStringLiteral("gameFps"));
    gameFps->setChecked(gameFpsAction_->isChecked());
    general->addRow(gameFps);
    auto* fpsHelp = new QLabel(Localization::text(QStringLiteral(
        "Число без префикса — FPS эмуляции, с ~ — оценка FPS игры. Оценка учитывает кадры, в которых игра читает управление; "
        "частота игровой логики может отличаться. Оценка приведена к нормальной скорости игры. Счётчики включаются независимо от OSD.")), &dialog);
    fpsHelp->setWordWrap(true);
    general->addRow(fpsHelp);
    const bool jpeg = settings_.value(QStringLiteral("screenshot/format"), QStringLiteral("png"))
                          .toString() == QLatin1String("jpeg");
    auto* screenshotFormat = new QComboBox(&dialog);
    screenshotFormat->setObjectName(QStringLiteral("screenshotFormat"));
    screenshotFormat->addItem(Localization::text(QStringLiteral("PNG (без потерь)")), QStringLiteral("png"));
    screenshotFormat->addItem(QStringLiteral("JPEG"), QStringLiteral("jpeg"));
    screenshotFormat->setCurrentIndex(jpeg ? 1 : 0);
    general->addRow(Localization::text(QStringLiteral("Формат скриншотов:")), screenshotFormat);
    const bool osdEnabled = osdAction_->isChecked();
    auto* showOsd = new QCheckBox(Localization::text(QStringLiteral("Показывать экранные сообщения (OSD)")), &dialog);
    showOsd->setObjectName(QStringLiteral("osdEnabled"));
    showOsd->setChecked(osdEnabled);
    general->addRow(showOsd);
    auto* speed = new QComboBox(&dialog);
    speed->setObjectName(QStringLiteral("fastForwardFactor"));
    for (int factor : {2, 3, 4, 6, 8, 12, 16, 32, 0})
        speed->addItem(factor ? QStringLiteral("×%1").arg(factor)
                              : Localization::text(QStringLiteral("Без ограничения")), factor);
    speed->setCurrentIndex(speed->findData(fastForwardFactor_));
    speed->setToolTip(Localization::text(QStringLiteral("Скорость при удержании Tab или назначенной кнопки ускорения.")));
    general->addRow(Localization::text(QStringLiteral("Скорость ускорения:")), speed);

    auto* games = addPage(Localization::text(QStringLiteral("Игры")),
        Localization::text(QStringLiteral("Совместимость картриджей и загрузка Sega CD.")));
    const bool attachCd = settings_.value(QStringLiteral("emulation/attachCd"), false).toBool();
    const bool attach32x = settings_.value(QStringLiteral("emulation/attach32x"), false).toBool();
    auto* cdAttachment = new QComboBox(&dialog);
    cdAttachment->setObjectName(QStringLiteral("cdAttachment"));
    auto* x32Attachment = new QComboBox(&dialog);
    x32Attachment->setObjectName(QStringLiteral("x32Attachment"));
    for (auto* combo : {cdAttachment, x32Attachment}) {
        combo->addItem(Localization::text(QStringLiteral("Автоматически")), false);
        combo->addItem(Localization::text(QStringLiteral("Подключено")), true);
    }
    cdAttachment->setCurrentIndex(attachCd ? 1 : 0);
    x32Attachment->setCurrentIndex(attach32x ? 1 : 0);
    games->addRow(Localization::text(QStringLiteral("Sega CD для обычных игр:")), cdAttachment);
    games->addRow(Localization::text(QStringLiteral("32X для обычных игр:")), x32Attachment);
    auto* attachmentHelp = new QLabel(Localization::text(QStringLiteral(
        "Игры Sega CD и 32X подключают нужное устройство автоматически. «Подключено» оставляет устройство "
        "и для обычных игр Mega Drive. Нужны BIOS. Изменения применятся при следующей загрузке или перезапуске питания.")), &dialog);
    attachmentHelp->setWordWrap(true);
    games->addRow(attachmentHelp);
    const bool fixChecksum = settings_.value(QStringLiteral("emulation/autoFixChecksum"), false).toBool();
    auto* checksum = new QCheckBox(Localization::text(QStringLiteral("Автоисправлять контрольную сумму ROM")), &dialog);
    checksum->setObjectName(QStringLiteral("autoFixChecksum"));
    checksum->setChecked(fixChecksum);
    games->addRow(checksum);
    auto* checksumHelp = new QLabel(Localization::text(QStringLiteral(
        "Исправляется только копия ROM в памяти. Настройка применится при следующей "
        "загрузке игры или перезапуске питания.")), &dialog);
    checksumHelp->setWordWrap(true);
    games->addRow(checksumHelp);
    const bool fastCd = settings_.value(QStringLiteral("emulation/fastCdLoading"), false).toBool();
    auto* cdLoading = new QComboBox(&dialog);
    cdLoading->setObjectName(QStringLiteral("cdLoadingMode"));
    cdLoading->addItem(Localization::text(QStringLiteral("Обычная (как на консоли)")), false);
    cdLoading->addItem(Localization::text(QStringLiteral("Ускоренная")), true);
    cdLoading->setCurrentIndex(fastCd ? 1 : 0);
    games->addRow(Localization::text(QStringLiteral("Загрузка CD:")), cdLoading);
    auto* cdHelp = new QLabel(Localization::text(QStringLiteral(
        "Ускоренный режим убирает задержки поиска дорожек. "
        "Если игра зависает или звук рассинхронизируется, верните обычный режим.")), &dialog);
    cdHelp->setWordWrap(true);
    games->addRow(cdHelp);
    const bool noSpriteLimit = settings_.value(QStringLiteral("emulation/removeSpriteLimit"), false).toBool();
    auto* spriteLimit = new QComboBox(&dialog);
    spriteLimit->setObjectName(QStringLiteral("spriteLimitMode"));
    spriteLimit->addItem(Localization::text(QStringLiteral("Как на консоли")), false);
    spriteLimit->addItem(Localization::text(QStringLiteral("Снять ограничение")), true);
    spriteLimit->setCurrentIndex(noSpriteLimit ? 1 : 0);
    spriteLimit->setToolTip(Localization::text(QStringLiteral(
        "Уменьшает мигание спрайтов; в отдельных играх может нарушать графические эффекты.")));
    games->addRow(Localization::text(QStringLiteral("Лимит спрайтов:")), spriteLimit);
    const bool movieActive = movieMode_ != 0;
    cdAttachment->setEnabled(!movieActive && !achievementsHardcore_);
    x32Attachment->setEnabled(!movieActive && !achievementsHardcore_);
    checksum->setEnabled(!movieActive && !achievementsHardcore_);
    cdLoading->setEnabled(!movieActive && !achievementsHardcore_);
    spriteLimit->setEnabled(!movieActive && !achievementsHardcore_);
    connect(&session_, &EmulationSession::achievementsHardcoreChanged, &dialog,
            [this, checksum, cdLoading, spriteLimit, cdAttachment, x32Attachment](bool active) {
        const bool editable = movieMode_ == 0 && !active;
        cdAttachment->setEnabled(editable);
        x32Attachment->setEnabled(editable);
        checksum->setEnabled(editable);
        cdLoading->setEnabled(editable);
        spriteLimit->setEnabled(editable);
    });

    auto* rewind = addPage(Localization::text(QStringLiteral("Перемотка")),
        Localization::text(QStringLiteral("Запись истории игры для возврата к предыдущим кадрам.")));
    auto* recordHistory = new QCheckBox(Localization::text(QStringLiteral("Записывать историю перемотки")), &dialog);
    recordHistory->setObjectName(QStringLiteral("rewindHistoryEnabled"));
    recordHistory->setChecked(rewindEnabled_);
    rewind->addRow(recordHistory);
    const int memoryMiB = qBound(16, settings_.value(QStringLiteral("emulation/rewindMemoryMiB"), 128).toInt(), 1024);
    auto* rewindMemory = new QSpinBox(&dialog);
    rewindMemory->setObjectName(QStringLiteral("rewindMemoryLimit"));
    rewindMemory->setRange(16, 1024);
    rewindMemory->setSingleStep(16);
    rewindMemory->setSuffix(Localization::text(QStringLiteral(" МиБ")));
    rewindMemory->setValue(memoryMiB);
    rewind->addRow(Localization::text(QStringLiteral("Память перемотки:")), rewindMemory);
    auto* rewindHelp = new QLabel(Localization::text(QStringLiteral(
        "История ограничена только памятью. При заполнении удаляются старые кадры. "
        "Изменение лимита очищает историю; память выделяется по мере накопления.")), &dialog);
    rewindHelp->setWordWrap(true);
    rewind->addRow(rewindHelp);
    rewindMemory->setToolTip(Localization::text(QStringLiteral(
        "Лимит сохранённой истории. Рабочие буферы эмулятора учитываются отдельно.")));
    // The history memory limit remains adjustable while a recording is active.

    auto* recording = addPage(Localization::text(QStringLiteral("Запись управления")),
        Localization::text(QStringLiteral("Формат счётчика записи и воспроизведения на экране игры.")));
    const QString counterUnit = settings_.value(QStringLiteral("recording/counterUnit"),
        QStringLiteral("time")).toString();
    auto* recordingCounterUnit = new QComboBox(&dialog);
    recordingCounterUnit->setObjectName(QStringLiteral("recordingCounterUnit"));
    recordingCounterUnit->addItem(Localization::text(QStringLiteral("Время (чч:мм:сс)")), QStringLiteral("time"));
    recordingCounterUnit->addItem(Localization::text(QStringLiteral("Кадры")), QStringLiteral("frames"));
    recordingCounterUnit->setCurrentIndex(counterUnit == QLatin1String("frames") ? 1 : 0);
    recording->addRow(Localization::text(QStringLiteral("Счётчик на экране:")), recordingCounterUnit);

    const bool achievementEnabled = settings_.value(QStringLiteral("achievements/enabled"), true).toBool();
    const bool achievementHardcoreRequested = settings_.value(QStringLiteral("achievements/hardcore"), false).toBool();
    bool achievementEnabledApplied = achievementEnabled;
    bool achievementHardcoreApplied = achievementHardcoreRequested;
    auto* achievements = addPage(Localization::text(QStringLiteral("Достижения")),
        Localization::text(QStringLiteral("RetroAchievements: учёт достижений доступен после входа в аккаунт.")));
    auto* achievementEnabledBox = new QCheckBox(Localization::text(QStringLiteral("Включить достижения")), &dialog);
    achievementEnabledBox->setObjectName(QStringLiteral("achievementsEnabled"));
    achievementEnabledBox->setChecked(achievementEnabled);
    achievements->addRow(achievementEnabledBox);
    auto* achievementMode = new QComboBox(&dialog);
    achievementMode->setObjectName(QStringLiteral("achievementMode"));
    achievementMode->addItem(QStringLiteral("Softcore"), false);
    achievementMode->addItem(QStringLiteral("Hardcore"), true);
    if (!AchievementsHardcoreApproved) {
        achievementMode->setItemData(1, 0, Qt::UserRole - 1);
        achievementMode->setItemData(1, Localization::text(QStringLiteral(
            "Hardcore требует одобрения Kega Refusion сервисом RetroAchievements. Сейчас доступен Softcore.")),
            Qt::ToolTipRole);
    }
    achievementMode->setCurrentIndex(AchievementsHardcoreApproved && achievementHardcoreRequested ? 1 : 0);
    achievementMode->setEnabled(achievementEnabled);
    connect(achievementEnabledBox, &QCheckBox::toggled, achievementMode, &QWidget::setEnabled);
    achievements->addRow(Localization::text(QStringLiteral("Режим:")), achievementMode);
    auto* achievementToasts = new QCheckBox(Localization::text(QStringLiteral("Показывать уведомления о достижениях")), &dialog);
    achievementToasts->setObjectName(QStringLiteral("achievementNotifications"));
    achievementToasts->setChecked(achievementNotifications_);
    achievementToasts->setToolTip(Localization::text(QStringLiteral("При выключенных уведомлениях достижения продолжают засчитываться.")));
    achievements->addRow(achievementToasts);
    auto* hideAvailability = new QCheckBox(Localization::text(QStringLiteral("Скрывать уведомления об отсутствии достижений или Hardcore")), &dialog);
    hideAvailability->setObjectName(QStringLiteral("hideAchievementAvailability"));
    hideAvailability->setChecked(hideAchievementAvailability_);
    hideAvailability->setEnabled(achievementToasts->isChecked());
    connect(achievementToasts, &QCheckBox::toggled, hideAvailability, &QWidget::setEnabled);
    achievements->addRow(hideAvailability);
    auto* achievementSound = new QCheckBox(Localization::text(QStringLiteral("Звук открытия достижения")), &dialog);
    achievementSound->setObjectName(QStringLiteral("achievementSound"));
    achievementSound->setChecked(achievementSound_);
    achievementSound->setEnabled(achievementToasts->isChecked());
    connect(achievementToasts, &QCheckBox::toggled, achievementSound, &QWidget::setEnabled);
    achievements->addRow(achievementSound);
    auto* achievementPosition = new AchievementPositionPicker(achievementPosition_, &dialog);
    achievements->addRow(Localization::text(QStringLiteral("Положение уведомлений:")), achievementPosition);
    auto* accountStatus = new QLabel(achievementsState_.status.isEmpty()
        ? Localization::text(QStringLiteral("Войдите в RetroAchievements.")) : achievementsState_.status, &dialog);
    accountStatus->setObjectName(QStringLiteral("achievementAccountStatus"));
    accountStatus->setWordWrap(true);
    achievements->addRow(accountStatus);
    auto* achievementUser = new QLineEdit(&dialog);
    achievementUser->setObjectName(QStringLiteral("achievementUsername"));
    achievementUser->setText(achievementsState_.username);
    achievements->addRow(Localization::text(QStringLiteral("Имя пользователя:")), achievementUser);
    auto* achievementPassword = new QLineEdit(&dialog);
    achievementPassword->setObjectName(QStringLiteral("achievementPassword"));
    achievementPassword->setEchoMode(QLineEdit::Password);
    achievements->addRow(Localization::text(QStringLiteral("Пароль:")), achievementPassword);
    auto* accountButtons = new QWidget(&dialog);
    auto* accountButtonsLayout = new QHBoxLayout(accountButtons);
    accountButtonsLayout->setContentsMargins(0, 0, 0, 0);
    auto* loginButton = new QPushButton(Localization::text(QStringLiteral("Войти")), accountButtons);
    auto* logoutButton = new QPushButton(Localization::text(QStringLiteral("Выйти")), accountButtons);
    logoutButton->setEnabled(achievementsState_.loggedIn);
    accountButtonsLayout->addWidget(loginButton);
    accountButtonsLayout->addWidget(logoutButton);
    accountButtonsLayout->addStretch();
    achievements->addRow(accountButtons);
    auto* accountHint = new QLabel(&dialog);
    accountHint->setWordWrap(true);
    achievements->addRow(accountHint);
    const auto updateAccountControls = [this, achievementEnabledBox,
                                        achievementUser, achievementPassword, loginButton,
                                        logoutButton, accountHint] {
        const bool active = achievementEnabledBox->isChecked();
        achievementUser->setEnabled(active);
        achievementPassword->setEnabled(active);
        loginButton->setEnabled(active);
        logoutButton->setEnabled(active && achievementsState_.loggedIn);
        accountHint->setText(active
            ? Localization::text(QStringLiteral("Вход и выход применяются сразу."))
            : Localization::text(QStringLiteral("Включите достижения для входа в аккаунт.")));
    };
    connect(achievementEnabledBox, &QCheckBox::toggled, &dialog, updateAccountControls);
    updateAccountControls();
    connect(loginButton, &QPushButton::clicked, &dialog,
            [this, achievementEnabledBox, achievementMode, achievementUser,
             achievementPassword, accountStatus, &achievementEnabledApplied,
             &achievementHardcoreApplied] {
        if (!achievementEnabledBox->isChecked()) return;
        const QString user = achievementUser->text().trimmed();
        const QString password = achievementPassword->text();
        if (user.isEmpty() || password.isEmpty()) {
            accountStatus->setText(Localization::text(QStringLiteral("Введите имя пользователя и пароль.")));
            return;
        }
        const bool hardcore = AchievementsHardcoreApproved &&
                              achievementMode->currentData().toBool();
        if (!achievementEnabledApplied || hardcore != achievementHardcoreApplied) {
            settings_.setValue(QStringLiteral("achievements/enabled"), true);
            settings_.setValue(QStringLiteral("achievements/hardcore"), hardcore);
            session_.configureAchievements(true, hardcore);
            achievementEnabledApplied = true;
            achievementHardcoreApplied = hardcore;
        }
        session_.loginAchievements(user, password);
        achievementPassword->clear();
        accountStatus->setText(Localization::text(QStringLiteral("Вход в RetroAchievements…")));
    });
    connect(logoutButton, &QPushButton::clicked, &dialog, [this, accountStatus] {
        session_.logoutAchievements();
        accountStatus->setText(Localization::text(QStringLiteral("Выход из RetroAchievements…")));
    });
    connect(&session_, &EmulationSession::achievementsChanged, &dialog,
            [accountStatus, updateAccountControls](const AchievementsState& state) {
        accountStatus->setText(state.status);
        updateAccountControls();
    });
    const QString hardcoreHelp = AchievementsHardcoreApproved
        ? Localization::text(QStringLiteral("Hardcore отключает загрузку состояния, перемотку и покадровые шаги; "
                         "включение режима перезапускает текущую игру."))
        : Localization::text(QStringLiteral("Hardcore требует одобрения Kega Refusion сервисом RetroAchievements. "
                         "Сейчас доступен Softcore."));
    auto* achievementHelp = new QLabel(Localization::text(QStringLiteral(
        "Пароль не сохраняется; после входа хранится только токен. "
        "Вход и выход выполняются сразу; остальные изменения — после «ОК». ")) + hardcoreHelp, &dialog);
    achievementHelp->setWordWrap(true);
    achievements->addRow(achievementHelp);
    auto* accountFile = new QLabel(Localization::text(QStringLiteral("Файл входа: %1")).arg(
        QDir::toNativeSeparators(QDir(dataPath_).filePath(QStringLiteral("retroachievements-account.ini")))), &dialog);
    accountFile->setTextFormat(Qt::PlainText);
    accountFile->setWordWrap(true);
    accountFile->setTextInteractionFlags(Qt::TextSelectableByMouse);
    achievements->addRow(accountFile);

    auto* clearBadges = new QPushButton(Localization::text(QStringLiteral("Очистить кэш картинок")), &dialog);
    clearBadges->setObjectName(QStringLiteral("clearAchievementPictures"));
    auto* cacheStatus = new QLabel(Localization::text(QStringLiteral(
        "Удаляются только скачанные картинки. Достижения, прогресс и вход сохраняются.")), &dialog);
    cacheStatus->setWordWrap(true);
    achievements->addRow(clearBadges);
    achievements->addRow(cacheStatus);
    connect(clearBadges, &QPushButton::clicked, &dialog, [this, clearBadges, cacheStatus] {
        clearBadges->setEnabled(false);
        cacheStatus->setText(Localization::text(QStringLiteral("Очистка картинок…")));
        achievementNetwork_->clearBadgeCache();
    });
    connect(achievementNetwork_, &AchievementNetwork::badgeCacheCleared, &dialog,
            [clearBadges, cacheStatus](bool success) {
        clearBadges->setEnabled(true);
        cacheStatus->setText(success ? Localization::text(QStringLiteral("Кэш картинок очищен."))
            : Localization::text(QStringLiteral("Не все картинки удалось удалить. Проверьте доступ к папке кэша.")));
    });

    auto* sound = addPage(Localization::text(QStringLiteral("Звук")),
        Localization::text(QStringLiteral("Устройство воспроизведения звука игры.")));
    auto* settingsVolume = new QSlider(Qt::Horizontal, &dialog);
    settingsVolume->setObjectName(QStringLiteral("settingsMasterVolumeSlider"));
    settingsVolume->setRange(volumeSlider_->minimum(), volumeSlider_->maximum());
    settingsVolume->setValue(volumeSlider_->value());
    auto* volumeValue = new QLabel(QStringLiteral("%1%").arg(settingsVolume->value()), &dialog);
    volumeValue->setMinimumWidth(volumeValue->fontMetrics().horizontalAdvance(QStringLiteral("100%")));
    auto* settingsVolumeRow = new QHBoxLayout;
    settingsVolumeRow->addWidget(settingsVolume, 1);
    settingsVolumeRow->addWidget(volumeValue);
    sound->addRow(Localization::text(QStringLiteral("Громкость")), settingsVolumeRow);
    // Share the menu's update/persistence path, including its coalesced writes.
    connect(settingsVolume, &QSlider::valueChanged, volumeSlider_, &QSlider::setValue);
    connect(volumeSlider_, &QSlider::valueChanged, settingsVolume, &QSlider::setValue);
    connect(settingsVolume, &QSlider::valueChanged, volumeValue, [volumeValue](int value) {
        volumeValue->setText(QStringLiteral("%1%").arg(value));
    });
    auto* outputDevice = new QComboBox(&dialog);
    outputDevice->setObjectName(QStringLiteral("audioOutputDevice"));
    outputDevice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    outputDevice->setMinimumContentsLength(24);
    const auto refreshOutputs = [this, outputDevice] {
        const QByteArray selectedId = outputDevice->count()
            ? outputDevice->currentData().toByteArray() : audio_->outputDeviceId();
        outputDevice->clear();
        const QAudioDevice defaultDevice = QMediaDevices::defaultAudioOutput();
        const QString defaultName = defaultDevice.isNull()
            ? Localization::text(QStringLiteral("нет устройства")) : defaultDevice.description();
        outputDevice->addItem(Localization::text(QStringLiteral("Системное по умолчанию (%1)")).arg(defaultName), QByteArray());
        const auto outputs = QMediaDevices::audioOutputs();
        bool selectedPresent = selectedId.isEmpty();
        for (const QAudioDevice& device : outputs)
            selectedPresent = selectedPresent || device.id() == selectedId;
        if (!selectedPresent)
            outputDevice->addItem(Localization::text(QStringLiteral("Недоступно — используется системное")), selectedId);
        for (const QAudioDevice& device : outputs)
            outputDevice->addItem(device.description(), device.id());
        outputDevice->setCurrentIndex(outputDevice->findData(selectedId));
    };
    connect(audio_, &AudioOutput::outputDevicesChanged, &dialog, refreshOutputs);
    refreshOutputs();
    sound->addRow(Localization::text(QStringLiteral("Устройство вывода:")), outputDevice);
    auto* outputHelp = new QLabel(Localization::text(QStringLiteral(
        "Если выбранное устройство отключено, звук временно выводится через системное. "
        "После подключения выбор восстановится.")), &dialog);
    outputHelp->setWordWrap(true);
    sound->addRow(outputHelp);
    auto* latencyProfile = new QComboBox(&dialog);
    latencyProfile->setObjectName(QStringLiteral("audioLatencyProfile"));
    latencyProfile->addItem(Localization::text(QStringLiteral("Низкая задержка")), 1);
    latencyProfile->addItem(Localization::text(QStringLiteral("Обычный")), 0);
    latencyProfile->addItem(Localization::text(QStringLiteral("Повышенная устойчивость")), 2);
    latencyProfile->setCurrentIndex(latencyProfile->findData(int(audio_->latencyProfile())));
    sound->addRow(Localization::text(QStringLiteral("Задержка звука:")), latencyProfile);
    auto* latencyHelp = new QLabel(Localization::text(QStringLiteral(
        "Применяется сразу, без перезапуска игры. Если слышен треск, выберите повышенную устойчивость. "
        "Фактическая задержка также зависит от устройства вывода.")), &dialog);
    latencyHelp->setWordWrap(true);
    sound->addRow(latencyHelp);
    connect(latencyProfile, &QComboBox::currentIndexChanged, &dialog, [this, latencyProfile] {
        const auto profile = AudioQueue::normalizeProfile(latencyProfile->currentData().toInt());
        if (profile == audio_->latencyProfile()) return;
        audio_->setLatencyProfile(profile);
        settings_.setValue(QStringLiteral("audio/latencyProfile"), int(profile));
    });
    auto* smsFmBox = new QCheckBox(Localization::text(QStringLiteral("FM-синтезатор (Master System)")), &dialog);
    smsFmBox->setObjectName(QStringLiteral("smsFmEnabled"));
    smsFmBox->setChecked(smsFmEnabled_);
    sound->addRow(smsFmBox);
    auto* smsFmHelp = new QLabel(Localization::text(QStringLiteral(
        "Применяется при следующей загрузке игры или перезапуске питания.")), &dialog);
    smsFmHelp->setWordWrap(true);
    sound->addRow(smsFmHelp);

    auto* biosPlaceholder = new QWidget(pages);
    pages->addWidget(biosPlaceholder);
    bool biosCreated = false;
    const auto createBiosPage = [this, &dialog, pages, biosPlaceholder, addPage] {
        auto* bios = addPage(QStringLiteral("BIOS"),
            Localization::text(QStringLiteral("Файлы BIOS для запуска Sega CD и 32X.")));
        const auto reportBiosImport = [&dialog](const QString& title, const QStringList& imported,
                                               const QString& error, const QString& emptyMessage) {
            QString message;
            if (!imported.isEmpty())
                message = Localization::text(QStringLiteral("Импортированы: %1")).arg(imported.join(QStringLiteral(", ")));
            if (!error.isEmpty()) {
                if (!message.isEmpty()) message += QStringLiteral("\n\n");
                QMessageBox::warning(&dialog, title, message + error);
            } else {
                QMessageBox::information(&dialog, title, message.isEmpty() ? emptyMessage : message);
            }
        };

        auto* cdGroup = new QGroupBox(QStringLiteral("Sega CD"), &dialog);
        auto* cdLayout = new QVBoxLayout(cdGroup);
        auto* cdForm = new QFormLayout;
        cdLayout->addLayout(cdForm);
        std::array<QLabel*, 3> cdStatusLabels{};
        const QStringList cdRegions{Localization::text(QStringLiteral("Япония")), Localization::text(QStringLiteral("США")),
                                    Localization::text(QStringLiteral("Европа"))};
        for (int region = 1; region <= 3; ++region) {
            auto* status = new QLabel(cdGroup);
            status->setObjectName(QStringLiteral("biosCdStatus%1").arg(region - 1));
            cdStatusLabels[region - 1] = status;
            cdForm->addRow(cdRegions[region - 1] + QLatin1Char(':'), status);
        }
        const auto refreshCdBios = [this, cdStatusLabels] {
            for (int region = 1; region <= 3; ++region) {
                QString error;
                const QString path = SegaCdBios::resolve(dataPath_, region, error);
                const bool installed = !path.isEmpty();
                cdStatusLabels[region - 1]->setText(installed ? Localization::text(QStringLiteral("Установлен"))
                                                            : Localization::text(QStringLiteral("Не готов")));
                cdStatusLabels[region - 1]->setToolTip(installed ? QDir::toNativeSeparators(path) : error);
            }
        };
        auto* cdActions = new QHBoxLayout;
        auto* checkCd = new QPushButton(Localization::text(QStringLiteral("Проверить")), cdGroup);
        checkCd->setObjectName(QStringLiteral("checkCdBios"));
        cdActions->addWidget(checkCd);
        auto* importCd = new QPushButton(Localization::text(QStringLiteral("Импортировать из папки…")), cdGroup);
        importCd->setObjectName(QStringLiteral("importCdBios"));
        cdActions->addWidget(importCd);
        cdActions->addStretch();
        cdLayout->addLayout(cdActions);
        bios->addRow(cdGroup);
        connect(checkCd, &QPushButton::clicked, &dialog, refreshCdBios);
        connect(importCd, &QPushButton::clicked, &dialog,
                [this, &dialog, refreshCdBios, reportBiosImport] {
            const QString source = QFileDialog::getExistingDirectory(
                &dialog, Localization::text(QStringLiteral("Папка с BIOS Sega CD")), QDir::homePath());
            if (source.isEmpty()) return;
            QString error;
            const QStringList imported = SegaCdBios::importDirectory(source, dataPath_, error);
            refreshCdBios();
            reportBiosImport(QStringLiteral("BIOS Sega CD"), imported, error,
                             Localization::text(QStringLiteral("Новые BIOS не найдены.")));
        });
        auto* x32Group = new QGroupBox(QStringLiteral("32X"), &dialog);
        auto* x32Layout = new QVBoxLayout(x32Group);
        auto* x32Hint = new QLabel(Localization::text(QStringLiteral("Для 32X нужны три файла BIOS. "
            "Для CD32X также требуется BIOS Sega CD соответствующего региона.")), x32Group);
        x32Hint->setWordWrap(true);
        x32Layout->addWidget(x32Hint);
        auto* x32Form = new QFormLayout;
        x32Layout->addLayout(x32Form);
        constexpr std::array<Sega32xBios::Piece, 3> pieces{{Sega32xBios::Piece::M68k,
            Sega32xBios::Piece::MasterSh2, Sega32xBios::Piece::SlaveSh2}};
        const QStringList labels{QStringLiteral("68000"), QStringLiteral("Master SH2"),
                                 QStringLiteral("Slave SH2")};
        std::array<QLabel*, 3> x32StatusLabels{};
        for (int i = 0; i < 3; ++i) {
            auto* status = new QLabel(x32Group);
            status->setObjectName(QStringLiteral("bios32xStatus%1").arg(i));
            x32StatusLabels[i] = status;
            x32Form->addRow(labels[i] + QLatin1Char(':'), status);
        }
        const auto refresh32xBios = [this, pieces, x32StatusLabels] {
            for (int i = 0; i < 3; ++i) {
                QString error;
                const QString path = Sega32xBios::resolve(dataPath_, pieces[i], error);
                const bool installed = !path.isEmpty();
                x32StatusLabels[i]->setText(installed ? Localization::text(QStringLiteral("Установлен"))
                                                   : Localization::text(QStringLiteral("Не готов")));
                x32StatusLabels[i]->setToolTip(installed ? QDir::toNativeSeparators(path) : error);
            }
        };
        auto* x32Actions = new QHBoxLayout;
        auto* check32x = new QPushButton(Localization::text(QStringLiteral("Проверить")), x32Group);
        check32x->setObjectName(QStringLiteral("check32xBios"));
        x32Actions->addWidget(check32x);
        auto* import32x = new QPushButton(Localization::text(QStringLiteral("Импортировать из папки…")), x32Group);
        import32x->setObjectName(QStringLiteral("import32xBios"));
        x32Actions->addWidget(import32x);
        x32Actions->addStretch();
        x32Layout->addLayout(x32Actions);
        bios->addRow(x32Group);
        connect(check32x, &QPushButton::clicked, &dialog, refresh32xBios);
        connect(import32x, &QPushButton::clicked, &dialog,
                [this, &dialog, refresh32xBios, reportBiosImport] {
            const QString source = QFileDialog::getExistingDirectory(
                &dialog, Localization::text(QStringLiteral("Папка с BIOS 32X")), QDir::homePath());
            if (source.isEmpty()) return;
            QString error;
            const QStringList imported = Sega32xBios::importDirectory(source, dataPath_, error);
            refresh32xBios();
            reportBiosImport(QStringLiteral("BIOS 32X"), imported, error,
                             Localization::text(QStringLiteral("Новые BIOS не найдены или уже установлены.")));
        });
        auto* biosHelp = new QLabel(Localization::text(QStringLiteral(
            "Импорт начнётся сразу после выбора папки. Существующие BIOS не заменяются; "
            "«Отмена» не удалит импортированные файлы. Новые BIOS используются при следующем запуске игры.")),
            &dialog);
        biosHelp->setWordWrap(true);
        bios->addRow(biosHelp);
        auto* biosPage = pages->widget(pages->count() - 1);
        pages->removeWidget(biosPage);
        pages->removeWidget(biosPlaceholder);
        delete biosPlaceholder;
        auto* biosViewport = new QScrollArea(pages);
        biosViewport->setWidgetResizable(true);
        biosViewport->setFrameShape(QFrame::NoFrame);
        biosViewport->setWidget(biosPage);
        pages->insertWidget(kBiosSettingsPage, biosViewport);

        refreshCdBios();
        refresh32xBios();
    };

    auto* controlsViewport = new QScrollArea(pages);
    controlsViewport->setObjectName(QStringLiteral("controlsPageViewport"));
    controlsViewport->setWidgetResizable(true);
    controlsViewport->setFrameShape(QFrame::NoFrame);
    pages->addWidget(controlsViewport);
    auto* netplaySettings = new NetplaySettingsPage(pages);
    netplaySettings->setNickname(netplay_->nickname());
    netplaySettings->setBannedPeers(netplay_->bannedPeers());
    connect(netplaySettings, &NetplaySettingsPage::unbanRequested, &dialog,
            [this, netplaySettings](const QString& fingerprint) {
        netplay_->unban(fingerprint);
        netplaySettings->setBannedPeers(netplay_->bannedPeers());
    });
    pages->addWidget(netplaySettings);
    auto* controlsError = new QLabel(&dialog);
    controlsError->setObjectName(QStringLiteral("settingsValidationError"));
    controlsError->setWordWrap(true);
    controlsError->setStyleSheet(QStringLiteral("color: #f08c82;"));
    controlsError->hide();
    ControlsPage* controls = nullptr;
    connect(navigation, &QListWidget::currentRowChanged, &dialog,
            [this, pages, controlsViewport, controlsError, &controls, &biosCreated, createBiosPage](int row) {
        if (pages->currentIndex() == kControlsSettingsPage && row != kControlsSettingsPage && controls) {
            controls->cancelCapture();
            controlsError->clear();
        }
        if (row == kBiosSettingsPage && !biosCreated) {
            createBiosPage();
            biosCreated = true;
        }
        if (row == kControlsSettingsPage && !controls) {
            controls = new ControlsPage(*input_, controlsViewport);
            controls->setExternalFeedback(true);
            if (hasControlsViewState_) controls->setViewState(controlsViewState_);
            connect(controls, &ControlsPage::feedbackChanged, controlsError,
                    [controlsError, pages](const QString& message, bool error) {
                const bool dark = controlsError->palette().color(QPalette::Window).lightness() < 128;
                const QColor color = error ? (dark ? QColor("#f08c82") : QColor("#a43028"))
                                           : (dark ? QColor("#70d993") : QColor("#18783c"));
                controlsError->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(color.name()));
                controlsError->setText(message);
                controlsError->setVisible(pages->currentIndex() == kControlsSettingsPage && !message.isEmpty());
            });
            controls->setTopologyLocked(movieMode_ != 0 || moviePending_ || session_.networkClient() || session_.networkLinkCable());
            controlsViewport->setWidget(controls);
        }
        pages->setCurrentIndex(row);
        controlsError->setVisible(row == kControlsSettingsPage && !controlsError->text().isEmpty());
    });
    navigation->setCurrentRow(qBound(0, initialPage < 0 ? settingsPage_ : initialPage, kNetplaySettingsPage));
    layout->addWidget(controlsError);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("settingsButtons"));
    buttons->button(QDialogButtonBox::Ok)->setText(Localization::text(QStringLiteral("ОК")));
    buttons->button(QDialogButtonBox::Cancel)->setText(Localization::text(QStringLiteral("Отмена")));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, navigation, &controls] {
        if (!controls || controls->apply()) dialog.accept();
        else navigation->setCurrentRow(kControlsSettingsPage);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, [&dialog, &controls] {
        if (controls) controls->cancelCapture();
        dialog.reject();
    });
    layout->addWidget(buttons);

    const int result = dialog.exec();
    settingsPage_ = navigation->currentRow();
    if (controls) {
        controlsViewState_ = controls->viewState();
        hasControlsViewState_ = true;
    }
    if (result == QDialog::Accepted) {
        if (startupUpdates->isChecked() != checkUpdatesOnStartup) {
            settings_.setValue(QStringLiteral("updates/checkOnStartup"), startupUpdates->isChecked());
            if (updateDialog_) updateDialog_->setCheckOnStartup(startupUpdates->isChecked());
        }
        const QString selectedNickname = netplaySettings->nickname();
        if (!selectedNickname.isEmpty() && selectedNickname != netplay_->nickname())
            netplay_->setNickname(selectedNickname);
        if ((screenshotFormat->currentIndex() == 1) != jpeg)
            settings_.setValue(QStringLiteral("screenshot/format"), screenshotFormat->currentData());
        osdAction_->setChecked(showOsd->isChecked());
        fpsAction_->setChecked(emulationFps->isChecked());
        gameFpsAction_->setChecked(gameFps->isChecked());
        if (language->currentData().toString() != selectedLanguage)
            settings_.setValue(QStringLiteral("ui/language"), language->currentData());
        if (cdAttachment->isEnabled() && x32Attachment->isEnabled() &&
            (cdAttachment->currentData().toBool() != attachCd || x32Attachment->currentData().toBool() != attach32x)) {
            settings_.setValue(QStringLiteral("emulation/attachCd"), cdAttachment->currentData());
            settings_.setValue(QStringLiteral("emulation/attach32x"), x32Attachment->currentData());
            session_.setExpansionDevices(cdAttachment->currentData().toBool(), x32Attachment->currentData().toBool());
        }
        const bool selectedAchievements = achievementEnabledBox->isChecked();
        const bool selectedHardcore = achievementMode->currentData().toBool();
        if (selectedAchievements != achievementEnabledApplied ||
            selectedHardcore != achievementHardcoreApplied) {
            settings_.setValue(QStringLiteral("achievements/enabled"), selectedAchievements);
            settings_.setValue(QStringLiteral("achievements/hardcore"), selectedHardcore);
            session_.configureAchievements(selectedAchievements, selectedHardcore);
        }
        if (achievementToasts->isChecked() != achievementNotifications_) {
            achievementNotifications_ = achievementToasts->isChecked();
            settings_.setValue(QStringLiteral("achievements/notifications"), achievementNotifications_);
            if (!achievementNotifications_) view_->clearAchievementNotices();
        }
        if (hideAvailability->isChecked() != hideAchievementAvailability_) {
            hideAchievementAvailability_ = hideAvailability->isChecked();
            settings_.setValue(QStringLiteral("achievements/hideAvailabilityNotices"), hideAchievementAvailability_);
        }
        if (achievementSound->isChecked() != achievementSound_) {
            achievementSound_ = achievementSound->isChecked();
            settings_.setValue(QStringLiteral("achievements/sound"), achievementSound_);
        }
        if (achievementPosition->position() != achievementPosition_) {
            achievementPosition_ = achievementPosition->position();
            settings_.setValue(QStringLiteral("achievements/position"), achievementPosition_);
            view_->setAchievementPosition(achievementPosition_);
        }
        const QString selectedCounterUnit = recordingCounterUnit->currentData().toString();
        if (selectedCounterUnit != counterUnit) {
            settings_.setValue(QStringLiteral("recording/counterUnit"), selectedCounterUnit);
            view_->setMovieCounterAsTime(selectedCounterUnit == QLatin1String("time"));
        }
        const int selectedSpeed = speed->currentData().toInt();
        if (selectedSpeed != fastForwardFactor_) {
            fastForwardFactor_ = selectedSpeed;
            settings_.setValue(QStringLiteral("emulation/fastForwardFactor"), selectedSpeed);
            session_.setFastForwardFactor(selectedSpeed);
            view_->setFastForwardFactor(selectedSpeed);
        }
        if (checksum->isEnabled() && checksum->isChecked() != fixChecksum) {
            settings_.setValue(QStringLiteral("emulation/autoFixChecksum"), checksum->isChecked());
            session_.setAutoFixChecksum(checksum->isChecked());
        }
        const bool selectedFastCd = cdLoading->currentData().toBool();
        if (cdLoading->isEnabled() && selectedFastCd != fastCd) {
            settings_.setValue(QStringLiteral("emulation/fastCdLoading"), selectedFastCd);
            session_.setFastCdLoading(selectedFastCd);
        }
        const bool selectedNoSpriteLimit = spriteLimit->currentData().toBool();
        if (spriteLimit->isEnabled() && selectedNoSpriteLimit != noSpriteLimit) {
            settings_.setValue(QStringLiteral("emulation/removeSpriteLimit"), selectedNoSpriteLimit);
            session_.setRemoveSpriteLimit(selectedNoSpriteLimit);
        }
        if (recordHistory->isChecked() != rewindEnabled_) {
            rewindEnabled_ = recordHistory->isChecked();
            settings_.setValue(QStringLiteral("emulation/rewindEnabled"), rewindEnabled_);
            session_.setRewindEnabled(rewindEnabled_);
            if (!rewindEnabled_) {
                rewindHeld_ = rewindPadHeld_ = false;
                rewindAction_->setChecked(false);
            }
            rewindAction_->setEnabled(session_.hasGame() && rewindEnabled_ && !achievementsHardcore_);
            updateRewind();
        }
        if (rewindMemory->value() != memoryMiB) {
            settings_.setValue(QStringLiteral("emulation/rewindMemoryMiB"), rewindMemory->value());
            session_.setRewindMemoryMiB(rewindMemory->value());
        }
        setSmsFmEnabled(smsFmBox->isChecked());
        const QByteArray selectedOutputId = outputDevice->currentData().toByteArray();
        if (selectedOutputId != audio_->outputDeviceId()) {
            audio_->setOutputDeviceId(selectedOutputId);
            settings_.setValue(QStringLiteral("audio/outputDeviceId"), selectedOutputId);
        }
    }
    view_->setFocus();
    updateMovieActions();
    updateInputEnabled();
}

void MainWindow::setSmsFmEnabled(bool enabled) {
    if (smsFmEnabled_ == enabled) return;
    smsFmEnabled_ = enabled;
    settings_.setValue(QStringLiteral("audio/smsFmEnabled"), enabled);
    const QSignalBlocker blocker(smsFmAction_);
    smsFmAction_->setChecked(enabled);
    session_.setSmsFmEnabled(enabled);
}

void MainWindow::showAchievements() {
    releaseInput();
    AchievementsDialog dialog(this);
    dialog.setState(achievementsState_);
    connect(&session_, &EmulationSession::achievementsChanged,
            &dialog, &AchievementsDialog::setState);
    connect(&dialog, &AchievementsDialog::badgeRequested,
            achievementNetwork_, &AchievementNetwork::requestBadge);
    connect(achievementNetwork_, &AchievementNetwork::badgeReady,
            &dialog, &AchievementsDialog::setBadge);
    connect(achievementNetwork_, &AchievementNetwork::badgeCacheCleared,
            &dialog, [&dialog](bool) { dialog.clearBadgeImages(); });
    session_.requestAchievements();
    dialog.exec();
    view_->setFocus();
    updateInputEnabled();
}

void MainWindow::editCheats() {
    if (!session_.hasGame() || loading_ || !cheatsSupported_ || achievementsHardcore_) return;
    releaseInput();
    CheatsDialog dialog(cheats_,cheatPlatform_,currentGameName_,this);
    if (dialog.exec() == QDialog::Accepted && dialog.cheats() != cheats_)
        session_.setCheats(dialog.cheats());
    view_->setFocus();
    updateInputEnabled();
}

void MainWindow::showCheatSearch() {
    if (!cheatSearchAction_->isEnabled()) return;
    releaseInput();
    if (!cheatSearchDialog_) {
        auto* dialog=new CheatSearchDialog(this);
        cheatSearchDialog_=dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog,&CheatSearchDialog::searchRequested,&session_,&EmulationSession::searchCheats);
        connect(dialog,&CheatSearchDialog::clearRequested,&session_,&EmulationSession::clearCheatSearch);
        connect(&session_,&EmulationSession::cheatSearchResults,dialog,&CheatSearchDialog::setResults);
        connect(&session_,&EmulationSession::cheatsChanged,dialog,
            [dialog](const CheatList& entries,int,bool,const QString&) { dialog->setCheats(entries); });
        connect(&session_,&EmulationSession::errorOccurred,dialog,&CheatSearchDialog::setSaveError);
        connect(dialog,&CheatSearchDialog::saveRequested,this,[this,dialog](const CheatEntry& entry) {
            if (!cheatsAction_->isEnabled()) return;
            auto entries=cheats_;
            auto existing=std::find_if(entries.begin(),entries.end(),[&](const CheatEntry& item) { return item.code==entry.code; });
            if (existing==entries.end()) entries.append(entry); else *existing=entry;
            CheatList normalized; QString error;
            if (!Cheats::normalize(entries,cheatPlatform_,normalized,error)) {
                dialog->setSaveError(error); return;
            }
            // Keep consecutive manual additions together until the worker confirms the list.
            cheats_=normalized;
            session_.setCheats(normalized);
        });
        connect(&session_,&EmulationSession::gameLoaded,dialog,[dialog] { dialog->resetSearch(); });
        connect(&session_,&EmulationSession::gameClosed,dialog,&QWidget::close);
    }
    cheatSearchDialog_->show(); cheatSearchDialog_->raise(); cheatSearchDialog_->activateWindow();
}

void MainWindow::deliverMouseMotion(unsigned player, double dx, double dy) {
    if (player >= MaxGunPlayers || !input_->isConnected(player) ||
        input_->controllerType(player) != ControllerType::Mouse ||
        !std::isfinite(dx) || !std::isfinite(dy)) return;
    const QPointF total = mouseMotionRemainder_[player] + QPointF(dx, dy);
    const double boundedX = std::clamp(total.x(), -32767.999, 32767.999);
    const double boundedY = std::clamp(total.y(), -32767.999, 32767.999);
    const int wholeX = int(std::trunc(boundedX));
    const int wholeY = int(std::trunc(boundedY));
    mouseMotionRemainder_[player] = QPointF(boundedX - wholeX, boundedY - wholeY);
    if (wholeX || wholeY) session_.addMouseMotion(player, wholeX, wholeY);
}

void MainWindow::resetAnalogMotion() {
    view_->releaseMouseCapture();
    mouseMotionRemainder_.fill(QPointF());
    session_.clearMouseMotion();
    view_->resetAnalogPointer();
}

void MainWindow::updateGunAim(unsigned player, QPointF position, bool offscreen) {
    if (player >= MaxGunPlayers || !input_->isConnected(player) ||
        !isLightgun(input_->controllerType(player))) return;
    offscreen = offscreen || position.x() < 0 || position.x() > 1 || position.y() < 0 || position.y() > 1;
    position.setX(qBound(0.0,position.x(),1.0));
    position.setY(qBound(0.0,position.y(),1.0));
    gunAims_[player] = position;
    view_->setLightgunAim(player,position,offscreen);
    session_.setGunAim(player,int(std::lround(position.x()*65534-32767)),
                       int(std::lround(position.y()*65534-32767)),offscreen);
}

void MainWindow::updateGunConfiguration() {
    if (!applyingSessionControllers_) {
        for (unsigned player=0;player<MaxControllerPlayers;++player)
            session_.setControllerKind(player,input_->controllerType(player));
    }
    updateGunPresentation();
}

void MainWindow::updateGunPresentation() {
    resetAnalogMotion();
    const bool online = netplay_ && netplay_->active();
    const int localRole = session_.networkRole();
    unsigned source = 0;
    bool gunRole = false;
    bool analogRole = false;
    if (online) {
        QJsonArray controllers;
        for (unsigned player = 0; player < MaxControllerPlayers; ++player)
            controllers.append(QJsonObject{{"connected", input_->isConnected(player)},
                                           {"kind", int(input_->controllerType(player))}});
        const QJsonObject hardware{{"adapter", int(input_->adapter())},
                                   {"jcart", input_->jcart()},
                                   {"jcartBase", int(input_->jcartPlayerBase())},
                                   {"controllers", controllers}};
        source = NetplayTimeline::localInputSource(localRole, hardware);
        if (localRole >= 1 && localRole <= int(MaxGunPlayers)) {
            const unsigned target = unsigned(localRole - 1);
            const ControllerType kind = input_->controllerType(target);
            gunRole = input_->isConnected(target) && isLightgun(kind);
            analogRole = input_->isConnected(target) && isAnalogController(kind);
        }
    }
    input_->setNetworkInputSourceEnabled(online && localRole > 0 && !gunRole && !analogRole);
    int mouseGunPlayer = -1;
    int analogMousePlayer = -1;
    ControllerType analogMouseType = ControllerType::Pad;
    for (unsigned player = 0; player < MaxGunPlayers; ++player) {
        const ControllerType kind = input_->controllerType(player);
        const bool connected = input_->isConnected(player);
        const bool gun = connected && isLightgun(kind) &&
                         (!online || (gunRole && player == source));
        const bool analog = connected && isAnalogController(kind) &&
                            (!online || (analogRole && player == source));
        view_->setCrosshairVisible(player, input_->mapping(input_->profile(), int(player)).showCrosshair);
        const unsigned gunNumber = online && gunRole
            ? unsigned(input_->profile() == ConsoleProfile::MegaDrive ? localRole - 1 : localRole)
            : (input_->profile() == ConsoleProfile::MegaDrive ? player : player + 1);
        view_->setLightgunEnabled(player, gun, gunNumber);
        if (gun && input_->aimWithMouse(player) && mouseGunPlayer < 0)
            mouseGunPlayer = int(player);
        if (analog && input_->aimWithMouse(player) && analogMousePlayer < 0) {
            analogMousePlayer = int(player);
            analogMouseType = kind;
        }
    }
    view_->setMouseGunPlayer(mouseGunPlayer);
    view_->setAnalogMousePlayer(analogMousePlayer, analogMouseType);
    for (unsigned player = 0; player < MaxGunPlayers; ++player) {
        const ControllerType kind = input_->controllerType(player);
        const bool connected = input_->isConnected(player);
        gunAims_[player] = QPointF(.5, .5);
        if (connected && isLightgun(kind)) {
            updateGunAim(player, gunAims_[player], input_->aimWithMouse(player));
        } else if (connected && kind == ControllerType::Paddle) {
            if (presentedControllerKinds_[player] != ControllerType::Paddle ||
                !presentedControllerConnected_[player])
                session_.setPaddlePosition(player, 0);
        } else if (!connected || kind != ControllerType::Mouse) {
            session_.setGunAim(player, 0, 0, true);
        }
        presentedControllerKinds_[player] = kind;
        presentedControllerConnected_[player] = connected;
    }
}

void MainWindow::editControls() { editSettings(kControlsSettingsPage); }

QString MainWindow::chooseMovieSavePath(bool branch) {
    releaseInput();
    const QDir movies(QDir(dataPath_).filePath(QStringLiteral("movies")));
    if (!QDir().mkpath(movies.absolutePath())) {
        QMessageBox::warning(this, Localization::text(QStringLiteral("Запись")),
            Localization::text(QStringLiteral("Не удалось создать папку записей: %1")).arg(movies.absolutePath()));
        view_->setFocus();
        return {};
    }
    QString stem = currentGameName_.left(80);
    for (qsizetype i = 0; i < stem.size(); ++i)
        if (stem.at(i).unicode() < 32 || QStringLiteral("\\/:*?\"<>|").contains(stem.at(i)))
            stem[i] = QLatin1Char('_');
    if (stem.trimmed().isEmpty()) stem = QStringLiteral("game");
    const QString proposal = movies.filePath(stem + QLatin1Char('_') +
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz")) +
        (branch ? QStringLiteral("-branch") : QString()) + QStringLiteral(".krm"));
    QString path = QFileDialog::getSaveFileName(this,
        branch ? Localization::text(QStringLiteral("Записывать с этого места в новый файл")) : Localization::text(QStringLiteral("Начать запись управления")),
        proposal, Localization::text(QStringLiteral("Записи управления (*.krm)")), nullptr,
        QFileDialog::DontConfirmOverwrite);
    view_->setFocus();
    if (path.isEmpty()) return {};
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".krm");
    const QFileInfo file(path);
    path = file.absoluteFilePath();
    // A branch is always a new file; leave source movies and earlier recordings intact.
    if (file.exists()) {
        QMessageBox::warning(this, Localization::text(QStringLiteral("Запись")),
            Localization::text(QStringLiteral("Файл уже существует. Выберите новое имя: %1")).arg(path));
        return {};
    }
    if (branch && !moviePath_.isEmpty()) {
        const Qt::CaseSensitivity sensitivity =
#ifdef Q_OS_WIN
            Qt::CaseInsensitive;
#else
            Qt::CaseSensitive;
#endif
        if (QDir::cleanPath(path).compare(QDir::cleanPath(QFileInfo(moviePath_).absoluteFilePath()),
                                          sensitivity) == 0) {
            QMessageBox::warning(this, Localization::text(QStringLiteral("Запись")),
                Localization::text(QStringLiteral("Для продолжения записи выберите новый файл.")));
            return {};
        }
    }
    return path;
}

QString MainWindow::chooseVideoSavePath(bool exporting) {
    releaseInput();
    const QString fallback = QDir(dataPath_).filePath(QStringLiteral("videos"));
    const QDir directory(settings_.value(QStringLiteral("video/lastDirectory"), fallback).toString());
    if (!QDir().mkpath(directory.absolutePath())) {
        QMessageBox::warning(this, Localization::text(QStringLiteral("Видео")),
            Localization::text(QStringLiteral("Не удалось создать папку видео: %1"))
                .arg(QDir::toNativeSeparators(directory.absolutePath())));
        return {};
    }
    QString stem = currentGameName_.left(80);
    for (qsizetype i = 0; i < stem.size(); ++i)
        if (stem.at(i).unicode() < 32 || QStringLiteral("\\/:*?\"<>|").contains(stem.at(i)))
            stem[i] = QLatin1Char('_');
    if (stem.trimmed().isEmpty()) stem = QStringLiteral("game");
    const QString proposal = directory.filePath(stem + QLatin1Char('_') +
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz")) +
        QStringLiteral(".mov"));
    QString path = QFileDialog::getSaveFileName(this,
        exporting ? Localization::text(QStringLiteral("Экспортировать запись управления в видео"))
                  : Localization::text(QStringLiteral("Начать запись видео")),
        proposal, Localization::text(QStringLiteral("Видео MOV без потерь (*.mov)")), nullptr,
        QFileDialog::DontConfirmOverwrite);
    view_->setFocus();
    if (path.isEmpty()) return {};
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".mov");
    const QFileInfo file(path);
    if (file.exists()) {
        QMessageBox::warning(this, Localization::text(QStringLiteral("Видео")),
            Localization::text(QStringLiteral("Файл уже существует. Выберите новое имя: %1"))
                .arg(QDir::toNativeSeparators(file.absoluteFilePath())));
        return {};
    }
    settings_.setValue(QStringLiteral("video/lastDirectory"), file.absolutePath());
    return file.absoluteFilePath();
}

void MainWindow::startVideoCapture() {
    if (!session_.hasGame() || loading_ || videoActive_ || videoPending_ || videoExporting_) return;
    const QString path = chooseVideoSavePath(false);
    if (path.isEmpty()) return;
    videoPending_ = true;
    videoStopping_ = false;
    updateVideoActions();
    session_.startVideoRecording(path);
}

void MainWindow::exportInputVideo() {
    if (!session_.hasGame() || loading_ || videoActive_ || videoPending_ || videoExporting_ || movieMode_ != 0 ||
        moviePending_ || achievementsHardcore_ || session_.networkActive() || netplay_->active()) return;
    releaseInput();
    const QString source = QFileDialog::getOpenFileName(this,
        Localization::text(QStringLiteral("Экспортировать запись управления в видео")),
        QDir(dataPath_).filePath(QStringLiteral("movies")),
        Localization::text(QStringLiteral("Записи управления (*.krm);;Все файлы (*)")));
    view_->setFocus();
    if (source.isEmpty()) return;
    const QString path = chooseVideoSavePath(true);
    if (path.isEmpty() || !session_.hasGame() || loading_ || movieMode_ != 0 ||
        moviePending_ || achievementsHardcore_ || session_.networkActive() || netplay_->active()) return;
    videoPending_ = true;
    videoExporting_ = true;
    videoStopping_ = false;
    releaseInput();
    updateMovieActions();
    updateDebugDialogs();
    updateInputEnabled();
    videoProgress_ = new QProgressDialog(
        Localization::text(QStringLiteral("Подготовка экспорта…")),
        Localization::text(QStringLiteral("Отмена")), 0, 0, this);
    videoProgress_->setObjectName(QStringLiteral("videoExportProgress"));
    videoProgress_->setWindowTitle(Localization::text(QStringLiteral("Экспорт видео")));
    videoProgress_->setWindowModality(Qt::NonModal);
    videoProgress_->setMinimumDuration(0);
    videoProgress_->setAutoClose(false);
    videoProgress_->setAutoReset(false);
    connect(videoProgress_, &QProgressDialog::canceled, this, &MainWindow::stopVideoCapture);
    videoProgress_->show();
    session_.exportMovieVideo(source, path);
}

void MainWindow::stopVideoCapture() {
    if ((!videoActive_ && !videoPending_ && !videoExporting_) || videoStopping_) return;
    videoStopping_ = true;
    if (videoProgress_)
        videoProgress_->setLabelText(Localization::text(QStringLiteral("Завершение видео…")));
    updateVideoActions();
    session_.stopVideoRecording();
}

void MainWindow::updateVideoActions() {
    if (!videoMenu_) return;
    const bool busy = videoActive_ || videoPending_ || videoExporting_;
    const bool game = session_.hasGame() && !loading_;
    const QString title = currentGameName_.isEmpty() ? QStringLiteral("Kega Refusion")
        : QStringLiteral("%1 — Kega Refusion").arg(currentGameName_);
    setWindowTitle(videoActive_ && !videoExporting_
        ? title + Localization::text(QStringLiteral(" — запись видео")) : title);
    videoMenu_->setEnabled(game || busy);
    videoStartAction_->setEnabled(game && !busy);
    videoStartAction_->setText(videoActive_ && !videoExporting_
        ? Localization::text(QStringLiteral("Идёт запись видео"))
        : Localization::text(QStringLiteral("Начать запись видео…")));
    videoStartAction_->setToolTip(Localization::text(QStringLiteral(
        "MOV: видео PNG без потерь в исходном разрешении, звук AAC 256 кбит/с; без OSD.")));
    videoExportAction_->setEnabled(game && !busy && movieMode_ == 0 && !moviePending_ &&
        !achievementsHardcore_ && !session_.networkActive() && !netplay_->active());
    videoStopAction_->setEnabled(busy && !videoStopping_);
    if (openGameAction_) openGameAction_->setEnabled(!loading_ && !videoExporting_);
    if (recentMenu_) recentMenu_->setEnabled(!recentPaths_.isEmpty() && !loading_ && !videoExporting_);
    for (QMenu* menu : exportBlockedMenus_) menu->setEnabled(!videoExporting_);
}

void MainWindow::updateMovieActions() {
    if (!movieMenu_) return;
    const bool game = session_.hasGame() && !loading_ && !videoExporting_;
    const bool protectedMovie = movieMode_ != 0;
    const bool ready = game && !moviePending_;
    movieMenu_->setEnabled(game);
    movieRecordAction_->setEnabled(ready && movieMode_ == 0);
    movieResumeAction_->setEnabled(ready && movieMode_ == 0 && !achievementsHardcore_);
    moviePlayAction_->setEnabled(ready && movieMode_ == 0 && !achievementsHardcore_);
    movieTakeoverAction_->setEnabled(ready && (movieMode_ == 2 || movieMode_ == 3));
    movieRerecordAction_->setEnabled(ready && !achievementsHardcore_ &&
                                     (movieMode_ == 2 || movieMode_ == 3));
    movieStopAction_->setEnabled(ready && protectedMovie);
    const QString stopLabel = movieMode_ == 1
        ? Localization::text(QStringLiteral("Сохранить и закончить запись"))
        : Localization::text(QStringLiteral("Остановить воспроизведение"));
    if (movieStopAction_->text() != stopLabel) movieStopAction_->setText(stopLabel);
    if (regionMenu_) regionMenu_->setEnabled(!protectedMovie && !moviePending_ &&
                                             !videoExporting_ && !achievementsHardcore_);
    if (resetAction_) resetAction_->setEnabled(game && !protectedMovie && !moviePending_);
    if (hardResetAction_) hardResetAction_->setEnabled(game && !protectedMovie && !moviePending_);
    if (cheatsAction_) cheatsAction_->setEnabled(game && cheatsSupported_ && !protectedMovie &&
                                                !moviePending_ && !achievementsHardcore_);
    if (saveAction_) saveAction_->setEnabled(game && !moviePending_);
    if (loadAction_) loadAction_->setEnabled(game && !moviePending_ && !achievementsHardcore_);
    const bool namedStateAvailable = game && !moviePending_ && !session_.networkClient();
    if (saveFileAction_) saveFileAction_->setEnabled(namedStateAvailable);
    if (loadFileAction_) loadFileAction_->setEnabled(namedStateAvailable && !achievementsHardcore_);
    if (rewindAction_) rewindAction_->setEnabled(game && rewindEnabled_ &&
                                                !moviePending_ && !achievementsHardcore_);
    if (changeDiscAction_) changeDiscAction_->setEnabled(!discPaths_.isEmpty() && !protectedMovie && !moviePending_ && !videoExporting_);
    if (ejectDiscAction_) ejectDiscAction_->setEnabled(!discPaths_.isEmpty() && !protectedMovie && !moviePending_ && !videoExporting_);
    if (discMenu_) discMenu_->setEnabled(discPaths_.size() > 1 && !protectedMovie && !moviePending_ && !videoExporting_);
    if (pauseAction_) pauseAction_->setEnabled(game);
    if (fastForwardAction_) fastForwardAction_->setEnabled(game);
    updateFrameActions();
    updateVideoActions();
}

void MainWindow::updateFrameActions() {
    const bool ready = session_.hasGame() && pauseAction_ && pauseAction_->isChecked() &&
        !loading_ && !videoExporting_ && !achievementsHardcore_;
    if (frameAdvanceAction_) frameAdvanceAction_->setEnabled(ready);
    const bool backReady = ready && !moviePending_ && !session_.networkActive() &&
        (!netplay_ || !netplay_->active());
    if (frameBackAction_) frameBackAction_->setEnabled(backReady);
    if (!backReady && (frameBackHeld_ || frameBackPadHeld_)) {
        frameBackHeld_ = frameBackPadHeld_ = false;
        session_.setFrameBackHeld(false);
    }
}

void MainWindow::setFrameAdvanceSource(bool keyboard, bool pressed) {
    bool& source = keyboard ? frameAdvanceHeld_ : frameAdvancePadHeld_;
    if (pressed && (!frameAdvanceAction_ || !frameAdvanceAction_->isEnabled())) return;
    if (source == pressed) return;
    const bool wasHeld = frameAdvanceHeld_ || frameAdvancePadHeld_;
    source = pressed;
    const bool held = frameAdvanceHeld_ || frameAdvancePadHeld_;
    if (held == wasHeld) return;
    if (held) session_.advanceFrame();
    session_.setFrameAdvanceHeld(held);
}

void MainWindow::setFrameBackSource(bool keyboard, bool pressed) {
    bool& source = keyboard ? frameBackHeld_ : frameBackPadHeld_;
    if (pressed && (!frameBackAction_ || !frameBackAction_->isEnabled())) return;
    if (source == pressed) return;
    const bool wasHeld = frameBackHeld_ || frameBackPadHeld_;
    source = pressed;
    const bool held = frameBackHeld_ || frameBackPadHeld_;
    if (held == wasHeld) return;
    if (held) session_.backFrame();
    session_.setFrameBackHeld(held);
}

void MainWindow::releaseInput() {
    view_->setLightgunInputEnabled(false);
    resetAnalogMotion();
    fastForwardHeld_ = false;
    fastForwardPadHeld_ = false;
    frameAdvanceHeld_ = frameAdvancePadHeld_ = false;
    session_.setFrameAdvanceHeld(false);
    frameBackHeld_ = frameBackPadHeld_ = false;
    session_.setFrameBackHeld(false);
    rewindHeld_ = rewindPadHeld_ = false;
    if (rewindAction_ && rewindAction_->isChecked()) rewindAction_->setChecked(false);
    updateRewind();
    input_->releaseKeyboard();
    input_->setBackgroundGamepadActive(false);
    input_->setEnabled(false);
    if (fastForwardAction_ && fastForwardAction_->isChecked())
        fastForwardAction_->setChecked(false);
    updateFastForward();
}

void MainWindow::updateInputEnabled() {
    if (closeGameAction_) closeGameAction_->setEnabled(session_.hasGame() && !loading_ && !videoExporting_);
    if (cheatsAction_) cheatsAction_->setEnabled(session_.hasGame() && cheatsSupported_ && !loading_ &&
                                                  movieMode_ == 0 && !videoExporting_ && !achievementsHardcore_);
    updateFrameActions();
    if (screenshotAction_) screenshotAction_->setEnabled(session_.hasGame() && !loading_);
    const bool modalActive = QApplication::activeModalWidget();
    const bool popupActive = menuPauseActive_ || QApplication::activePopupWidget();
    bool debugWindowActive = false;
    for (QWidget* active = QApplication::activeWindow(); active; active = active->parentWidget()) {
        if (active == debugGraphicsDialog_ || active == debugAudioDialog_) {
            debugWindowActive = true;
            break;
        }
    }
    session_.setBackgroundPaused(!videoExporting_ && !netplay_->active() &&
        (modalActive || popupActive || (pauseOnFocusLoss_ &&
        ((!isActiveWindow() && !debugWindowActive) ||
         QApplication::applicationState() != Qt::ApplicationActive))));
    const bool enabled = isActiveWindow() && view_->hasFocus() && session_.hasGame() &&
                         !loading_ && !moviePending_ && !videoExporting_ && !modalActive && !popupActive;
    view_->setLightgunInputEnabled(enabled);
    input_->setEnabled(enabled);
    input_->setBackgroundGamepadActive(input_->backgroundGamepadEnabled() &&
        session_.hasGame() && !loading_ && !moviePending_ && !videoExporting_ &&
        !modalActive && !popupActive && !enabled);
    input_->poll();
}

void MainWindow::updateFastForward() {
    const bool enabled = !videoExporting_ && (fastForwardHeld_ || fastForwardPadHeld_ ||
                         (fastForwardAction_ && fastForwardAction_->isChecked()));
    view_->setFastForwardStatus(enabled && !(rewindEnabled_ && (rewindHeld_ || rewindPadHeld_ ||
        (rewindAction_ && rewindAction_->isChecked()))));
    if (fastForwardActive_ == enabled) return;
    fastForwardActive_ = enabled;
    if (enabled) audio_->clear();
    session_.setFastForward(enabled);
}

void MainWindow::updateRewind() {
    const bool enabled = rewindEnabled_ && !videoExporting_ && !achievementsHardcore_ && (rewindHeld_ || rewindPadHeld_ ||
        (rewindAction_ && rewindAction_->isChecked()));
    session_.setRewind(enabled);
    updateFastForward();
}

void MainWindow::setPausedUi(bool paused) {
    if (!paused) {
        frameAdvanceHeld_ = frameAdvancePadHeld_ = false;
        session_.setFrameAdvanceHeld(false);
    }
    if (pauseAction_) {
        const QSignalBlocker blocker(pauseAction_);
        pauseAction_->setChecked(paused);
    }
    updateFrameActions();
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
    if (videoExporting_) return;
    if (command == RewindCommand) { rewindPadHeld_ = pressed; updateRewind(); return; }
    if (command == FastForwardCommand) {
        fastForwardPadHeld_ = pressed;
        updateFastForward();
        return;
    }
    if (command == FrameAdvanceCommand) {
        setFrameAdvanceSource(false, pressed);
        return;
    }
    if (command == FrameBackCommand) {
        setFrameBackSource(false, pressed);
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
    // Apply before the first native show, including Qt-created message/file
    // dialogs. MainWindow and the modeless lobby keep their own window flags.
    if (event->type() == QEvent::Polish) {
        if (auto* dialog = qobject_cast<QDialog*>(watched)) {
            auto flags = dialog->windowFlags();
            flags &= ~(Qt::WindowMinMaxButtonsHint | Qt::WindowContextHelpButtonHint);
            flags |= Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint;
            if (flags != dialog->windowFlags()) dialog->setWindowFlags(flags);
        }
    }
    if (watched == this && event->type() == QEvent::WindowBlocked) {
        if (!netplay_->active() && !videoExporting_) session_.setBackgroundPaused(true);
        releaseInput();
    }
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
    if (!shortcutOverride && !pressed && !keyEvent->isAutoRepeat() &&
        key == Qt::Key_Period && frameAdvanceHeld_) {
        setFrameAdvanceSource(true, false);
        return true;
    }
    if (!shortcutOverride && !pressed && !keyEvent->isAutoRepeat() &&
        key == Qt::Key_Comma && frameBackHeld_) {
        setFrameBackSource(true, false);
        return true;
    }
    const bool volumeUp = (key == Qt::Key_Equal || key == Qt::Key_Plus) &&
        (modifiers == Qt::NoModifier || modifiers == Qt::ShiftModifier);
    const bool volumeDown = key == Qt::Key_Minus && modifiers == Qt::NoModifier;
    if (volumeUp || volumeDown) {
        if (shortcutOverride) event->accept();
        else if (pressed && volumeSlider_)
            volumeSlider_->setValue(qBound(0, volumeSlider_->value() +
                                            (volumeUp ? 5 : -5), 100));
        return true;
    }
    // Claim these keys before Qt resolves the logical QAction shortcuts. A
    // shortcut and its physical counterpart must produce exactly one action.
    QAction* commandAction = nullptr;
    if (modifiers == Qt::NoModifier) {
        if (key == Qt::Key_P) commandAction = pauseAction_;
        else if (key == Qt::Key_Period) commandAction = frameAdvanceAction_;
        else if (key == Qt::Key_Comma) commandAction = frameBackAction_;
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
        else if (commandAction == frameAdvanceAction_) {
            if (!keyEvent->isAutoRepeat()) setFrameAdvanceSource(true, pressed);
        } else if (commandAction == frameBackAction_) {
            if (!keyEvent->isAutoRepeat()) setFrameBackSource(true, pressed);
        } else if (pressed && !keyEvent->isAutoRepeat()) commandAction->trigger();
        return true;
    }
    if (key == Qt::Key_Escape && modifiers == Qt::NoModifier && view_->mouseCaptured()) {
        if (shortcutOverride) event->accept();
        else if (pressed && !keyEvent->isAutoRepeat()) view_->releaseMouseCapture();
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
    if (!session_.hasGame() || loading_ || videoExporting_ || !view_->hasFocus())
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
    if (loading_ || videoExporting_) { event->ignore(); return; }
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
    if (loading_ || videoExporting_) { event->ignore(); return; }
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && isSupportedDropFile(url.toLocalFile())) {
            raise();
            activateWindow();
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
    if (videoActive_ || videoPending_ || videoExporting_) {
        event->ignore();
        closeAfterVideo_ = true;
        stopVideoCapture();
        return;
    }
    if (movieMode_ == 1 || (moviePending_ && moviePendingMode_ == 1)) {
        event->ignore();
        if (!closeAfterMovie_) {
            closeAfterMovie_ = true;
            moviePending_ = true;
            moviePendingMode_ = 0;
            updateMovieActions();
            session_.closeGame(); // Save and unload while movie battery writes are still guarded.
        }
        return;
    }
    if (volumeSaveTimer_ && volumeSaveTimer_->isActive()) flushVolumePreference();
    settings_.setValue(QStringLiteral("window/geometry"),
                       isFullScreen() && !windowedGeometry_.isEmpty() ?
                           windowedGeometry_ : saveGeometry());
    settings_.sync();
    QMainWindow::closeEvent(event);
}
