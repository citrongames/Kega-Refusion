#pragma once
#include "ControllerType.h"
#include "ControllerTopology.h"
#include <QByteArray>
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
                    PreviousSlotCommand, MenuCommand, RewindCommand, HardResetCommand, FrameAdvanceCommand,
                    FrameBackCommand, SpeedDecreaseCommand, SpeedIncreaseCommand, SpeedResetCommand };
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
    bool connected = true; // Virtual port or chained Justifier; defaults disable ports after A.
    bool sixButton = true; // Mega Drive pad type; bindings survive device changes.
    ControllerType controllerType = ControllerType::Pad;
    AimSource aimSource = AimSource::Mouse; // The chained second Justifier defaults to a stick.
    bool showCrosshair = true;
    QString deviceKey = QStringLiteral("auto"); // auto, none, or persistent device key
    QMap<int,PadBinding> pad;
    QMap<int,int> keyboard; // target -> US physical key position expressed as a Qt key
    QMap<int,QMap<int,int>> mouse; // controller type -> target -> Qt::MouseButton (0 = unbound)
    int deadzonePercent = 25;
    int analogSensitivity = 100; // MD Mouse stick speed, percent (25..400).
};
class InputManager final : public QObject {
    Q_OBJECT
public:
    explicit InputManager(const QString& settingsPath, QObject* parent=nullptr);
    ~InputManager() override;
    QVector<InputDevice> devices() const;
    PlayerMapping mapping(ConsoleProfile profile, int player) const;
    PlayerMapping ordinaryMapping(ConsoleProfile profile, int player) const;
    void setMapping(ConsoleProfile profile, int player, const PlayerMapping& mapping);
    static PlayerMapping defaultMapping(ConsoleProfile profile, int player);
    static QString bindingName(const PadBinding& binding);
    // Compare at capture time, including the selected device's automatic mapping.
    bool capturedBindingConflicts(ConsoleProfile profile, const QString& deviceKey, int target,
                                  const PadBinding& configured, const PadBinding& captured) const;
    static QString profileName(ConsoleProfile profile);
    static QVector<int> gameTargets(ConsoleProfile profile, bool sixButton = true,
                                    ControllerType type = ControllerType::Pad);
    static QString targetName(ConsoleProfile profile, int target,
                              ControllerType type = ControllerType::Pad);
    bool jcart() const;
    unsigned jcartPlayerBase() const;
    void setJCart(bool enabled); // transient cartridge hardware, never persisted
    ControllerAdapter adapter() const;
    ControllerAdapter adapter(ConsoleProfile profile) const;
    bool adapterConnection(ConsoleProfile profile, unsigned player) const;
    void setAdapter(ConsoleProfile profile, ControllerAdapter adapter);
    void setProfile(ConsoleProfile profile);
    void setMovieControllers(const QByteArray& controllersJson);
    ConsoleProfile profile() const;
    bool isConnected(unsigned player) const;
    bool sixButton(unsigned player) const;
    ControllerType controllerType(unsigned player) const;
    bool aimWithMouse(unsigned player) const;
    unsigned mouseButtonMask(unsigned player, unsigned qtButtons) const;
    void setNetworkInputSourceEnabled(bool enabled); // transient local P1 pad source for online pad roles
    void setEnabled(bool enabled); // foreground keyboard, pad commands, and mouse input
    bool backgroundGamepadEnabled() const;
    void setBackgroundGamepadEnabled(bool enabled); // persistent preference
    bool mouseCaptureEnabled() const;
    void setMouseCaptureEnabled(bool enabled); // persistent preference
    void setBackgroundGamepadActive(bool active); // MainWindow's game/modal focus gate
    static int keyboardKey(int logicalKey, quint32 nativeScanCode = 0,
                           KeyboardScanCodes codes = KeyboardScanCodes::Native);
    static bool reservedKeyboardKey(int key);
    bool keyEvent(int key, bool pressed);
    void releaseKeyboard();
    void beginCapture(const QString& deviceKey, int player); // arm each input independently; ignore a previously held input
    void cancelCapture();
    bool capturing() const;
    void poll(); // timer calls it; explicit for deterministic virtual joystick tests
signals:
    void devicesChanged();
    void adapterChanged(int adapter);
    void jcartChanged(bool enabled);
    void maskChanged(unsigned player, unsigned mask);
    void connectionChanged(unsigned player, bool connected);
    void controllerTypeChanged(unsigned player, bool sixButton);
    void deviceTypeChanged(unsigned player, int type);
    void configurationChanged();
    void backgroundGamepadChanged(bool enabled);
    void mouseCaptureChanged(bool enabled);
    void mouseMotion(unsigned player, double dx, double dy); // relative core pixels from a stick
    void paddleMoved(unsigned player, int position); // signed absolute X, -32768..32767
    void aimMoved(unsigned player, double dx, double dy); // screen fractions, includes elapsed time
    void commandChanged(int command, bool pressed);
    void captureReady(); // ready for a fresh input; other held controls do not block it
    void bindingCaptured(const PadBinding& binding);
    void captureCancelled();
    void notification(const QString& message);
    void previewChanged(unsigned player, unsigned mask);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
