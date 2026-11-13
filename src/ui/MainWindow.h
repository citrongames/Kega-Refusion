#pragma once

#include "GameView.h"
#include "ControlsPage.h"
#include "input/ControllerTopology.h"
#include "../core/Cheats.h"
#include "../core/Achievements.h"
#include <array>

#include <QColor>
#include <QHash>
#include <QMainWindow>
#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#endif
#include <QSettings>
#include <QPointer>
#include <QStringList>
#include <QVector>

class QAction;
class QActionGroup;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QMenu;
class QProgressDialog;
class QTimer;
class AudioOutput;
class AchievementNetwork;
class EmulationSession;
class DebugGraphicsDialog;
class CheatSearchDialog;
class DebugAudioDialog;
class InputManager;
class NetplayController;
class UpdateService;
class UpdateDialog;
enum class ConsoleProfile;

class MainWindow final : public QMainWindow
#ifdef Q_OS_WIN
    , private QAbstractNativeEventFilter
#endif
{
    Q_OBJECT
public:
    explicit MainWindow(EmulationSession& session, const QString& dataPath,
                        bool smokeTest = false, QWidget* parent = nullptr);
    ~MainWindow() override;
    void openRom(const QString& path);
    void setFullscreenMode(bool enabled);
    bool isMainMenuVisible() const;
    void showOsdDemo();
    void checkForUpdates(bool interactive = true);

signals:
    void volumeChanged(int value);

protected:
#ifdef Q_OS_WIN
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override;
#endif
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void buildMenus();
    void showUpdateDialog(bool activate = true);
    void refreshUpdateDialog();
    void setSmsFmEnabled(bool enabled);
    void setVolume(int value);
    void flushVolumePreference();
    void updateDebugDialogs();
    void applyDebugMasks();
    void showDebugGraphics();
    void showDebugAudio();
    void openGameDialog();
    void setLoadingUi(bool loading, const QString& fileName = {});
    void saveScreenshot();
    void updateRecentMenu();
    void updateDiscMenu();
    void editSettings(int initialPage = -1);
    void editControls();
    void editCheats();
    void showCheatSearch();
    void showAchievements();
    void updateGunConfiguration();
    void updateGunPresentation();
    void updateGunAim(unsigned player, QPointF position, bool offscreen);
    void deliverMouseMotion(unsigned player, double dx, double dy);
    void resetAnalogMotion();
    bool applyShaderPreset(const QString& path);
    void editShaderParameters();
    void updateShaderActions();
    void setGameGearLcdEnabled(bool enabled);
    void releaseInput();
    void updateMovieActions();
    void updateVideoActions();
    void setGameSpeedPercent(int percent);
    void updateGameSpeedActions();
    int effectiveGameSpeedPercent() const;
    void startVideoCapture();
    void exportInputVideo();
    void stopVideoCapture();
    QString chooseVideoSavePath(bool exporting);
    void updateFrameActions();
    void setFrameAdvanceSource(bool keyboard, bool pressed);
    void setFrameBackSource(bool keyboard, bool pressed);
    QString chooseMovieSavePath(bool branch);
    void updateInputEnabled();
    void updateFastForward();
    void updateRewind();
    void setPausedUi(bool paused);
    void setSlot(int slot);
    void setMainMenuVisible(bool visible);
    void toggleFullscreenMenu();
    void pauseForMenu();
    void handleInputCommand(int command, bool pressed, bool keyboard);
    QAction* commandAction(int command) const;
    void refreshCommandShortcuts();
    int keyboardCommand(int chord, int key, Qt::KeyboardModifiers modifiers) const;
    QHash<int, int> keyboardCommands_; // chord -> command
    QHash<int, int> heldCommandChords_; // held command -> physical chord at key-down

    EmulationSession& session_;
    QSettings settings_;
    int settingsPage_ = 0;
    std::array<ControlsPage::ViewState, 4> controlsViewStates_{};
    QString dataPath_;
    GameView* view_ = nullptr;
    AudioOutput* audio_ = nullptr;
    AchievementNetwork* achievementNetwork_ = nullptr;
    UpdateService* updateService_ = nullptr;
    QPointer<UpdateDialog> updateDialog_;
    QString availableUpdateVersion_;
    QString availableUpdateNotes_;
    QString updateError_;
    qint64 updateBytesReceived_ = 0;
    qint64 updateBytesTotal_ = 0;
    bool checkingForUpdates_ = false;
    bool updateCheckInteractive_ = false;
    bool installingUpdate_ = false;
    bool restartingUpdate_ = false;
    InputManager* input_ = nullptr;
    NetplayController* netplay_ = nullptr;
    QAction* netplayLeaveAction_ = nullptr;
    ConsoleProfile activeInputProfile_{};
    QTimer* inputTimer_ = nullptr;
    QTimer* volumeSaveTimer_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QMenu* movieMenu_ = nullptr;
    QMenu* videoMenu_ = nullptr;
    QVector<QMenu*> exportBlockedMenus_;
    QMenu* regionMenu_ = nullptr;
    QMenu* gameSpeedMenu_ = nullptr;
    QMenu* discMenu_ = nullptr;
    QMenu* debugMenu_ = nullptr;
    QAction* debugGraphicsAction_ = nullptr;
    QAction* debugAudioAction_ = nullptr;
    DebugGraphicsDialog* debugGraphicsDialog_ = nullptr;
    DebugAudioDialog* debugAudioDialog_ = nullptr;
    QAction* openGameAction_ = nullptr;
    QAction* videoStartAction_ = nullptr;
    QAction* videoExportAction_ = nullptr;
    QAction* videoStopAction_ = nullptr;
    QPointer<QProgressDialog> videoProgress_;
    QAction* movieRecordAction_ = nullptr;
    QAction* movieResumeAction_ = nullptr;
    QAction* moviePlayAction_ = nullptr;
    QAction* movieTakeoverAction_ = nullptr;
    QAction* movieRerecordAction_ = nullptr;
    QAction* movieStopAction_ = nullptr;
    QActionGroup* discGroup_ = nullptr;
    QAction* changeDiscAction_ = nullptr;
    QAction* ejectDiscAction_ = nullptr;
    QAction* closeGameAction_ = nullptr;
    QAction* pauseAction_ = nullptr;
    QAction* frameAdvanceAction_ = nullptr;
    QAction* frameBackAction_ = nullptr;
    QAction* cheatsAction_ = nullptr;
    QAction* cheatSearchAction_ = nullptr;
    QPointer<CheatSearchDialog> cheatSearchDialog_;
    CheatList cheats_;
    AchievementsState achievementsState_;
    bool achievementsHardcore_ = false;
    bool achievementNotifications_ = true;
    bool hideAchievementAvailability_ = true;
    bool achievementSound_ = true;
    int achievementPosition_ = 0;
    Cheats::Platform cheatPlatform_ = Cheats::Platform::MegaDrive;
    bool cheatsSupported_ = false;
    std::array<unsigned,MaxControllerPlayers> padMasks_{};
    std::array<unsigned,3> gunMasks_{};
    std::array<QPointF,3> gunAims_{{QPointF(.5,.5),QPointF(.5,.5),QPointF(.5,.5)}};
    std::array<QPointF,3> mouseMotionRemainder_{};
    std::array<ControllerType,3> presentedControllerKinds_{};
    std::array<bool,3> presentedControllerConnected_{};
    QAction* fastForwardAction_ = nullptr;
    QAction* slowDownAction_ = nullptr;
    QAction* speedUpAction_ = nullptr;
    QAction* normalSpeedAction_ = nullptr;
    QAction* rewindAction_ = nullptr;
    QAction* resetAction_ = nullptr;
    QAction* hardResetAction_ = nullptr;
    std::array<QAction*, 5> blarggActions_{};
    bool gameGearLcdEnabled_ = false;
    QAction* gameGearLcdAction_ = nullptr;
    QAction* shaderOffAction_ = nullptr;
    QAction* shaderDitherAction_ = nullptr;
    QAction* shaderCustomAction_ = nullptr;
    QAction* shaderParametersAction_ = nullptr;
    QAction* screenshotAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* loadAction_ = nullptr;
    QAction* saveFileAction_ = nullptr;
    QAction* loadFileAction_ = nullptr;
    QAction* osdAction_ = nullptr;
    QAction* fpsAction_ = nullptr;
    QAction* gameFpsAction_ = nullptr;
    QAction* fullscreenAction_ = nullptr;
    QAction* muteAction_ = nullptr;
    int volume_ = 80;
    int savedVolume_ = 80;
    QAction* smsFmAction_ = nullptr;
    QStringList recentPaths_;
    QStringList discPaths_;
    int discIndex_ = -1;
    bool discEjected_ = false;
    QVector<QAction*> slotActions_;
    QString currentRom_;
    QString currentGameName_;
    QByteArray windowedGeometry_;
    int stateSlot_ = 0;
    int runTimerMode_ = 0;
    int fastForwardFactor_ = 4;
    int gameSpeedPercent_ = 100; // Retained across games until the app closes.
    bool rewindEnabled_ = true;
    bool rewindAccelerate_ = true;
    int rewindFixedFactor_ = 1;
    bool smsFmEnabled_ = true;
    quint32 debugVideoAvailable_ = 0;
    quint32 debugVideoDisabled_ = 0;
    quint64 debugAudioAvailable_ = 0;
    quint64 debugAudioDisabled_ = 0;
    std::array<unsigned, 38> debugAudioGains_{};
    QColor debugBackdropColor_{255, 0, 255};
    int region_ = 0, blarggFilter_ = 0;
    unsigned occupiedSlots_ = 0;
    bool pauseOnFocusLoss_ = true;
    bool menuPauseActive_ = false;
    bool rewindHeld_ = false, rewindPadHeld_ = false;
    bool frameAdvanceHeld_ = false, frameAdvancePadHeld_ = false;
    bool frameBackHeld_ = false, frameBackPadHeld_ = false;
    bool fastForwardHeld_ = false;
    bool fastForwardPadHeld_ = false;
    bool fastForwardActive_ = false;
#ifdef Q_OS_WIN
    void updateFullscreenComposition();
    void refreshWindowsMenuTheme(bool windowCreated);
    void scheduleWindowsMenuRefresh(bool systemMetricsChanged);
    bool menuRefreshPending_ = false;
    bool menuMetricsDirty_ = false;
    bool fullscreenAltConsumed_ = false;
    bool darkNativeMenu_ = false;
    bool modalWindowBlocked_ = false;
    bool fullscreenMenuShown_ = false;
#endif
    bool wasMaximizedBeforeFullscreen_ = false;
    bool loading_ = false;
    bool applyingSessionControllers_ = false;
    bool videoActive_ = false;
    bool videoPending_ = false;
    bool videoExporting_ = false;
    bool videoStopping_ = false;
    bool closeAfterVideo_ = false;
    bool moviePending_ = false;
    bool closeAfterMovie_ = false;
    int moviePendingMode_ = -1;
    int movieMode_ = 0;
    QString moviePath_;
    bool smokeTest_ = false;
};
