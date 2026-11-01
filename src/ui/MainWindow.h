#pragma once

#include "GameView.h"

#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QSettings>

class QAction;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QMenu;
class AudioOutput;
class EmulationSession;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(EmulationSession& session, const QString& dataPath,
                        bool smokeTest = false, QWidget* parent = nullptr);
    void openRom(const QString& path);

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
    void updateFastForward();
    void setPausedUi(bool paused);

    EmulationSession& session_;
    QSettings settings_;
    GameView* view_ = nullptr;
    AudioOutput* audio_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QAction* pauseAction_ = nullptr;
    QAction* fastForwardAction_ = nullptr;
    QHash<int, unsigned> keyToButton_;
    QSet<int> heldKeys_;
    QStringList recentPaths_;
    QString currentRom_;
    int stateSlot_ = 0;
    bool fastForwardHeld_ = false;
    bool smokeTest_ = false;
};
