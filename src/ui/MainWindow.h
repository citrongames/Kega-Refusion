#pragma once

#include "GameView.h"

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
class EmulationSession;
class InputManager;

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
    void saveScreenshot();
    void updateRecentMenu();
    void editSegaCdBios();
    void editSega32xBios();
    void updateDiscMenu();
    void editSettings();
    void editControls();
    bool applyShaderPreset(const QString& path);
    void editShaderParameters();
    void updateShaderActions();
    void releaseInput();
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
    InputManager* input_ = nullptr;
    QTimer* inputTimer_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QMenu* discMenu_ = nullptr;
    QActionGroup* discGroup_ = nullptr;
    QAction* changeDiscAction_ = nullptr;
    QAction* ejectDiscAction_ = nullptr;
    QAction* pauseAction_ = nullptr;
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
    int rewindSeconds_ = 30;
    int region_ = 0, blarggFilter_ = 0;
    unsigned occupiedSlots_ = 0;
    bool pauseOnFocusLoss_ = true;
    bool rewindHeld_ = false, rewindPadHeld_ = false;
    bool fastForwardHeld_ = false;
    bool fastForwardPadHeld_ = false;
    bool fastForwardActive_ = false;
    bool wasMaximizedBeforeFullscreen_ = false;
    bool loading_ = false;
    bool smokeTest_ = false;
};
