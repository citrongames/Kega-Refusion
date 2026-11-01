#pragma once
#include <QObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>
#include <memory>

// Targets 0..15 are libretro joypad IDs. Commands are separate from game inputs.
// Qt uses Windows Set-1 codes or XKB keycodes (evdev + 8) on Linux.
enum class KeyboardScanCodes { Native, Windows, Xkb, Unavailable };
enum class ConsoleProfile { MegaDrive, MasterSystem, GameGear, SG1000 };
enum InputCommand { PauseCommand=16, FastForwardCommand, SaveCommand, LoadCommand,
                    ResetCommand, FullscreenCommand, MuteCommand, NextSlotCommand,
                    PreviousSlotCommand, MenuCommand, RewindCommand };
struct PadBinding {
    enum Kind { None, Button, Axis, Hat, Automatic };
    Kind kind = None;
    int index = 0;
    int direction = 0; // axis: -1/+1, hat: SDL hat direction bit.
    bool operator==(const PadBinding& b) const { return kind==b.kind && index==b.index && direction==b.direction; }
};
Q_DECLARE_METATYPE(PadBinding)
struct InputDevice { int instanceId=-1; QString key; QString name; };
struct PlayerMapping {
    QString deviceKey = QStringLiteral("auto"); // auto, none, or persistent device key
    QMap<int,PadBinding> pad;
    QMap<int,int> keyboard; // target -> US physical key position expressed as a Qt key
    int deadzonePercent = 25;
};
class InputManager final : public QObject {
    Q_OBJECT
public:
    explicit InputManager(const QString& settingsPath, QObject* parent=nullptr);
    ~InputManager() override;
    QVector<InputDevice> devices() const;
    PlayerMapping mapping(ConsoleProfile profile, int player) const;
    void setMapping(ConsoleProfile profile, int player, const PlayerMapping& mapping);
    static PlayerMapping defaultMapping(ConsoleProfile profile, int player);
    static QString bindingName(const PadBinding& binding);
    static QString profileName(ConsoleProfile profile);
    static QVector<int> gameTargets(ConsoleProfile profile);
    static QString targetName(ConsoleProfile profile, int target);
    void setProfile(ConsoleProfile profile);
    ConsoleProfile profile() const;
    void setEnabled(bool enabled); // release all outputs when unfocused or in modal dialogs
    static int keyboardKey(int logicalKey, quint32 nativeScanCode = 0,
                           KeyboardScanCodes codes = KeyboardScanCodes::Native);
    static bool reservedKeyboardKey(int key);
    bool keyEvent(int key, bool pressed);
    void releaseKeyboard();
    void beginCapture(const QString& deviceKey, int player); // wait for neutral before accepting
    void cancelCapture();
    bool capturing() const;
    void poll(); // timer calls it; explicit for deterministic virtual joystick tests
signals:
    void devicesChanged();
    void maskChanged(unsigned player, unsigned mask);
    void commandChanged(int command, bool pressed);
    void bindingCaptured(const PadBinding& binding);
    void captureCancelled();
    void notification(const QString& message);
    void previewChanged(unsigned player, unsigned mask);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
