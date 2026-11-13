#include "i18n/Localization.h"
#include "InputManager.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QKeyEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace {
constexpr int playerCount = int(MaxControllerPlayers); // Slot 2 also stores the legacy chained Justifier.
constexpr int targetCount = ScreenshotCommand + 1;
constexpr int captureMilliseconds = 10000;
constexpr int captureAxisThreshold = 16000;

constexpr int mdTargets[] = {4, 5, 6, 7, 1, 0, 8, 10, 9, 11, 3, 2};
constexpr int md3Targets[] = {4, 5, 6, 7, 1, 0, 8, 3};
constexpr int smsTargets[] = {4, 5, 6, 7, 0, 8, 3};
constexpr int sgTargets[] = {4, 5, 6, 7, 0, 8};
constexpr int phaserTargets[] = {1, 2};
constexpr int menacerTargets[] = {1, 0, 8, 3, 2};
constexpr int justifierTargets[] = {1, 3, 2};
constexpr int mouseTargets[] = {1, 0, 8, 3};
constexpr int paddleTargets[] = {0};
ControllerType validControllerType(ConsoleProfile profile, ControllerType type) {
    if (profile == ConsoleProfile::MegaDrive &&
        (type == ControllerType::Menacer || type == ControllerType::Justifier ||
         type == ControllerType::Mouse)) return type;
    if (profile == ConsoleProfile::MasterSystem &&
        (type == ControllerType::LightPhaser || type == ControllerType::Paddle)) return type;
    return ControllerType::Pad;
}
struct TargetRange {
    const int* first;
    std::size_t count;
    const int* begin() const { return first; }
    const int* end() const { return first + count; }
};
TargetRange gameTargetRange(ConsoleProfile profile, bool sixButton = true,
                            ControllerType type = ControllerType::Pad) {
    switch (validControllerType(profile, type)) {
    case ControllerType::LightPhaser: return {phaserTargets, 2};
    case ControllerType::Menacer: return {menacerTargets, 5};
    case ControllerType::Justifier: return {justifierTargets, 3};
    case ControllerType::Mouse: return {mouseTargets, 4};
    case ControllerType::Paddle: return {paddleTargets, 1};
    case ControllerType::Pad: break;
    }
    if (profile == ConsoleProfile::MegaDrive)
        return sixButton ? TargetRange{mdTargets, 12} : TargetRange{md3Targets, 8};
    if (profile == ConsoleProfile::SG1000) return {sgTargets, 6};
    return {smsTargets, 7};
}

bool triggerAxis(SDL_GameController* controller, int axis) {
    if (!controller) return false;
    for (const auto trigger : {SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT}) {
        const auto binding = SDL_GameControllerGetBindForAxis(controller, trigger);
        if (binding.bindType == SDL_CONTROLLER_BINDTYPE_AXIS && binding.value.axis == axis) return true;
    }
    return false;
}


QString profileId(ConsoleProfile profile) {
    switch (profile) {
    case ConsoleProfile::MegaDrive: return QStringLiteral("md");
    case ConsoleProfile::MasterSystem: return QStringLiteral("sms");
    case ConsoleProfile::GameGear: return QStringLiteral("gg");
    case ConsoleProfile::SG1000: return QStringLiteral("sg");
    }
    return QStringLiteral("md");
}
int profileIndex(ConsoleProfile profile) { return int(profile); }
PadBinding button(int index) { return {PadBinding::Button, index, 0}; }
PadBinding axis(int index, int direction) { return {PadBinding::Axis, index, direction}; }
PadBinding hat(int index, int direction) { return {PadBinding::Hat, index, direction}; }
PadBinding automatic(int target) { return {PadBinding::Automatic, target, 0}; }
PadBinding rawDefault(int target) {
    switch (target) {
    case 4: return hat(0, SDL_HAT_UP);
    case 5: return hat(0, SDL_HAT_DOWN);
    case 6: return hat(0, SDL_HAT_LEFT);
    case 7: return hat(0, SDL_HAT_RIGHT);
    case 0: return button(0);
    case 8: return button(1);
    case 1: return button(2);
    case 10: return button(3);
    case 9: return button(4);
    case 11: return button(5);
    case 2: return button(6);
    case 3: return button(7);
    default: return {};
    }
}
QString bindingText(const PadBinding& b) {
    switch (b.kind) {
    case PadBinding::Button: return QStringLiteral("b:%1").arg(b.index);
    case PadBinding::Axis: return QStringLiteral("a:%1:%2").arg(b.index).arg(b.direction);
    case PadBinding::Hat: return QStringLiteral("h:%1:%2").arg(b.index).arg(b.direction);
    case PadBinding::Automatic: return QStringLiteral("auto");
    case PadBinding::None: return QStringLiteral("n");
    }
    return QStringLiteral("n");
}
PadBinding parseBinding(const QString& text) {
    if (text == "auto") return automatic(0);
    const auto parts = text.split(':');
    bool okay = false;
    if (parts.size() == 2 && parts[0] == "b") {
        const int index = parts[1].toInt(&okay);
        if (okay && index >= 0) return button(index);
    }
    if (parts.size() == 3) {
        const int index = parts[1].toInt(&okay);
        if (!okay || index < 0) return {};
        const int direction = parts[2].toInt(&okay);
        if (!okay) return {};
        if (parts[0] == "a" && (direction == -1 || direction == 1)) return axis(index, direction);
        if (parts[0] == "h" && (direction == SDL_HAT_UP || direction == SDL_HAT_DOWN ||
                                 direction == SDL_HAT_LEFT || direction == SDL_HAT_RIGHT)) return hat(index, direction);
    }
    return {};
}
QString settingsPrefix(ConsoleProfile profile, int player) {
    return QStringLiteral("input/%1/player%2/").arg(profileId(profile)).arg(player + 1);
}
bool gameplayTarget(int target) { return target >= 0 && target < 16; }
bool validPlayer(int player) { return player >= 0 && player < playerCount; }
ControllerAdapter allowedAdapter(ConsoleProfile profile, ControllerAdapter adapter) {
    if (!validControllerAdapter(int(adapter))) return ControllerAdapter::None;
    if (profile == ConsoleProfile::MegaDrive && int(adapter) >= 1 && int(adapter) <= 4) return adapter;
    if (profile == ConsoleProfile::MasterSystem && isMasterTap(adapter)) return adapter;
    return ControllerAdapter::None;
}
QString adapterSetting(ConsoleProfile profile) {
    return QStringLiteral("input/%1/adapter").arg(profileId(profile));
}
ControllerType slotControllerType(ConsoleProfile profile, int player, ControllerType type) {
    if (profile == ConsoleProfile::MegaDrive && player == 0)
        return type == ControllerType::Mouse ? type : ControllerType::Pad;
    if (player == 2) return profile == ConsoleProfile::MegaDrive
        ? ControllerType::Justifier : ControllerType::Pad;
    if (player >= 3) return ControllerType::Pad;
    return validControllerType(profile, type);
}
bool validMouseButton(int button) {
    return button == 0 || button == int(Qt::LeftButton) || button == int(Qt::RightButton) ||
           button == int(Qt::MiddleButton) || button == int(Qt::XButton1) ||
           button == int(Qt::XButton2);
}
constexpr int keypadKey(int key) { return int(Qt::KeypadModifier) | key; }

int legacyKeyboardKey(int key) {
    // Qt used Key_Enter specifically for keypad Enter before we stored its location.
    if (key == Qt::Key_Enter) return keypadKey(Qt::Key_Enter);
    // Old settings stored the active layout's Unicode key. Keep those bindings
    // usable at their original Russian/English key positions after upgrading.
    if (key >= 'a' && key <= 'z') key -= 'a' - 'A';
    if (key >= 0x0430 && key <= 0x044f) key -= 0x20;
    if (key == 0x0451) key = 0x0401;
    if (key < 0x0400 || key > 0x04ff) return key;
    const QString russian = QStringLiteral("ЁЙЦУКЕНГШЩЗХЪФЫВАПРОЛДЖЭЯЧСМИТЬБЮ");
    const QString physical = QStringLiteral("`QWERTYUIOP[]ASDFGHJKL;'ZXCVBNM,.");
    const int index = russian.indexOf(QChar(key));
    return index >= 0 ? physical[index].unicode() : key;
}
int storedKeyboardKey(int key) {
    key = legacyKeyboardKey(key);
    return InputManager::reservedKeyboardKey(key) ? 0 : key;
}
constexpr int commandModifierMask = Qt::ShiftModifier | Qt::ControlModifier |
                                    Qt::AltModifier | Qt::MetaModifier;
int normalizedCommandKey(int chord) {
    if (!chord) return 0;
    int modifiers = chord & commandModifierMask;
    if (chord & Qt::GroupSwitchModifier) modifiers |= Qt::AltModifier;
    int key = chord & ~(commandModifierMask | int(Qt::GroupSwitchModifier) | int(Qt::KeypadModifier));
    if (key == Qt::Key_Backtab) {
        key = Qt::Key_Tab;
        modifiers |= Qt::ShiftModifier;
    }
    if (key == Qt::Key_unknown || key == Qt::Key_Control || key == Qt::Key_Shift ||
        key == Qt::Key_Alt || key == Qt::Key_AltGr || key == Qt::Key_Meta || key <= 0)
        return 0;
    key = legacyKeyboardKey(key);
    return modifiers | (chord & Qt::KeypadModifier) | key;
}
bool reservedCommandChord(int chord) {
    const bool keypad = (chord & Qt::KeypadModifier) != 0;
    const int key = chord & ~(commandModifierMask | int(Qt::KeypadModifier));
    const int modifiers = chord & commandModifierMask;
    if (!keypad && modifiers == 0 &&
        ((key >= Qt::Key_0 && key <= Qt::Key_9) || key == Qt::Key_Escape))
        return true;
    if (!keypad && modifiers == Qt::ControlModifier &&
        (key == Qt::Key_O || key == Qt::Key_Q)) return true;
    if (!keypad && modifiers == Qt::AltModifier && key == Qt::Key_F4) return true;
    if (keypad) return false;
    // These physical combinations control the fixed master volume.
    if ((key == Qt::Key_Equal || key == Qt::Key_Plus) &&
        (modifiers == 0 || modifiers == Qt::ShiftModifier)) return true;
    return key == Qt::Key_Minus && modifiers == 0;
}
QString guidString(SDL_Joystick* joystick) {
    char text[33]{};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), text, sizeof(text));
    return QString::fromLatin1(text);
}
SDL_GameControllerButton controllerButton(ConsoleProfile profile, int target) {
    if (profile == ConsoleProfile::MasterSystem && target == 1)
        return SDL_CONTROLLER_BUTTON_A; // Light Phaser trigger.
    switch (target) {
    case 4: return SDL_CONTROLLER_BUTTON_DPAD_UP;
    case 5: return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    case 6: return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    case 7: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    case 0: return SDL_CONTROLLER_BUTTON_A;
    case 8: return SDL_CONTROLLER_BUTTON_B;
    case 3: return SDL_CONTROLLER_BUTTON_START;
    case 1: return SDL_CONTROLLER_BUTTON_X;
    case 2: return SDL_CONTROLLER_BUTTON_BACK;
    case 10: return SDL_CONTROLLER_BUTTON_Y;
    case 9: return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    case 11: return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    default: return SDL_CONTROLLER_BUTTON_INVALID;
    }
}
int dpadDirection(int target) {
    switch (target) {
    case 4: return -1;
    case 5: return 1;
    case 6: return -1;
    case 7: return 1;
    default: return 1;
    }
}
}

struct InputManager::Impl {
    struct Device {
        SDL_Joystick* joystick = nullptr;
        SDL_GameController* controller = nullptr;
        int instanceId = -1;
        QString key, name, guid;
        QVector<int> captureNeutralAxes;
        ~Device() { if (controller) SDL_GameControllerClose(controller); else if (joystick) SDL_JoystickClose(joystick); }
    };
    InputManager* owner;
    QString settingsPath;
    std::array<std::array<PlayerMapping, playerCount>, 4> mappings;
    std::array<PlayerMapping, playerCount> movieMappings;
    std::array<ControllerAdapter, 4> adapters{};
    std::array<std::array<bool, playerCount>, 4> adapterConnections{};
    ControllerAdapter movieAdapter = ControllerAdapter::None;
    bool jcartActive = false;
    bool movieJCart = false;
    unsigned movieJCartBase = 0;
    bool effectiveJCart() const {
        if (profile != ConsoleProfile::MegaDrive) return false;
        return movieControllersActive && profile == movieProfile ? movieJCart : jcartActive;
    }
    ControllerAdapter effectiveAdapter() const {
        if (movieControllersActive && profile == movieProfile) return movieAdapter;
        const ControllerAdapter selected = adapters[profileIndex(profile)];
        return effectiveJCart() && adapterPlayerCount(selected) == MaxControllerPlayers
            ? ControllerAdapter::None : selected;
    }
    unsigned effectiveJCartBase() const {
        if (!effectiveJCart()) return 0;
        if (movieControllersActive && profile == movieProfile) return movieJCartBase;
        if (effectiveAdapter() != ControllerAdapter::None)
            return adapterPlayerCount(effectiveAdapter());
        const auto& first = mappings[profileIndex(profile)][1];
        const auto& second = mappings[profileIndex(profile)][2];
        return first.connected && first.controllerType == ControllerType::Justifier &&
               second.connected ? 3u : 2u;
    }
    bool jcartPlayer(int player) const {
        const unsigned base = effectiveJCartBase();
        return base && player >= int(base) && player < int(base + 2);
    }
    ControllerType effectiveType(int player) const {
        return effectiveAdapter() != ControllerAdapter::None || jcartPlayer(player)
            ? ControllerType::Pad : effective(player).controllerType;
    }
    ControllerType inputSourceType(int player) const {
        return networkInputSourceEnabled && player == 0
            ? ControllerType::Pad : effectiveType(player);
    }
    ConsoleProfile movieProfile = ConsoleProfile::MegaDrive;
    bool movieControllersActive = false;
    const PlayerMapping& effective(int player) const {
        return movieControllersActive && profile == movieProfile
            ? movieMappings[player] : mappings[profileIndex(profile)][player];
    }
    std::vector<std::unique_ptr<Device>> devices;
    QSet<int> keys;
    std::array<std::array<std::array<bool, 2>, targetCount>, playerCount> axisHeld{};
    ConsoleProfile profile = ConsoleProfile::MegaDrive;
    bool sdlReady = false;
    bool initialScan = true;
    bool enabled = true;
    bool networkInputSourceEnabled = false;
    bool inputSourceConnected(int player) const {
        return connected(player) || (networkInputSourceEnabled && player == 0);
    }
    bool backgroundGamepadEnabled = false;
    bool mouseCaptureEnabled = true;
    bool backgroundGamepadActive = false;
    std::array<unsigned, playerCount> masks{};
    std::array<unsigned, playerCount> previews{};
    bool legacyPortOneGun = false;
    bool legacyGunMoved = false;
    bool legacyGunMovedToChain = false;
    QMap<int,int> commandKeys;
    QMap<int,int> commandKeysSecondary;
    std::array<bool, targetCount - 16> commands{};
    std::array<bool, targetCount - 16> commandBlocked{};
    bool capture = false;
    int captureInstance = -1, capturePlayer = -1;
    QElapsedTimer captureTimer;
    QElapsedTimer aimTimer;
    std::array<int, playerCount> paddlePositions{};
    std::array<bool, playerCount> paddlePositionValid{};
    QVector<int> neutralAxes, captureAxesStart;
    QVector<quint8> captureButtonsBlocked, captureHatsBlocked, captureAxisNeutralPolls;

    explicit Impl(InputManager* parent, QString path) : owner(parent), settingsPath(std::move(path)) {
        commandBlocked.fill(true);
        for (int p = 0; p < 4; ++p)
            for (int player = 0; player < playerCount; ++player)
                mappings[p][player] = InputManager::defaultMapping(ConsoleProfile(p), player);
        QSettings settings(settingsPath, QSettings::IniFormat);
        for (int p = 0; p < 4; ++p) {
            const auto profile = ConsoleProfile(p);
            adapters[p] = allowedAdapter(profile, ControllerAdapter(settings.value(
                adapterSetting(profile), 0).toInt()));
        }
        backgroundGamepadEnabled = settings.value(QStringLiteral("input/backgroundGamepad"), false).toBool();
        mouseCaptureEnabled = settings.value(QStringLiteral("input/mouseCapture"), true).toBool();
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
                    backgroundGamepadEnabled ? "1" : "0");
        for (int p = 0; p < 4; ++p) for (int player = 0; player < playerCount; ++player) {
            auto& m = mappings[p][player];
            const QString prefix = settingsPrefix(ConsoleProfile(p), player);
            m.deviceKey = settings.value(prefix + "device", m.deviceKey).toString();
            adapterConnections[p][player] = settings.value(
                QStringLiteral("input/%1/adapterPlayer%2/connected")
                    .arg(profileId(ConsoleProfile(p))).arg(player + 1), true).toBool();
            m.connected = settings.value(prefix + "connected", m.connected).toBool();
            m.sixButton = settings.value(prefix + "sixButton", m.sixButton).toBool();
            m.controllerType = validControllerType(ConsoleProfile(p), static_cast<ControllerType>(
                settings.value(prefix + "controllerType", int(m.controllerType)).toInt()));
            m.aimSource = settings.value(prefix + "aimSource", int(m.aimSource)).toInt() == int(AimSource::LeftStick)
                ? AimSource::LeftStick : AimSource::Mouse;
            m.showCrosshair = settings.value(prefix + "showCrosshair", true).toBool();
            m.deadzonePercent = std::clamp(settings.value(prefix + "deadzone", m.deadzonePercent).toInt(), 5, 60);
            m.analogSensitivity = std::clamp(settings.value(prefix + "analogSensitivity", 100).toInt(), 25, 400);
            for (int target = 0; target < targetCount; ++target) {
                const QString key = prefix + QStringLiteral("pad/%1").arg(target);
                if (settings.contains(key)) {
                    m.pad[target] = parseBinding(settings.value(key).toString());
                    if (m.pad[target].kind == PadBinding::Automatic) m.pad[target] = automatic(target);
                }
                const QString keyKey = prefix + QStringLiteral("key/%1").arg(target);
                if (gameplayTarget(target) && settings.contains(keyKey)) m.keyboard[target] = storedKeyboardKey(settings.value(keyKey).toInt());
                const QString secondPad = prefix + QStringLiteral("padSecondary/%1").arg(target);
                if (settings.contains(secondPad)) {
                    m.padSecondary[target] = parseBinding(settings.value(secondPad).toString());
                    if (m.padSecondary[target].kind == PadBinding::Automatic) m.padSecondary[target] = automatic(target);
                }
                const QString secondKey = prefix + QStringLiteral("keySecondary/%1").arg(target);
                if (gameplayTarget(target) && settings.contains(secondKey))
                    m.keyboardSecondary[target] = storedKeyboardKey(settings.value(secondKey).toInt());
            }
            for (ControllerType type : {ControllerType::LightPhaser, ControllerType::Menacer,
                                        ControllerType::Justifier, ControllerType::Mouse,
                                        ControllerType::Paddle}) {
                if (validControllerType(ConsoleProfile(p), type) != type) continue;
                for (int target : gameTargetRange(ConsoleProfile(p), true, type)) {
                    const QString key = prefix + QStringLiteral("mouse/%1/%2").arg(int(type)).arg(target);
                    if (settings.contains(key)) {
                        const int button = settings.value(key).toInt();
                        if (validMouseButton(button)) m.mouse[int(type)][target] = button;
                    }
                    const QString secondKey = prefix + QStringLiteral("mouseSecondary/%1/%2").arg(int(type)).arg(target);
                    if (settings.contains(secondKey)) {
                        const int button = settings.value(secondKey).toInt();
                        if (validMouseButton(button)) m.mouseSecondary[int(type)][target] = button;
                    }
                }
            }
            if (!settings.contains(prefix + "device")) {
                for (int target : InputManager::gameTargets(ConsoleProfile(p))) {
                    const QString legacy = QStringLiteral("keys/button_%1").arg(target);
                    if (settings.contains(legacy)) m.keyboard[target] = storedKeyboardKey(settings.value(legacy).toInt());
                }
            }
        }
        auto& mdPortA = mappings[profileIndex(ConsoleProfile::MegaDrive)][0];
        if (isLightgun(mdPortA.controllerType)) {
            legacyPortOneGun = true;
            auto& mdPortB = mappings[profileIndex(ConsoleProfile::MegaDrive)][1];
            const auto defaultB = InputManager::defaultMapping(ConsoleProfile::MegaDrive, 1);
            const bool portBIsDefault = mdPortB.connected == defaultB.connected &&
                mdPortB.sixButton == defaultB.sixButton &&
                mdPortB.controllerType == defaultB.controllerType &&
                (mdPortB.aimSource == defaultB.aimSource ||
                 (mdPortB.controllerType == ControllerType::Pad &&
                  mdPortB.aimSource == AimSource::LeftStick)) &&
                mdPortB.showCrosshair == defaultB.showCrosshair &&
                mdPortB.deviceKey == defaultB.deviceKey &&
                mdPortB.deadzonePercent == defaultB.deadzonePercent &&
                mdPortB.analogSensitivity == defaultB.analogSensitivity &&
                mdPortB.pad == defaultB.pad && mdPortB.keyboard == defaultB.keyboard &&
                mdPortB.padSecondary.isEmpty() && mdPortB.keyboardSecondary.isEmpty() &&
                mdPortB.mouseSecondary.isEmpty() && mdPortB.mouse == defaultB.mouse;
            if (portBIsDefault) { mdPortB = mdPortA; legacyGunMoved = true; }
            else if (mdPortA.controllerType == ControllerType::Justifier &&
                     mdPortB.controllerType == ControllerType::Justifier) {
                auto& chained = mappings[profileIndex(ConsoleProfile::MegaDrive)][2];
                const auto defaultChained = InputManager::defaultMapping(ConsoleProfile::MegaDrive, 2);
                if (chained.connected == defaultChained.connected &&
                    chained.sixButton == defaultChained.sixButton &&
                    chained.controllerType == defaultChained.controllerType &&
                    chained.aimSource == defaultChained.aimSource &&
                    chained.showCrosshair == defaultChained.showCrosshair &&
                    chained.deviceKey == defaultChained.deviceKey &&
                    chained.deadzonePercent == defaultChained.deadzonePercent &&
                    chained.analogSensitivity == defaultChained.analogSensitivity &&
                    chained.pad == defaultChained.pad && chained.keyboard == defaultChained.keyboard &&
                    chained.padSecondary.isEmpty() && chained.keyboardSecondary.isEmpty() &&
                    chained.mouseSecondary.isEmpty() && chained.mouse == defaultChained.mouse) {
                    chained = mdPortA;
                    legacyGunMovedToChain = true;
                }
            }
            mdPortA.controllerType = ControllerType::Pad;
            mdPortA.connected = false; // A gun migration must not create an implicit port-A pad.
        }
        for (int p = 0; p < 4; ++p)
            for (int player = 0; player < playerCount; ++player) {
                auto& slot = mappings[p][player];
                slot.controllerType = slotControllerType(ConsoleProfile(p), player, slot.controllerType);
                if (player == 2 && p != profileIndex(ConsoleProfile::MegaDrive))
                    slot.connected = false;
            }
        for (int command = PauseCommand; command < targetCount; ++command) {
            const QString key = QStringLiteral("input/commands/key/%1").arg(command);
            commandKeys[command] = normalizedCommandKey(settings.value(
                key, InputManager::defaultCommandKey(command)).toInt());
            commandKeysSecondary[command] = normalizedCommandKey(settings.value(
                QStringLiteral("input/commands/keySecondary/%1").arg(command), 0).toInt());
        }
        sanitizeCommandKeys();
        SDL_SetMainReady(); // Qt owns main(), including in SDL's Windows builds.
        if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) == 0) {
            sdlReady = true;
            SDL_JoystickEventState(SDL_ENABLE);
            SDL_GameControllerEventState(SDL_ENABLE);
        } else {
            const QString message = Localization::text(QStringLiteral("Геймпады недоступны: %1")).arg(QString::fromUtf8(SDL_GetError()));
            QTimer::singleShot(0, owner, [parent, message] { emit parent->notification(message); });
        }
    }
    ~Impl() {
        devices.clear();
        if (sdlReady) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    }
    bool gameplayUsesKey(int key) const {
        for (const auto& profileMappings : mappings)
            for (const auto& mapping : profileMappings)
                for (int slot = 0; slot < 2; ++slot) {
                    const auto& bindings = slot == 1 ? mapping.keyboardSecondary : mapping.keyboard;
                    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it)
                        if (gameplayTarget(it.key()) && it.value() == key) return true;
                }
        return false;
    }
    bool sanitizeCommandKeys() {
        bool changed = false;
        QSet<int> used;
        for (int command = PauseCommand; command < targetCount; ++command) {
            int accepted[2]{};
            for (int slot = 0; slot < 2; ++slot) {
                const int requested = normalizedCommandKey((slot == 1 ? commandKeysSecondary : commandKeys).value(command));
                const bool gameplayConflict = (requested & commandModifierMask) == 0 && gameplayUsesKey(requested);
                accepted[slot] = requested && !reservedCommandChord(requested) &&
                    !gameplayConflict && !used.contains(requested) ? requested : 0;
                if (accepted[slot]) used.insert(accepted[slot]);
            }
            if (!accepted[0]) std::swap(accepted[0], accepted[1]);
            if (commandKeys.value(command) != accepted[0] || commandKeysSecondary.value(command) != accepted[1]) changed = true;
            commandKeys[command] = accepted[0];
            commandKeysSecondary[command] = accepted[1];
        }
        return changed;
    }
    void writeCommandKeys(QSettings& settings) const {
        for (int command = PauseCommand; command < targetCount; ++command) {
            const QString key = QStringLiteral("input/commands/key/%1").arg(command);
            const int value = commandKeys.value(command);
            if (value == InputManager::defaultCommandKey(command)) settings.remove(key);
            else settings.setValue(key, value);
            const QString secondaryKey = QStringLiteral("input/commands/keySecondary/%1").arg(command);
            const int secondary = commandKeysSecondary.value(command);
            if (!secondary) settings.remove(secondaryKey);
            else settings.setValue(secondaryKey, secondary);
        }
    }
    Device* findById(int id) const {
        for (const auto& device : devices) if (device->instanceId == id) return device.get();
        return nullptr;
    }
    Device* findByKey(const QString& key) const {
        for (const auto& device : devices) if (device->key == key) return device.get();
        return nullptr;
    }
    bool connected(int player) const {
        if (jcartPlayer(player)) {
            const bool recorded = movieControllersActive && profile == movieProfile;
            return recorded ? effective(player).connected
                : adapterConnections[profileIndex(profile)][player];
        }
        if (effectiveAdapter() != ControllerAdapter::None) {
            const bool recorded = movieControllersActive && profile == movieProfile;
            return player < int(adapterPlayerCount(effectiveAdapter())) &&
                (recorded ? effective(player).connected
                          : adapterConnections[profileIndex(profile)][player]);
        }
        if (player >= 3) return false;
        if (player == 2)
            return profile == ConsoleProfile::MegaDrive && effective(1).connected &&
                   effective(1).controllerType == ControllerType::Justifier && effective(2).connected;
        return effective(player).connected;
    }
    std::array<Device*, playerCount> assignments() const {
        std::array<Device*, playerCount> out{};
        for (int player = 0; player < playerCount; ++player) {
            const QString key = effective(player).deviceKey;
            if (key != "auto" && key != "none") out[player] = findByKey(key);
        }
        for (int pass = 0; pass < 2; ++pass) {
            for (int player = 0; player < playerCount; ++player) {
                if (effective(player).deviceKey != "auto" ||
                    inputSourceConnected(player) != (pass == 0)) continue;
                for (const auto& device : devices) {
                    bool usedByActive = false;
                    for (int other = 0; other < playerCount; ++other)
                        usedByActive |= inputSourceConnected(other) && out[other] == device.get();
                    if (!usedByActive) { out[player] = device.get(); break; }
                }
                if (!out[player] && !devices.empty()) out[player] = devices.front().get();
            }
        }
        return out;
    }
    void refreshDevices() {
        if (!sdlReady) return;
        SDL_PumpEvents();
        SDL_GameControllerUpdate();
        SDL_JoystickUpdate();
        SDL_FlushEvents(SDL_JOYAXISMOTION, SDL_CONTROLLERSENSORUPDATE);
        const int count = SDL_NumJoysticks();
        QSet<int> present;
        for (int index = 0; index < count; ++index) {
            const int id = SDL_JoystickGetDeviceInstanceID(index);
            if (id >= 0) present.insert(id);
        }
        bool changed = false;
        QStringList notices;
        for (int i = int(devices.size()) - 1; i >= 0; --i) {
            if (!present.contains(devices[i]->instanceId)) {
                notices.push_back(Localization::text(QStringLiteral("Геймпад отключён: %1")).arg(devices[i]->name));
                if (capture && captureInstance == devices[i]->instanceId) cancelCapture();
                devices.erase(devices.begin() + i);
                changed = true;
            }
        }
        for (int index = 0; index < count; ++index) {
            const int id = SDL_JoystickGetDeviceInstanceID(index);
            if (id < 0 || findById(id)) continue;
            auto device = std::make_unique<Device>();
            if (SDL_IsGameController(index)) {
                device->controller = SDL_GameControllerOpen(index);
                if (device->controller) device->joystick = SDL_GameControllerGetJoystick(device->controller);
            }
            if (!device->joystick) device->joystick = SDL_JoystickOpen(index);
            if (!device->joystick) continue;
            device->instanceId = id;
            device->name = QString::fromUtf8(SDL_JoystickName(device->joystick));
            device->guid = guidString(device->joystick);
            const char* serial = SDL_JoystickGetSerial(device->joystick);
            const QString serialText = serial ? QString::fromUtf8(serial) : QString();
            QString base = device->guid + (serialText.isEmpty() ? QStringLiteral("|ordinal") : QStringLiteral("|serial:") + serialText);
            int ordinal = 0;
            device->key = base;
            while (findByKey(device->key))
                device->key = base + '#' + QString::number(++ordinal);
            notices.push_back(Localization::text(QStringLiteral("Геймпад подключён: %1")).arg(device->name));
            devices.push_back(std::move(device));
            changed = true;
        }
        if (changed) {
            axisHeld = {};
            emit owner->devicesChanged();
            for (const QString& notice : notices) {
                if (initialScan) {
                    InputManager* target = owner;
                    QTimer::singleShot(0, target, [target, notice] { emit target->notification(notice); });
                } else emit owner->notification(notice);
            }
        }
        initialScan = false;
    }
    bool axisActive(int magnitude, int deadzone, bool& held) {
        const int threshold = 32767 * std::clamp(deadzone, 5, 60) / 100;
        const int releaseThreshold = threshold * 3 / 4;
        const bool nowHeld = magnitude >= (held ? releaseThreshold : threshold);
        held = nowHeld;
        return nowHeld;
    }
    bool active(const Device* device, const PadBinding& b, int deadzone, bool& held) {
        if (!device) return false;
        SDL_Joystick* joy = device->joystick;
        if (b.kind == PadBinding::Button)
            return b.index >= 0 && b.index < SDL_JoystickNumButtons(joy) && SDL_JoystickGetButton(joy, b.index);
        if (b.kind == PadBinding::Hat)
            return b.index >= 0 && b.index < SDL_JoystickNumHats(joy) && (SDL_JoystickGetHat(joy, b.index) & b.direction);
        if (b.kind == PadBinding::Axis && b.index >= 0 && b.index < SDL_JoystickNumAxes(joy) &&
            (b.direction == -1 || b.direction == 1)) {
            const int magnitude = int(SDL_JoystickGetAxis(joy, b.index)) * b.direction;
            return axisActive(magnitude, deadzone, held);
        }
        return false;
    }
    bool automaticActive(const Device* device, int target, int deadzone, bool& held) {
        if (!device) return false;
        if (device->controller) {
            const auto logical = controllerButton(profile, target);
            const bool digital = logical != SDL_CONTROLLER_BUTTON_INVALID &&
                SDL_GameControllerGetButton(device->controller, logical);
            if (target < 4 || target > 7) return digital;
            const auto logicalAxis = (target == 4 || target == 5) ?
                SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_LEFTX;
            const int magnitude = int(SDL_GameControllerGetAxis(device->controller, logicalAxis)) * dpadDirection(target);
            return digital || axisActive(magnitude, deadzone, held);
        }
        if (target >= 4 && target <= 7) {
            const bool hatPressed = active(device, rawDefault(target), deadzone, held);
            const bool stickPressed = SDL_JoystickNumAxes(device->joystick) >= 2 &&
                active(device, axis(target <= 5 ? 1 : 0, dpadDirection(target)), deadzone, held);
            return hatPressed || stickPressed;
        }
        return active(device, rawDefault(profile == ConsoleProfile::MasterSystem && target == 1 ? 0 : target),
                      deadzone, held);
    }
    void pollAim(const std::array<Device*, playerCount>& assigned) {
        // Only poll() advances analog input. Key events and settings changes
        // cannot multiply motion. Use the same device assignment as pad input.
        const bool allowed = (enabled || backgroundGamepadActive) && !capture;
        bool timedMotionNeeded = false;
        for (int player = 0; player < playerCount; ++player) {
            const auto type = inputSourceType(player);
            const auto& mapping = effective(player);
            const auto* device = assigned[player];
            const bool stick = allowed && inputSourceConnected(player) &&
                mapping.aimSource == AimSource::LeftStick && device;
            if (stick && type == ControllerType::Paddle) {
                int raw;
                if (device->controller)
                    raw = SDL_GameControllerGetAxis(device->controller, SDL_CONTROLLER_AXIS_LEFTX);
                else if (SDL_JoystickNumAxes(device->joystick) > 0)
                    raw = SDL_JoystickGetAxis(device->joystick, 0);
                else {
                    paddlePositionValid[player] = false;
                    continue;
                }
                const int threshold = 32767 * mapping.deadzonePercent / 100;
                const int magnitude = std::abs(raw);
                const int position = magnitude <= threshold ? 0 :
                    (raw < 0 ? -1 : 1) * std::min(32767,
                        (magnitude - threshold) * 32767 / (32767 - threshold));
                if (!paddlePositionValid[player] || paddlePositions[player] != position) {
                    paddlePositions[player] = position;
                    paddlePositionValid[player] = true;
                    emit owner->paddleMoved(unsigned(player), position);
                }
            } else {
                paddlePositionValid[player] = false;
            }
            timedMotionNeeded |= stick && (isLightgun(type) || type == ControllerType::Mouse);
        }
        if (!timedMotionNeeded) { aimTimer.invalidate(); return; }
        if (!aimTimer.isValid()) { aimTimer.start(); return; }
        const double seconds = std::min(aimTimer.nsecsElapsed() * 1e-9, 0.032);
        aimTimer.restart();
        for (int player = 0; player < playerCount; ++player) {
            const auto type = inputSourceType(player);
            if (!isLightgun(type) && type != ControllerType::Mouse) continue;
            const auto& mapping = effective(player);
            const auto* device = assigned[player];
            if (!allowed || !inputSourceConnected(player) ||
                mapping.aimSource != AimSource::LeftStick || !device) continue;
            double x, y;
            if (device->controller) {
                x = SDL_GameControllerGetAxis(device->controller, SDL_CONTROLLER_AXIS_LEFTX) / 32768.0;
                y = SDL_GameControllerGetAxis(device->controller, SDL_CONTROLLER_AXIS_LEFTY) / 32768.0;
            } else {
                if (SDL_JoystickNumAxes(device->joystick) < 2) continue;
                x = SDL_JoystickGetAxis(device->joystick, 0) / 32768.0;
                y = SDL_JoystickGetAxis(device->joystick, 1) / 32768.0;
            }
            const double length = std::hypot(x, y);
            const double deadzone = mapping.deadzonePercent / 100.0;
            if (length <= deadzone) continue;
            const double strength = (std::min(length, 1.0) - deadzone) / (1.0 - deadzone);
            if (type == ControllerType::Mouse) {
                constexpr double corePixelsPerSecond = 300.0;
                const double distance = strength * corePixelsPerSecond *
                    mapping.analogSensitivity / 100.0 * seconds;
                emit owner->mouseMotion(unsigned(player),
                                        x / length * distance, y / length * distance);
            } else {
                constexpr double screenFractionsPerSecond = 0.65;
                const double distance = strength * screenFractionsPerSecond * seconds;
                emit owner->aimMoved(unsigned(player),
                                     x / length * distance, y / length * distance);
            }
        }
    }
    unsigned padMask(int player, Device* device) {
        if (!device) return 0;
        const auto& m = effective(player);
        unsigned result = 0;
        for (int target : gameTargetRange(profile, m.sixButton, inputSourceType(player))) {
            for (int slot = 0; slot < 2; ++slot) {
                const auto configured = m.padBinding(target, slot);
                bool& held = axisHeld[player][target][slot];
                const bool pressed = configured.kind == PadBinding::Automatic ?
                    automaticActive(device, target, m.deadzonePercent, held) :
                    active(device, configured, m.deadzonePercent, held);
                if (pressed) result |= (1u << target);
            }
        }
        return result;
    }
    void recompute(bool updateAim = false) {
        const auto assigned = assignments();
        std::array<bool, targetCount - 16> nextCommands{};
        for (int player = 0; player < playerCount; ++player) {
            const auto& m = effective(player);
            const unsigned pad = inputSourceConnected(player) ? padMask(player, assigned[player]) : 0u;
            if (previews[player] != pad) {
                previews[player] = pad;
                emit owner->previewChanged(player, pad);
            }
            unsigned mask = pad;
            for (int target : gameTargetRange(profile, m.sixButton, inputSourceType(player)))
                if (keys.contains(m.keyboardBinding(target, 0)) || keys.contains(m.keyboardBinding(target, 1)))
                    mask |= (1u << target);
            if ((!enabled && !backgroundGamepadActive) || capture || !inputSourceConnected(player)) mask = 0;
            if (masks[player] != mask) {
                masks[player] = mask;
                emit owner->maskChanged(player, mask);
            }
            if (enabled && !capture && assigned[player]) {
                for (int command = 16; command < targetCount; ++command) {
                    for (int slot = 0; slot < 2; ++slot)
                        nextCommands[command - 16] |= active(assigned[player], m.padBinding(command, slot),
                            m.deadzonePercent, axisHeld[player][command][slot]);
                }
            }
        }
        for (int command = 16; command < targetCount; ++command) {
            const int index = command - 16;
            bool value = nextCommands[index];
            if (enabled && !capture && commandBlocked[index]) {
                if (!value) commandBlocked[index] = false;
                value = false;
            }
            if (commands[command - 16] != value) {
                commands[command - 16] = value;
                emit owner->commandChanged(command, value);
            }
        }
        if (updateAim) pollAim(assigned);
    }
    void cancelCapture() {
        if (!capture) return;
        capture = false; captureInstance = -1; capturePlayer = -1;
        emit owner->captureCancelled();
        recompute();
    }
    void updateCapture() {
        if (!capture) return;
        if (captureTimer.elapsed() >= captureMilliseconds) { cancelCapture(); return; }
        Device* device = findById(captureInstance);
        if (!device) { cancelCapture(); return; }
        SDL_Joystick* joy = device->joystick;
        PadBinding found;
        // Each physical input arms independently. A held/drifting axis must
        // never stop a fresh button or hat press from being assigned.
        for (int i = 0; i < captureButtonsBlocked.size(); ++i) {
            const bool down = SDL_JoystickGetButton(joy,i);
            if (!down) captureButtonsBlocked[i] = 0;
            else if (!captureButtonsBlocked[i]) { found = button(i); break; }
        }
        if (found.kind == PadBinding::None) for (int i = 0; i < captureHatsBlocked.size(); ++i) {
            const Uint8 current = SDL_JoystickGetHat(joy,i);
            captureHatsBlocked[i] &= current;
            const Uint8 fresh = current & ~captureHatsBlocked[i];
            for (const int direction : {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT})
                if (fresh & direction) { found = hat(i,direction); break; }
            if (found.kind != PadBinding::None) break;
        }
        if (found.kind == PadBinding::None) for (int i = 0; i < neutralAxes.size(); ++i) {
            const int value = SDL_JoystickGetAxis(joy,i);
            const int neutral = neutralAxes[i];
            auto& neutralPolls = captureAxisNeutralPolls[i];
            if (neutralPolls < 2) {
                if (std::abs(value-neutral) <= 8000) ++neutralPolls;
                else neutralPolls = 0;
                // A deliberate Up -> Down movement can cross the center
                // between polls. This applies only to centered axes, not a
                // trigger returning to its released negative endpoint.
                const int start = captureAxesStart[i];
                if (std::abs(neutral) <= 8000 && std::abs(start) >= captureAxisThreshold &&
                    std::abs(value) >= captureAxisThreshold && ((start < 0) != (value < 0)))
                    neutralPolls = 2;
            }
            if (neutralPolls >= 2 && std::abs(value-neutral) >= captureAxisThreshold &&
                std::abs(value) >= captureAxisThreshold) {
                found = axis(i,value > 0 ? 1 : -1);
                break;
            }
        }
        if (found.kind != PadBinding::None) {
            capture = false; captureInstance = -1; capturePlayer = -1;
            emit owner->bindingCaptured(found);
            recompute();
        }
    }
};

InputManager::InputManager(const QString& settingsPath, QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>(this, settingsPath)) {
    if (d->legacyPortOneGun) {
        setMapping(ConsoleProfile::MegaDrive, 0, mapping(ConsoleProfile::MegaDrive, 0));
        if (d->legacyGunMoved)
            setMapping(ConsoleProfile::MegaDrive, 1, mapping(ConsoleProfile::MegaDrive, 1));
        if (d->legacyGunMovedToChain)
            setMapping(ConsoleProfile::MegaDrive, 2, mapping(ConsoleProfile::MegaDrive, 2));
    }
    poll();
}
InputManager::~InputManager() = default;
QVector<InputDevice> InputManager::devices() const {
    QVector<InputDevice> result;
    for (const auto& device : d->devices) result.push_back({device->instanceId, device->key, device->name});
    return result;
}
PlayerMapping InputManager::mapping(ConsoleProfile profile, int player) const {
    if (!validPlayer(player)) return {};
    if (d->movieControllersActive && profile == d->movieProfile)
        return d->movieMappings[player];
    PlayerMapping result = ordinaryMapping(profile, player);
    const bool activeProfile = profile == d->profile;
    const ControllerAdapter activeAdapter = activeProfile ? d->effectiveAdapter()
        : d->adapters[profileIndex(profile)];
    if (activeAdapter != ControllerAdapter::None ||
        (activeProfile && d->jcartPlayer(player)))
        result.connected = d->adapterConnections[profileIndex(profile)][player];
    if (activeProfile && d->jcartPlayer(player))
        result.controllerType = ControllerType::Pad;
    return result;
}
PlayerMapping InputManager::ordinaryMapping(ConsoleProfile profile, int player) const {
    return validPlayer(player) ? d->mappings[profileIndex(profile)][player] : PlayerMapping{};
}
void InputManager::setMapping(ConsoleProfile profile, int player, const PlayerMapping& value) {
    if (!validPlayer(player)) return;
    const unsigned oldJCartBase = d->effectiveJCartBase();
    auto m = value;
    m.deadzonePercent = std::clamp(m.deadzonePercent, 5, 60);
    m.analogSensitivity = std::clamp(m.analogSensitivity, 25, 400);
    m.controllerType = slotControllerType(profile, player, m.controllerType);
    if (player == 2 && profile != ConsoleProfile::MegaDrive) m.connected = false;
    if (m.aimSource != AimSource::LeftStick) m.aimSource = AimSource::Mouse;
    for (int slot = 0; slot < 2; ++slot) {
        auto& keyboard = slot == 1 ? m.keyboardSecondary : m.keyboard;
        for (auto it = keyboard.begin(); it != keyboard.end(); ++it)
            it.value() = storedKeyboardKey(it.value());
        auto& mouse = slot == 1 ? m.mouseSecondary : m.mouse;
        for (auto type = mouse.begin(); type != mouse.end(); ++type)
            for (auto button = type.value().begin(); button != type.value().end(); ++button)
                if (!validMouseButton(button.value())) button.value() = 0;
    }
    for (int target = 0; target < targetCount; ++target) {
        if (m.keyboard.value(target) == m.keyboardSecondary.value(target)) m.keyboardSecondary.remove(target);
        if (m.pad.value(target) == m.padSecondary.value(target)) m.padSecondary.remove(target);
        for (auto type = m.mouseSecondary.begin(); type != m.mouseSecondary.end(); ++type)
            if (m.mouse.value(type.key()).value(target) == type.value().value(target)) type.value().remove(target);
    }
    d->axisHeld[player] = {};
    const auto defaults = defaultMapping(profile, player);
    const auto& previous = d->mappings[profileIndex(profile)][player];
    if (d->movieControllersActive && profile == d->movieProfile) {
        // Bindings may change during a movie; recorded port hardware is transient.
        m.connected = previous.connected;
        m.sixButton = previous.sixButton;
        m.controllerType = previous.controllerType;
        const auto hardware = d->movieMappings[player];
        d->movieMappings[player] = m;
        d->movieMappings[player].connected = hardware.connected;
        d->movieMappings[player].sixButton = hardware.sixButton;
        d->movieMappings[player].controllerType = hardware.controllerType;
    }
    if (profile == d->profile && (previous.sixButton != m.sixButton || previous.controllerType != m.controllerType))
        d->keys.clear(); // Release keys hidden by a type change, including their later key-up.
    d->aimTimer.invalidate();
    d->paddlePositionValid.fill(false);
    const bool tapActive = (profile == d->profile ? d->effectiveAdapter()
        : d->adapters[profileIndex(profile)]) != ControllerAdapter::None ||
        (profile == d->profile && d->jcartPlayer(player));
    if (tapActive && !(d->movieControllersActive && profile == d->movieProfile)) {
        d->adapterConnections[profileIndex(profile)][player] = value.connected;
        m.connected = previous.connected; // Keep the ordinary port and chained gun state.
    }
    d->mappings[profileIndex(profile)][player] = m;
    QSettings settings(d->settingsPath, QSettings::IniFormat);
    const QString prefix = settingsPrefix(profile, player);
    settings.setValue(prefix + "device", m.deviceKey);
    settings.setValue(prefix + "connected", m.connected);
    if (tapActive && !(d->movieControllersActive && profile == d->movieProfile))
        settings.setValue(QStringLiteral("input/%1/adapterPlayer%2/connected")
            .arg(profileId(profile)).arg(player + 1),
            d->adapterConnections[profileIndex(profile)][player]);
    settings.setValue(prefix + "sixButton", m.sixButton);
    settings.setValue(prefix + "controllerType", int(m.controllerType));
    settings.setValue(prefix + "aimSource", int(m.aimSource));
    if (m.showCrosshair) settings.remove(prefix + "showCrosshair");
    else settings.setValue(prefix + "showCrosshair", false);
    settings.setValue(prefix + "deadzone", m.deadzonePercent);
    if (m.analogSensitivity == 100) settings.remove(prefix + "analogSensitivity");
    else settings.setValue(prefix + "analogSensitivity", m.analogSensitivity);
    for (int target = 0; target < targetCount; ++target) {
        const QString key = prefix + QStringLiteral("pad/%1").arg(target);
        if (m.pad.value(target) == defaults.pad.value(target)) settings.remove(key);
        else settings.setValue(key, bindingText(m.pad.value(target)));
        const QString keyKey = prefix + QStringLiteral("key/%1").arg(target);
        if (!gameplayTarget(target) || m.keyboard.value(target, 0) == defaults.keyboard.value(target, 0)) settings.remove(keyKey);
        else settings.setValue(keyKey, m.keyboard.value(target, 0));
        const QString secondPad = prefix + QStringLiteral("padSecondary/%1").arg(target);
        if (m.padSecondary.value(target).kind == PadBinding::None) settings.remove(secondPad);
        else settings.setValue(secondPad, bindingText(m.padSecondary.value(target)));
        const QString secondKey = prefix + QStringLiteral("keySecondary/%1").arg(target);
        if (!gameplayTarget(target) || !m.keyboardSecondary.value(target)) settings.remove(secondKey);
        else settings.setValue(secondKey, m.keyboardSecondary.value(target));
    }
    for (ControllerType type : {ControllerType::LightPhaser, ControllerType::Menacer,
                                ControllerType::Justifier, ControllerType::Mouse,
                                ControllerType::Paddle}) {
        if (validControllerType(profile, type) != type) continue;
        for (int target : gameTargetRange(profile, true, type)) {
            const QString key = prefix + QStringLiteral("mouse/%1/%2").arg(int(type)).arg(target);
            const int button = m.mouse.value(int(type)).value(target, 0);
            if (button == defaults.mouse.value(int(type)).value(target, 0)) settings.remove(key);
            else settings.setValue(key, button);
            const QString secondKey = prefix + QStringLiteral("mouseSecondary/%1/%2").arg(int(type)).arg(target);
            const int secondaryButton = m.mouseSecondary.value(int(type)).value(target, 0);
            if (!secondaryButton) settings.remove(secondKey);
            else settings.setValue(secondKey, secondaryButton);
        }
    }
    const bool commandKeysChanged = d->sanitizeCommandKeys();
    if (commandKeysChanged) d->writeCommandKeys(settings);
    settings.sync();
    if (commandKeysChanged) emit this->commandKeysChanged();
    if (profile == d->profile) {
        if (oldJCartBase != d->effectiveJCartBase()) {
            emit jcartChanged(d->effectiveJCart());
            for (unsigned slot = 0; slot < playerCount; ++slot) {
                emit connectionChanged(slot, d->connected(int(slot)));
                emit controllerTypeChanged(slot, d->effective(int(slot)).sixButton);
                emit deviceTypeChanged(slot, int(d->effectiveType(int(slot))));
            }
        } else {
            emit connectionChanged(unsigned(player), d->connected(player));
            if (player == 1) emit connectionChanged(2, d->connected(2));
            emit controllerTypeChanged(unsigned(player), d->effective(player).sixButton);
            emit deviceTypeChanged(unsigned(player), int(d->effectiveType(player)));
        }
        emit configurationChanged();
    }
    d->axisHeld = {};
    d->recompute();
}
PlayerMapping InputManager::defaultMapping(ConsoleProfile profile, int player) {
    PlayerMapping m;
    m.connected = player == 0;
    m.controllerType = slotControllerType(profile, player, ControllerType::Pad);
    m.aimSource = player == 2 ? AimSource::LeftStick : AimSource::Mouse;
    for (int target : gameTargets(profile)) m.pad[target] = automatic(target);
    // Keep hidden Phaser trigger/reload defaults alongside the SMS pad bindings;
    // changing device type never destroys either set of assignments.
    if (profile == ConsoleProfile::MasterSystem) {
        m.pad[1] = automatic(1);
        m.pad[2] = automatic(2);
        m.mouse[int(ControllerType::LightPhaser)][1] = int(Qt::LeftButton);
        m.mouse[int(ControllerType::LightPhaser)][2] = int(Qt::RightButton);
    }
    if (profile == ConsoleProfile::MasterSystem)
        m.mouse[int(ControllerType::Paddle)][0] = int(Qt::LeftButton);
    if (profile == ConsoleProfile::MegaDrive) {
        auto& mouse = m.mouse[int(ControllerType::Mouse)];
        mouse[1] = int(Qt::LeftButton);
        mouse[0] = int(Qt::RightButton);
        mouse[8] = int(Qt::MiddleButton);
        auto& menacer = m.mouse[int(ControllerType::Menacer)];
        menacer[1] = int(Qt::LeftButton);
        menacer[0] = int(Qt::RightButton);
        menacer[8] = int(Qt::XButton1);
        menacer[3] = int(Qt::MiddleButton);
        auto& justifier = m.mouse[int(ControllerType::Justifier)];
        justifier[1] = int(Qt::LeftButton);
        justifier[2] = int(Qt::RightButton);
        justifier[3] = int(Qt::MiddleButton);
    }
    if (player == 0) {
        m.keyboard[4] = Qt::Key_Up; m.keyboard[5] = Qt::Key_Down;
        m.keyboard[6] = Qt::Key_Left; m.keyboard[7] = Qt::Key_Right;
        m.keyboard[0] = Qt::Key_X; m.keyboard[8] = Qt::Key_C;
        if (profile != ConsoleProfile::SG1000) m.keyboard[3] = Qt::Key_Return;
        if (profile == ConsoleProfile::MasterSystem) {
            m.keyboard[1] = Qt::Key_Z;
            m.keyboard[2] = Qt::Key_Q;
        }
        if (profile == ConsoleProfile::MegaDrive) {
            m.keyboard[1] = Qt::Key_Z; m.keyboard[10] = Qt::Key_A;
            m.keyboard[9] = Qt::Key_S; m.keyboard[11] = Qt::Key_D;
            m.keyboard[2] = Qt::Key_Backspace;
        }
    } else if (player == 1) {
        // A complete second-player layout, disjoint from player 1 and commands.
        m.keyboard[4] = Qt::Key_I; m.keyboard[5] = Qt::Key_K;
        m.keyboard[6] = Qt::Key_J; m.keyboard[7] = Qt::Key_L;
        m.keyboard[0] = Qt::Key_G; m.keyboard[8] = Qt::Key_H;
        if (profile != ConsoleProfile::SG1000) m.keyboard[3] = Qt::Key_Space;
        if (profile == ConsoleProfile::MasterSystem) {
            m.keyboard[1] = Qt::Key_F;
            m.keyboard[2] = Qt::Key_U;
        }
        if (profile == ConsoleProfile::MegaDrive) {
            m.keyboard[1] = Qt::Key_F; m.keyboard[10] = Qt::Key_V;
            m.keyboard[9] = Qt::Key_B; m.keyboard[11] = Qt::Key_N;
            m.keyboard[2] = Qt::Key_U;
        }
    }
    return m;
}
bool InputManager::capturedBindingConflicts(ConsoleProfile profile, const QString& deviceKey, int target,
                                           const PadBinding& configured, const PadBinding& captured) const {
    if (configured.kind == PadBinding::None || captured.kind == PadBinding::None) return false;
    if (configured.kind != PadBinding::Automatic) return configured == captured;
    const auto* device = d->findByKey(deviceKey);
    if (!device || target >= 16) return false;
    if (!device->controller) {
        const auto physical = rawDefault(profile == ConsoleProfile::MasterSystem && target == 1 ? 0 : target);
        return physical == captured || (target >= 4 && target <= 7 &&
            captured == axis(target <= 5 ? 1 : 0,dpadDirection(target)));
    }
    const auto matches = [&captured](const SDL_GameControllerButtonBind& binding) {
        switch (binding.bindType) {
        case SDL_CONTROLLER_BINDTYPE_BUTTON:
            return captured.kind == PadBinding::Button && captured.index == binding.value.button;
        case SDL_CONTROLLER_BINDTYPE_HAT:
            return captured.kind == PadBinding::Hat && captured.index == binding.value.hat.hat &&
                   (captured.direction & binding.value.hat.hat_mask);
        case SDL_CONTROLLER_BINDTYPE_AXIS:
            return captured.kind == PadBinding::Axis && captured.index == binding.value.axis;
        default: return false;
        }
    };
    const auto logical = controllerButton(profile,target);
    if (logical != SDL_CONTROLLER_BUTTON_INVALID &&
        matches(SDL_GameControllerGetBindForButton(device->controller,logical)) &&
        SDL_GameControllerGetButton(device->controller,logical)) return true;
    if (target < 4 || target > 7) return false;
    const auto logicalAxis = target <= 5 ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_LEFTX;
    // Read the mapped sign at the captured edge: SDL may invert/split an axis.
    return matches(SDL_GameControllerGetBindForAxis(device->controller,logicalAxis)) &&
        int(SDL_GameControllerGetAxis(device->controller,logicalAxis)) * dpadDirection(target) >= captureAxisThreshold;
}

QString InputManager::bindingName(const PadBinding& binding) {
    switch (binding.kind) {
    case PadBinding::Button: return Localization::text(QStringLiteral("Кнопка %1")).arg(binding.index + 1);
    case PadBinding::Axis: return Localization::text(QStringLiteral("Ось %1 %2")).arg(binding.index + 1).arg(binding.direction > 0 ? '+' : '-');
    case PadBinding::Hat: return Localization::text(QStringLiteral("Крестовина %1 (%2)")).arg(binding.index + 1).arg(binding.direction);
    case PadBinding::Automatic: return Localization::text(QStringLiteral("Авто"));
    case PadBinding::None: return Localization::text(QStringLiteral("Не назначено"));
    }
    return Localization::text(QStringLiteral("Не назначено"));
}
QString InputManager::profileName(ConsoleProfile profile) {
    switch (profile) {
    case ConsoleProfile::MegaDrive: return QStringLiteral("Mega Drive");
    case ConsoleProfile::MasterSystem: return QStringLiteral("Master System");
    case ConsoleProfile::GameGear: return QStringLiteral("Game Gear");
    case ConsoleProfile::SG1000: return QStringLiteral("SG-1000");
    }
    return {};
}
QVector<int> InputManager::gameTargets(ConsoleProfile profile, bool sixButton, ControllerType type) {
    const auto targets = gameTargetRange(profile, sixButton, type);
    return QVector<int>(targets.begin(), targets.end());
}
QString InputManager::targetName(ConsoleProfile profile, int target, ControllerType type) {
    if (type == ControllerType::Paddle && target == 0)
        return Localization::text(QStringLiteral("Кнопка"));
    if (isLightgun(validControllerType(profile, type))) {
        if (target == 1) return Localization::text(QStringLiteral("Курок"));
        if (target == 2) return Localization::text(QStringLiteral("Перезарядка"));
        if (target == 0) return Localization::text(QStringLiteral("Кнопка 2"));
        if (target == 8) return Localization::text(QStringLiteral("Кнопка 3"));
    }
    switch (target) {
    case 4: return Localization::text(QStringLiteral("Вверх"));
    case 5: return Localization::text(QStringLiteral("Вниз"));
    case 6: return Localization::text(QStringLiteral("Влево"));
    case 7: return Localization::text(QStringLiteral("Вправо"));
    case 0: return profile == ConsoleProfile::MegaDrive ? QStringLiteral("B") : QStringLiteral("1");
    case 8: return profile == ConsoleProfile::MegaDrive ? QStringLiteral("C") : QStringLiteral("2");
    case 1: return QStringLiteral("A");
    case 3: return profile == ConsoleProfile::MasterSystem ? Localization::text(QStringLiteral("Пауза консоли")) : QStringLiteral("Start");
    case 2: return QStringLiteral("Mode");
    case 9: return QStringLiteral("Y");
    case 10: return QStringLiteral("X");
    case 11: return QStringLiteral("Z");
    case PauseCommand: return Localization::text(QStringLiteral("Пауза эмуляции"));
    case FastForwardCommand: return Localization::text(QStringLiteral("Ускорение"));
    case SaveCommand: return Localization::text(QStringLiteral("Быстрое сохранение"));
    case LoadCommand: return Localization::text(QStringLiteral("Быстрая загрузка"));
    case ResetCommand: return Localization::text(QStringLiteral("Сброс"));
    case HardResetCommand: return Localization::text(QStringLiteral("Перезапуск питания"));
    case FrameAdvanceCommand: return Localization::text(QStringLiteral("Следующий кадр"));
    case FrameBackCommand: return Localization::text(QStringLiteral("Предыдущий кадр"));
    case SpeedDecreaseCommand: return Localization::text(QStringLiteral("Замедлить на 5%"));
    case SpeedIncreaseCommand: return Localization::text(QStringLiteral("Ускорить на 5%"));
    case SpeedResetCommand: return Localization::text(QStringLiteral("Вернуть 100%"));
    case FullscreenCommand: return Localization::text(QStringLiteral("Полный экран"));
    case MuteCommand: return Localization::text(QStringLiteral("Без звука"));
    case NextSlotCommand: return Localization::text(QStringLiteral("Следующий слот"));
    case PreviousSlotCommand: return Localization::text(QStringLiteral("Предыдущий слот"));
    case MenuCommand: return Localization::text(QStringLiteral("Меню"));
    case RewindCommand: return Localization::text(QStringLiteral("Перемотка назад"));
    case SaveFileCommand: return Localization::text(QStringLiteral("Сохранить в файл"));
    case LoadFileCommand: return Localization::text(QStringLiteral("Загрузить из файла"));
    case ScreenshotCommand: return Localization::text(QStringLiteral("Сохранить скриншот"));
    default: return Localization::text(QStringLiteral("Кнопка %1")).arg(target);
    }
}
bool InputManager::jcart() const { return d->effectiveJCart(); }
unsigned InputManager::jcartPlayerBase() const { return d->effectiveJCartBase(); }
void InputManager::setJCart(bool enabled) {
    if (d->jcartActive == enabled) return;
    const ControllerAdapter oldAdapter = d->effectiveAdapter();
    d->jcartActive = enabled;
    if (d->profile != ConsoleProfile::MegaDrive ||
        (d->movieControllersActive && d->movieProfile == d->profile)) return;
    d->keys.clear();
    d->axisHeld = {};
    d->aimTimer.invalidate();
    d->paddlePositionValid.fill(false);
    if (d->effectiveAdapter() != oldAdapter)
        emit adapterChanged(int(d->effectiveAdapter()));
    emit jcartChanged(enabled);
    for (unsigned player = 0; player < playerCount; ++player) {
        emit connectionChanged(player, d->connected(int(player)));
        emit controllerTypeChanged(player, d->effective(int(player)).sixButton);
        emit deviceTypeChanged(player, int(d->effectiveType(int(player))));
    }
    emit configurationChanged();
    d->recompute();
}
ControllerAdapter InputManager::adapter() const {
    return d->effectiveAdapter();
}
ControllerAdapter InputManager::adapter(ConsoleProfile profile) const {
    return profile == d->profile ? d->effectiveAdapter()
        : d->adapters[profileIndex(profile)];
}
bool InputManager::adapterConnection(ConsoleProfile profile, unsigned player) const {
    return player < playerCount &&
        d->adapterConnections[profileIndex(profile)][player];
}

void InputManager::setAdapter(ConsoleProfile profile, ControllerAdapter requested) {
    const ControllerAdapter selected = allowedAdapter(profile, requested);
    const int p = profileIndex(profile);
    if (d->adapters[p] == selected) return;
    const ControllerAdapter oldEffective = d->effectiveAdapter();
    d->adapters[p] = selected;
    QSettings settings(d->settingsPath, QSettings::IniFormat);
    settings.setValue(adapterSetting(profile), int(selected));
    settings.sync();
    if (profile != d->profile ||
        (d->movieControllersActive && d->movieProfile == d->profile)) return;
    d->keys.clear();
    d->axisHeld = {};
    d->aimTimer.invalidate();
    d->paddlePositionValid.fill(false);
    if (d->effectiveAdapter() != oldEffective)
        emit adapterChanged(int(d->effectiveAdapter()));
    for (unsigned player = 0; player < playerCount; ++player) {
        emit connectionChanged(player, d->connected(int(player)));
        emit controllerTypeChanged(player, d->effective(int(player)).sixButton);
        emit deviceTypeChanged(player, int(d->effectiveType(int(player))));
    }
    emit configurationChanged();
    d->recompute();
}
void InputManager::setProfile(ConsoleProfile profile) {
    if (d->profile == profile) return;
    const bool oldJCart = d->effectiveJCart();
    d->profile = profile; d->axisHeld = {}; d->aimTimer.invalidate();
    d->paddlePositionValid.fill(false);
    emit adapterChanged(int(d->effectiveAdapter()));
    if (oldJCart != d->effectiveJCart()) emit jcartChanged(d->effectiveJCart());
    for (unsigned player = 0; player < playerCount; ++player) {
        const auto& mapping = d->effective(int(player));
        emit connectionChanged(player, d->connected(player));
        emit controllerTypeChanged(player, mapping.sixButton);
        emit deviceTypeChanged(player, int(d->effectiveType(int(player))));
    }
    emit configurationChanged();
    d->recompute();
}
void InputManager::setMovieControllers(const QByteArray& controllersJson) {
    const ControllerAdapter previousAdapter = d->effectiveAdapter();
    const bool previousJCart = d->effectiveJCart();
    const unsigned previousJCartBase = d->effectiveJCartBase();
    if (controllersJson.isEmpty()) {
        if (!d->movieControllersActive) return;
        d->movieControllersActive = false;
    } else {
        const QJsonDocument document = QJsonDocument::fromJson(controllersJson);
        QJsonArray controllers;
        ControllerAdapter recordedAdapter = ControllerAdapter::None;
        bool recordedJCart = false;
        unsigned recordedJCartBase = 0;
        int count = 3;
        if (document.isArray()) {
            controllers = document.array(); // Legacy recordings have three ports.
        } else if (document.isObject()) {
            const QJsonObject object = document.object();
            const QJsonValue adapterValue = object.value(QStringLiteral("adapter"));
            if (!adapterValue.isDouble() || !validControllerAdapter(adapterValue.toInt(-1)) ||
                object.value(QStringLiteral("controllers")).type() != QJsonValue::Array) {
                qWarning("Ignoring invalid movie controller configuration");
                return;
            }
            recordedAdapter = ControllerAdapter(adapterValue.toInt());
            if (allowedAdapter(d->profile, recordedAdapter) != recordedAdapter) {
                qWarning("Ignoring unsupported movie controller adapter");
                return;
            }
            const QJsonValue jcartValue = object.value(QStringLiteral("jcart"));
            if (!jcartValue.isUndefined() && !jcartValue.isBool()) {
                qWarning("Ignoring invalid movie J-Cart configuration");
                return;
            }
            recordedJCart = jcartValue.toBool();
            if (recordedJCart) {
                const QJsonValue baseValue = object.value(QStringLiteral("jcartBase"));
                const int base = baseValue.toInt(-1);
                if (d->profile != ConsoleProfile::MegaDrive || !baseValue.isDouble() ||
                    base < 2 || base > 6 ||
                    (recordedAdapter == ControllerAdapter::None && base != 2 && base != 3) ||
                    (recordedAdapter != ControllerAdapter::None &&
                     (adapterPlayerCount(recordedAdapter) == MaxControllerPlayers ||
                      base != int(adapterPlayerCount(recordedAdapter))))) {
                    qWarning("Ignoring invalid movie J-Cart configuration");
                    return;
                }
                recordedJCartBase = unsigned(base);
            }
            controllers = object.value(QStringLiteral("controllers")).toArray();
            count = playerCount;
        } else {
            qWarning("Ignoring invalid movie controller configuration");
            return;
        }
        if (controllers.size() != count) {
            qWarning("Ignoring invalid movie controller configuration");
            return;
        }
        std::array<PlayerMapping, playerCount> next = d->mappings[profileIndex(d->profile)];
        for (int player = 0; player < count; ++player) {
            if (!controllers.at(player).isObject()) {
                qWarning("Ignoring invalid movie controller configuration");
                return;
            }
            const QJsonObject object = controllers.at(player).toObject();
            const QJsonValue connected = object.value(QStringLiteral("connected"));
            const QJsonValue sixButton = object.value(QStringLiteral("sixButton"));
            const QJsonValue kind = object.value(QStringLiteral("kind"));
            if (!connected.isBool() || !sixButton.isBool() || !kind.isDouble() ||
                kind.toInt(-1) < 0 || kind.toInt(-1) > int(ControllerType::Paddle)) {
                qWarning("Ignoring invalid movie controller configuration");
                return;
            }
            next[player].connected = connected.toBool();
            next[player].sixButton = sixButton.toBool();
            next[player].controllerType = slotControllerType(d->profile, player,
                static_cast<ControllerType>(kind.toInt()));
        }
        // v1 recordings describe only three ports. Leave their unrecorded
        // extra slots disconnected even if the user's local adapter bank is enabled.
        for (int player = count; player < playerCount; ++player) {
            next[player].connected = false;
            next[player].controllerType = ControllerType::Pad;
        }
        if (recordedJCart && recordedAdapter == ControllerAdapter::None) {
            const bool chained = next[1].connected &&
                next[1].controllerType == ControllerType::Justifier &&
                next[2].connected && next[2].controllerType == ControllerType::Justifier;
            if (recordedJCartBase != (chained ? 3u : 2u)) {
                qWarning("Ignoring inconsistent movie J-Cart base");
                return;
            }
        }
        bool unchanged = d->movieControllersActive && d->movieProfile == d->profile &&
                         d->movieAdapter == recordedAdapter && d->movieJCart == recordedJCart &&
                         d->movieJCartBase == recordedJCartBase;
        for (int player = 0; player < playerCount && unchanged; ++player) {
            const auto& previous = d->movieMappings[player];
            const auto& current = next[player];
            unchanged = previous.connected == current.connected &&
                        previous.sixButton == current.sixButton &&
                        previous.controllerType == current.controllerType;
        }
        // Repeated network snapshots must not release keys held by a remote player.
        if (unchanged) return;
        d->movieProfile = d->profile;
        d->movieAdapter = recordedAdapter;
        d->movieJCart = recordedJCart;
        d->movieJCartBase = recordedJCartBase;
        d->movieMappings = std::move(next);
        d->movieControllersActive = true;
    }
    d->keys.clear();
    d->axisHeld = {};
    d->aimTimer.invalidate();
    d->paddlePositionValid.fill(false);
    if (d->effectiveAdapter() != previousAdapter)
        emit adapterChanged(int(d->effectiveAdapter()));
    if (d->effectiveJCart() != previousJCart ||
        d->effectiveJCartBase() != previousJCartBase)
        emit jcartChanged(d->effectiveJCart());
    for (unsigned player = 0; player < playerCount; ++player) {
        emit connectionChanged(player, d->connected(int(player)));
        emit controllerTypeChanged(player, d->effective(int(player)).sixButton);
        emit deviceTypeChanged(player, int(d->effectiveType(int(player))));
    }
    emit configurationChanged();
    d->recompute();
}

ConsoleProfile InputManager::profile() const { return d->profile; }
bool InputManager::isConnected(unsigned player) const {
    return player < playerCount && d->connected(int(player));
}
bool InputManager::sixButton(unsigned player) const {
    return player < playerCount && d->effective(int(player)).sixButton;
}
ControllerType InputManager::controllerType(unsigned player) const {
    return player < playerCount ? d->effectiveType(int(player)) : ControllerType::Pad;
}
bool InputManager::aimWithMouse(unsigned player) const {
    return player < playerCount && d->effective(int(player)).aimSource == AimSource::Mouse;
}
unsigned InputManager::mouseButtonMask(unsigned player, unsigned qtButtons) const {
    if (player >= playerCount || !d->enabled || d->capture) return 0;
    const auto& mapping = d->effective(int(player));
    if (!d->inputSourceConnected(int(player)) || d->inputSourceType(int(player)) == ControllerType::Pad ||
        mapping.aimSource != AimSource::Mouse) return 0;
    unsigned mask = 0;
    for (int target : gameTargetRange(d->profile, mapping.sixButton, mapping.controllerType)) {
        for (int slot = 0; slot < 2; ++slot) {
            const int button = mapping.mouseBinding(mapping.controllerType, target, slot);
            if (button && (qtButtons & unsigned(button))) mask |= 1u << target;
        }
    }
    return mask;
}
bool InputManager::backgroundGamepadEnabled() const { return d->backgroundGamepadEnabled; }
void InputManager::setBackgroundGamepadEnabled(bool enabled) {
    if (d->backgroundGamepadEnabled == enabled) return;
    d->backgroundGamepadEnabled = enabled;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, enabled ? "1" : "0");
    QSettings settings(d->settingsPath, QSettings::IniFormat);
    settings.setValue(QStringLiteral("input/backgroundGamepad"), enabled);
    if (!enabled) setBackgroundGamepadActive(false);
    emit backgroundGamepadChanged(enabled);
}
bool InputManager::mouseCaptureEnabled() const { return d->mouseCaptureEnabled; }
void InputManager::setMouseCaptureEnabled(bool enabled) {
    if (d->mouseCaptureEnabled == enabled) return;
    d->mouseCaptureEnabled = enabled;
    QSettings settings(d->settingsPath, QSettings::IniFormat);
    settings.setValue(QStringLiteral("input/mouseCapture"), enabled);
    emit mouseCaptureChanged(enabled);
}
void InputManager::setBackgroundGamepadActive(bool active) {
    active = active && d->backgroundGamepadEnabled;
    if (d->backgroundGamepadActive == active) return;
    d->backgroundGamepadActive = active;
    d->aimTimer.invalidate();
    d->paddlePositionValid.fill(false);
    d->recompute();
}
void InputManager::setNetworkInputSourceEnabled(bool enabled) {
    if (d->networkInputSourceEnabled == enabled) return;
    d->networkInputSourceEnabled = enabled;
    d->axisHeld[0] = {};
    d->aimTimer.invalidate();
    d->paddlePositionValid.fill(false);
    d->recompute();
}
void InputManager::setEnabled(bool enabled) {
    if (d->enabled == enabled) return;
    d->enabled = enabled;
    d->aimTimer.invalidate();
    d->paddlePositionValid.fill(false);
    if (!enabled) d->keys.clear();
    d->commandBlocked.fill(true);
    d->recompute();
}
int InputManager::keyboardKey(const QKeyEvent& event) {
    return keyboardKey(event.key(), event.nativeScanCode(), KeyboardScanCodes::Native, event.modifiers());
}
int InputManager::keyboardKey(int logicalKey, quint32 nativeScanCode, KeyboardScanCodes codes,
                              Qt::KeyboardModifiers modifiers) {
    if (codes == KeyboardScanCodes::Native) {
#if defined(Q_OS_WIN)
        codes = KeyboardScanCodes::Windows;
#elif defined(Q_OS_LINUX)
        codes = KeyboardScanCodes::Xkb;
#else
        codes = KeyboardScanCodes::Unavailable;
#endif
    }
    // The standard printable keys have matching Set-1 and Linux evdev codes.
    // Preserve the E0 prefix on Windows: extended keys are different positions.
    quint32 scan = nativeScanCode;
    if (codes == KeyboardScanCodes::Xkb) scan = scan >= 8 ? scan - 8 : 0;
    if (codes != KeyboardScanCodes::Unavailable && nativeScanCode != 0) {
        // The keypad matrix occupies the same Set-1 / evdev positions. Dedicated
        // navigation keys are extended on Windows and have separate XKB codes.
        if (scan >= 0x47 && scan <= 0x53) return keypadKey("789-456+1230."[scan - 0x47]);
        if (scan == 0x37) return keypadKey(Qt::Key_Asterisk);
        if (scan == (codes == KeyboardScanCodes::Windows ? 0xe01cu : 96u)) return keypadKey(Qt::Key_Enter);
        if (scan == (codes == KeyboardScanCodes::Windows ? 0xe035u : 98u)) return keypadKey(Qt::Key_Slash);
        if (codes == KeyboardScanCodes::Xkb && scan == 117) return keypadKey(Qt::Key_Equal);
        if (codes == KeyboardScanCodes::Xkb && scan == 121) return keypadKey(Qt::Key_Comma);
        if (scan >= 0x02 && scan <= 0x0d) return "1234567890-="[scan - 0x02];
        if (scan >= 0x10 && scan <= 0x1b) return "QWERTYUIOP[]"[scan - 0x10];
        if (scan >= 0x1e && scan <= 0x28) return "ASDFGHJKL;'"[scan - 0x1e];
        if (scan >= 0x2c && scan <= 0x35) return "ZXCVBNM,./"[scan - 0x2c];
        if (scan == 0x29) return Qt::Key_QuoteLeft;
        if (scan == 0x2b) return Qt::Key_Backslash;
        if (scan == 0x56) return Qt::Key_Less; // extra ISO key, distinct from US backslash
    }
    // Some events only carry the Qt keypad flag. With Num Lock off their
    // logical key is the navigation function printed on the same keycap.
    if (modifiers.testFlag(Qt::KeypadModifier)) {
        switch (logicalKey) {
        case Qt::Key_Insert:   return keypadKey(Qt::Key_0);
        case Qt::Key_End:      return keypadKey(Qt::Key_1);
        case Qt::Key_Down:     return keypadKey(Qt::Key_2);
        case Qt::Key_PageDown: return keypadKey(Qt::Key_3);
        case Qt::Key_Left:     return keypadKey(Qt::Key_4);
        case Qt::Key_Clear:    return keypadKey(Qt::Key_5);
        case Qt::Key_Right:    return keypadKey(Qt::Key_6);
        case Qt::Key_Home:     return keypadKey(Qt::Key_7);
        case Qt::Key_Up:       return keypadKey(Qt::Key_8);
        case Qt::Key_PageUp:   return keypadKey(Qt::Key_9);
        case Qt::Key_Delete:
        case Qt::Key_Comma:    return keypadKey(Qt::Key_Period);
        case Qt::Key_Return:   return keypadKey(Qt::Key_Enter);
        default:
            if (logicalKey != Qt::Key_unknown && logicalKey != Qt::Key_NumLock)
                return keypadKey(logicalKey);
        }
    }
    return legacyKeyboardKey(logicalKey);
}
int InputManager::keyboardChord(const QKeyEvent& event) {
    int modifiers = int(event.modifiers()) & commandModifierMask;
    if (event.modifiers().testFlag(Qt::GroupSwitchModifier)) modifiers |= Qt::AltModifier;
    return normalizedCommandKey(keyboardKey(event) | modifiers);
}
int InputManager::defaultCommandKey(int command) {
    switch (command) {
    case PauseCommand: return Qt::Key_P;
    case FastForwardCommand: return Qt::Key_Tab;
    case SaveCommand: return Qt::Key_F5;
    case LoadCommand: return Qt::Key_F8;
    case ResetCommand: return (Qt::ControlModifier | Qt::Key_R).toCombined();
    case FullscreenCommand: return (Qt::AltModifier | Qt::Key_Return).toCombined();
    case MuteCommand: return Qt::Key_M;
    case RewindCommand: return Qt::Key_R;
    case HardResetCommand: return (Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_R).toCombined();
    case FrameAdvanceCommand: return Qt::Key_Period;
    case FrameBackCommand: return Qt::Key_Comma;
    case SpeedDecreaseCommand: return Qt::Key_BracketLeft;
    case SpeedIncreaseCommand: return Qt::Key_BracketRight;
    case SaveFileCommand: return (Qt::ShiftModifier | Qt::Key_F5).toCombined();
    case LoadFileCommand: return (Qt::ShiftModifier | Qt::Key_F8).toCombined();
    case ScreenshotCommand: return Qt::Key_F12;
    default: return 0;
    }
}
int InputManager::commandKey(int command, int slot) const {
    if (command < PauseCommand || command >= targetCount || slot < 0 || slot > 1) return 0;
    return (slot == 1 ? d->commandKeysSecondary : d->commandKeys).value(command);
}
void InputManager::setCommandKeys(const QMap<int,int>& keys) {
    QMap<int,QVector<int>> bindings;
    for (auto it = keys.cbegin(); it != keys.cend(); ++it) bindings[it.key()] = {it.value()};
    setCommandKeyBindings(bindings);
}
void InputManager::setCommandKeyBindings(const QMap<int,QVector<int>>& keys) {
    const auto previous = d->commandKeys;
    const auto previousSecondary = d->commandKeysSecondary;
    for (int command = PauseCommand; command < targetCount; ++command) {
        const auto bindings = keys.value(command, {defaultCommandKey(command)});
        d->commandKeys[command] = normalizedCommandKey(bindings.value(0));
        d->commandKeysSecondary[command] = normalizedCommandKey(bindings.value(1));
    }
    d->sanitizeCommandKeys();
    if (d->commandKeys == previous && d->commandKeysSecondary == previousSecondary) return;
    QSettings settings(d->settingsPath, QSettings::IniFormat);
    d->writeCommandKeys(settings);
    settings.sync();
    emit commandKeysChanged();
}
bool InputManager::reservedKeyboardKey(int key) {
    return reservedCommandChord(normalizedCommandKey(key));
}
bool InputManager::hasGameplayKey(int key) const {
    key = legacyKeyboardKey(key);
    if (!key || reservedKeyboardKey(key)) return false;
    for (int player = 0; player < playerCount; ++player) {
        const auto& mapping = d->effective(player);
        for (int target : gameTargetRange(d->profile, mapping.sixButton, d->inputSourceType(player)))
            if (mapping.keyboardBinding(target, 0) == key || mapping.keyboardBinding(target, 1) == key) return true;
    }
    return false;
}
bool InputManager::keyEvent(int key, bool pressed) {
    key = legacyKeyboardKey(key);
    if (!hasGameplayKey(key)) return false;
    if (!d->enabled || d->capture) return true;
    if (pressed) d->keys.insert(key); else d->keys.remove(key);
    d->recompute();
    return true;
}
void InputManager::releaseKeyboard() { d->keys.clear(); d->recompute(); }
void InputManager::beginCapture(const QString& deviceKey, int player) {
    // Replacing an in-progress capture is a re-arm, not a cancellation event.
    d->capture = false;
    if (!validPlayer(player)) { emit captureCancelled(); return; }
    d->refreshDevices();
    Impl::Device* device = nullptr;
    if (deviceKey == "auto") device = d->assignments()[player];
    else device = d->findByKey(deviceKey);
    if (!device) { emit captureCancelled(); return; }
    d->capture = true; d->captureInstance = device->instanceId;
    d->commandBlocked.fill(true);
    d->capturePlayer = player;
    d->neutralAxes = device->captureNeutralAxes;
    if (d->neutralAxes.size() != SDL_JoystickNumAxes(device->joystick)) {
        d->neutralAxes.clear();
        for (int i = 0; i < SDL_JoystickNumAxes(device->joystick); ++i) {
            Sint16 initial = 0;
            SDL_JoystickGetAxisInitialState(device->joystick, i, &initial);
            // Only actual mapped triggers may rest at a negative endpoint.
            // Axis numbers >= 2 also contain centered sticks on many pads.
            if (triggerAxis(device->controller, i) &&
                SDL_JoystickGetAxis(device->joystick, i) <= -30000)
                initial = -32768;
            d->neutralAxes.push_back(initial);
        }
    }
    device->captureNeutralAxes = d->neutralAxes;
    auto* joystick = device->joystick;
    d->captureButtonsBlocked.resize(qMax(0,SDL_JoystickNumButtons(joystick)));
    for (int i=0;i<d->captureButtonsBlocked.size();++i)
        d->captureButtonsBlocked[i] = SDL_JoystickGetButton(joystick,i) != 0;
    d->captureHatsBlocked.resize(qMax(0,SDL_JoystickNumHats(joystick)));
    for (int i=0;i<d->captureHatsBlocked.size();++i)
        d->captureHatsBlocked[i] = SDL_JoystickGetHat(joystick,i);
    d->captureAxesStart.resize(d->neutralAxes.size());
    d->captureAxisNeutralPolls.resize(d->neutralAxes.size());
    for (int i=0;i<d->neutralAxes.size();++i) {
        const int value=SDL_JoystickGetAxis(joystick,i);
        d->captureAxesStart[i]=value;
        d->captureAxisNeutralPolls[i]=std::abs(value-d->neutralAxes[i]) <= 8000 ? 2 : 0;
    }
    d->captureTimer.start();
    d->aimTimer.invalidate();
    d->paddlePositionValid.fill(false);
    d->recompute();
    emit captureReady();
}
void InputManager::cancelCapture() { d->cancelCapture(); }
bool InputManager::capturing() const { return d->capture; }
void InputManager::poll() {
    d->refreshDevices();
    d->updateCapture();
    d->recompute(true);
}
