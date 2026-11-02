#pragma once

#include "../input/InputManager.h"

#include <QWidget>
#include <QMap>
#include <QSet>
#include <QVector>

class QHideEvent;
class QCheckBox;
class QComboBox;
class QKeyEvent;
class QKeySequence;
class QLabel;
class QPushButton;
class QSlider;
class QTableWidget;
class QTabWidget;
class ControllerDiagram;

class ControlsPage final : public QWidget {
    Q_OBJECT
public:
    explicit ControlsPage(InputManager& input, QWidget* parent = nullptr);
    ~ControlsPage() override;
    bool apply();
    void cancelCapture();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    ConsoleProfile selectedProfile() const;
    int selectedPlayer() const;
    int draftKey() const;
    PlayerMapping& draft();
    QVector<int> activeGameTargets();
    void refreshPlayerChoices();
    QString captureDeviceKey();
    void refreshDevices();
    void refreshContents();
    void fillGameTable();
    void fillCommandTable();
    void selectTarget(int target);
    void refreshPadCell(int target);
    void updateActionButtons();
    void setFeedback(const QString& message, bool warning = false);
    void onKeyboardChanged(int target, const QKeySequence& sequence);
    void beginCaptureFor(int target);
    void startSequence();
    void advanceSequence();
    void skipCapture();
    void stopCapture();
    void onCaptured(const PadBinding& binding);
    void onCaptureCancelled();
    bool validateDrafts();

    InputManager& input_;
    QComboBox* profileBox_ = nullptr;
    QComboBox* playerBox_ = nullptr;
    QComboBox* deviceBox_ = nullptr;
    QLabel* deviceTypeLabel_ = nullptr;
    QComboBox* deviceTypeBox_ = nullptr;
    QCheckBox* showCrosshairBox_ = nullptr;
    QCheckBox* secondJustifierBox_ = nullptr;
    QLabel* aimSourceLabel_ = nullptr;
    QComboBox* aimSourceBox_ = nullptr;
    QLabel* aimHint_ = nullptr;
    QLabel* controllerTypeLabel_ = nullptr;
    QComboBox* controllerTypeBox_ = nullptr;
    QSlider* deadzoneSlider_ = nullptr;
    QLabel* deadzoneValue_ = nullptr;
    QLabel* feedback_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTableWidget* gameTable_ = nullptr;
    QTableWidget* commandTable_ = nullptr;
    ControllerDiagram* diagram_ = nullptr;
    QPushButton* assignButton_ = nullptr;
    QPushButton* sequenceButton_ = nullptr;
    QPushButton* skipButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QMap<int, PlayerMapping> drafts_;
    QSet<int> dirty_;
    QMap<QString, QString> knownDevices_;
    QVector<int> sequence_;
    int sequenceIndex_ = 0;
    int captureTarget_ = -1;
    int selectedTarget_ = -1;
    bool refreshing_ = false;
    bool skipping_ = false;
};
