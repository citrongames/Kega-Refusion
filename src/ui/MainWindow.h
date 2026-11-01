#pragma once

#include "GameView.h"

#include <QMainWindow>
#include <QSettings>
#include <QStringList>
#include <QVector>

class QAction;
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
    void updateRecentMenu();
    void editControls();
    void releaseInput();
    void updateInputEnabled();
    void updateFastForward();
    void setPausedUi(bool paused);
    void setSlot(int slot);
    void toggleFullscreenMenu();
    void handleInputCommand(int command, bool pressed);

    EmulationSession& session_;
    QSettings settings_;
    GameView* view_ = nullptr;
    AudioOutput* audio_ = nullptr;
    InputManager* input_ = nullptr;
    QTimer* inputTimer_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QAction* pauseAction_ = nullptr;
    QAction* fastForwardAction_ = nullptr;
    QAction* resetAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* loadAction_ = nullptr;
    QAction* fullscreenAction_ = nullptr;
    QAction* muteAction_ = nullptr;
    QStringList recentPaths_;
    QVector<QAction*> slotActions_;
    QString currentRom_;
    QByteArray windowedGeometry_;
    int stateSlot_ = 0;
    bool fastForwardHeld_ = false;
    bool fastForwardPadHeld_ = false;
    bool fastForwardActive_ = false;
    bool wasMaximizedBeforeFullscreen_ = false;
    bool loading_ = false;
    bool smokeTest_ = false;
};
