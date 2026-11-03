#pragma once

#include "GameView.h"
#include "input/ControllerTopology.h"
#include "../core/Cheats.h"
#include "../core/Achievements.h"
#include <array>

#include <QColor>
#include <QMainWindow>
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
class QTimer;
class QSlider;
class AudioOutput;
class AchievementNetwork;
class EmulationSession;
class DebugGraphicsDialog;
class CheatSearchDialog;
class DebugAudioDialog;
class InputManager;
class NetplayController;
enum class ConsoleProfile;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(EmulationSession& session, const QString& dataPath,
                        bool smokeTest = false, QWidget* parent = nullptr);
    ~MainWindow() override;
    void openRom(const QString& path);
    void setFullscreenMode(bool enabled);
    void showOsdDemo();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void buildMenus();
    void setSmsFmEnabled(bool enabled);
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
    void editSettings(int initialPage = 0);
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
    void releaseInput();
    void updateMovieActions();
    void setFrameAdvanceSource(bool keyboard, bool pressed);
    QString chooseMovieSavePath(bool branch);
    void updateInputEnabled();
    void updateFastForward();
    void updateRewind();
    void setPausedUi(bool paused);
    void setSlot(int slot);
    void toggleFullscreenMenu();
    void handleInputCommand(int command, bool pressed);

    EmulationSession& session_;
    QSettings settings_;
    QString dataPath_;
    GameView* view_ = nullptr;
    AudioOutput* audio_ = nullptr;
    AchievementNetwork* achievementNetwork_ = nullptr;
    InputManager* input_ = nullptr;
    NetplayController* netplay_ = nullptr;
    QAction* netplayLeaveAction_ = nullptr;
    ConsoleProfile activeInputProfile_{};
    QTimer* inputTimer_ = nullptr;
    QTimer* volumeSaveTimer_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QMenu* movieMenu_ = nullptr;
    QMenu* regionMenu_ = nullptr;
    QMenu* discMenu_ = nullptr;
    QMenu* debugMenu_ = nullptr;
    QAction* debugGraphicsAction_ = nullptr;
    QAction* debugAudioAction_ = nullptr;
    DebugGraphicsDialog* debugGraphicsDialog_ = nullptr;
    DebugAudioDialog* debugAudioDialog_ = nullptr;
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
    QAction* cheatsAction_ = nullptr;
    QAction* cheatSearchAction_ = nullptr;
    QPointer<CheatSearchDialog> cheatSearchDialog_;
    CheatList cheats_;
    AchievementsState achievementsState_;
    bool achievementsHardcore_ = false;
    bool achievementNotifications_ = true;
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
    QAction* rewindAction_ = nullptr;
    QAction* resetAction_ = nullptr;
    QAction* hardResetAction_ = nullptr;
    std::array<QAction*, 5> blarggActions_{};
    QAction* shaderOffAction_ = nullptr;
    QAction* shaderGdaptAction_ = nullptr;
    QAction* shaderCustomAction_ = nullptr;
    QAction* shaderParametersAction_ = nullptr;
    QAction* screenshotAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* loadAction_ = nullptr;
    QAction* osdAction_ = nullptr;
    QAction* fpsAction_ = nullptr;
    QAction* gameFpsAction_ = nullptr;
    QAction* fullscreenAction_ = nullptr;
    QAction* muteAction_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
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
    int fastForwardFactor_ = 4;
    bool rewindEnabled_ = true;
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
    bool fastForwardHeld_ = false;
    bool fastForwardPadHeld_ = false;
    bool fastForwardActive_ = false;
    bool wasMaximizedBeforeFullscreen_ = false;
    bool loading_ = false;
    bool moviePending_ = false;
    bool closeAfterMovie_ = false;
    int moviePendingMode_ = -1;
    int movieMode_ = 0;
    QString moviePath_;
    bool smokeTest_ = false;
};
