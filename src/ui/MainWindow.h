#pragma once

#include "GameView.h"
#include "../core/Cheats.h"
#include "../core/Achievements.h"
#include <array>

#include <QMainWindow>
#include <QSettings>
#include <QStringList>
#include <QVector>

class QAction;
class QActionGroup;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QMenu;
class QTimer;
class AudioOutput;
class AchievementNetwork;
class EmulationSession;
class InputManager;
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
    void openGameDialog();
    void setLoadingUi(bool loading, const QString& fileName = {});
    void saveScreenshot();
    void updateRecentMenu();
    void updateDiscMenu();
    void editSettings(int initialPage = 0);
    void editControls();
    void editCheats();
    void showAchievements();
    void updateGunConfiguration();
    void updateGunAim(unsigned player, QPointF position, bool offscreen);
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
    ConsoleProfile activeInputProfile_{};
    QTimer* inputTimer_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QMenu* movieMenu_ = nullptr;
    QMenu* regionMenu_ = nullptr;
    QMenu* blarggMenu_ = nullptr;
    QMenu* discMenu_ = nullptr;
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
    CheatList cheats_;
    AchievementsState achievementsState_;
    bool achievementsHardcore_ = false;
    bool achievementNotifications_ = true;
    bool achievementSound_ = true;
    int achievementPosition_ = 0;
    Cheats::Platform cheatPlatform_ = Cheats::Platform::MegaDrive;
    bool cheatsSupported_ = false;
    std::array<unsigned,3> padMasks_{}, gunMasks_{};
    std::array<QPointF,3> gunAims_{{QPointF(.5,.5),QPointF(.5,.5),QPointF(.5,.5)}};
    QAction* fastForwardAction_ = nullptr;
    QAction* rewindAction_ = nullptr;
    QAction* resetAction_ = nullptr;
    QAction* hardResetAction_ = nullptr;
    QAction* shaderOffAction_ = nullptr;
    QAction* shaderGdaptAction_ = nullptr;
    QAction* shaderCustomAction_ = nullptr;
    QAction* shaderParametersAction_ = nullptr;
    QAction* screenshotAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* loadAction_ = nullptr;
    QAction* osdAction_ = nullptr;
    QAction* fullscreenAction_ = nullptr;
    QAction* muteAction_ = nullptr;
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
    int region_ = 0, blarggFilter_ = 0;
    unsigned occupiedSlots_ = 0;
    bool pauseOnFocusLoss_ = true;
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
