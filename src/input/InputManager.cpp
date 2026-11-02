#include "InputManager.h"

#include <QElapsedTimer>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace {
constexpr int playerCount = 3; // Ports A/B and a second Justifier chained to port B.
constexpr int targetCount = FrameAdvanceCommand + 1;
constexpr int captureMilliseconds = 10000;
constexpr int captureAxisThreshold = 16000;

constexpr int mdTargets[] = {4, 5, 6, 7, 1, 0, 8, 10, 9, 11, 3, 2};
constexpr int md3Targets[] = {4, 5, 6, 7, 1, 0, 8, 3};
constexpr int smsTargets[] = {4, 5, 6, 7, 0, 8, 3};
constexpr int sgTargets[] = {4, 5, 6, 7, 0, 8};
constexpr int phaserTargets[] = {1, 2};
constexpr int menacerTargets[] = {1, 0, 8, 3, 2};
constexpr int justifierTargets[] = {1, 3, 2};
ControllerType validControllerType(ConsoleProfile profile, ControllerType type) {
    if (profile == ConsoleProfile::MegaDrive &&
        (type == ControllerType::Menacer || type == ControllerType::Justifier)) return type;
    if (profile == ConsoleProfile::MasterSystem && type == ControllerType::LightPhaser) return type;
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
bool activeGameplayTarget(ConsoleProfile profile, const PlayerMapping& mapping, int target) {
    const auto range = gameTargetRange(profile, mapping.sixButton, mapping.controllerType);
    return std::find(range.begin(), range.end(), target) != range.end();
}
bool validPlayer(int player) { return player >= 0 && player < playerCount; }
ControllerType slotControllerType(ConsoleProfile profile, int player, ControllerType type) {
    if (profile == ConsoleProfile::MegaDrive && player == 0) return ControllerType::Pad;
    if (player == 2) return profile == ConsoleProfile::MegaDrive
        ? ControllerType::Justifier : ControllerType::Pad;
    return validControllerType(profile, type);
}
bool validMouseButton(int button) {
    return button == 0 || button == int(Qt::LeftButton) || button == int(Qt::RightButton) ||
           button == int(Qt::MiddleButton) || button == int(Qt::XButton1) ||
           button == int(Qt::XButton2);
}
int legacyKeyboardKey(int key) {
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
    std::vector<std::unique_ptr<Device>> devices;
    QSet<int> keys;
    std::array<std::array<bool, targetCount>, playerCount> axisHeld{};
    ConsoleProfile profile = ConsoleProfile::MegaDrive;
    bool sdlReady = false;
    bool initialScan = true;
    bool enabled = true;
    std::array<unsigned, playerCount> masks{};
    std::array<unsigned, playerCount> previews{};
    bool legacyPortOneGun = false;
    bool legacyGunMoved = false;
    bool legacyGunMovedToChain = false;
    std::array<bool, targetCount - 16> commands{};
    std::array<bool, targetCount - 16> commandBlocked{};
    bool capture = false, captureArmed = false;
    int captureInstance = -1, capturePlayer = -1, neutralPolls = 0;
    QElapsedTimer captureTimer;
    QElapsedTimer aimTimer;
    QVector<int> neutralAxes;

    explicit Impl(InputManager* parent, QString path) : owner(parent), settingsPath(std::move(path)) {
        commandBlocked.fill(true);
        for (int p = 0; p < 4; ++p)
            for (int player = 0; player < playerCount; ++player)
                mappings[p][player] = InputManager::defaultMapping(ConsoleProfile(p), player);
        QSettings settings(settingsPath, QSettings::IniFormat);
        for (int p = 0; p < 4; ++p) for (int player = 0; player < playerCount; ++player) {
            auto& m = mappings[p][player];
            const QString prefix = settingsPrefix(ConsoleProfile(p), player);
            m.deviceKey = settings.value(prefix + "device", m.deviceKey).toString();
            m.connected = settings.value(prefix + "connected", m.connected).toBool();
            m.sixButton = settings.value(prefix + "sixButton", m.sixButton).toBool();
            m.controllerType = validControllerType(ConsoleProfile(p), static_cast<ControllerType>(
                settings.value(prefix + "controllerType", int(m.controllerType)).toInt()));
            m.aimSource = settings.value(prefix + "aimSource", int(m.aimSource)).toInt() == int(AimSource::LeftStick)
                ? AimSource::LeftStick : AimSource::Mouse;
            m.showCrosshair = settings.value(prefix + "showCrosshair", true).toBool();
            m.deadzonePercent = std::clamp(settings.value(prefix + "deadzone", m.deadzonePercent).toInt(), 5, 60);
            for (int target = 0; target < targetCount; ++target) {
                const QString key = prefix + QStringLiteral("pad/%1").arg(target);
                if (settings.contains(key)) {
                    m.pad[target] = parseBinding(settings.value(key).toString());
                    if (m.pad[target].kind == PadBinding::Automatic) m.pad[target] = automatic(target);
                }
                const QString keyKey = prefix + QStringLiteral("key/%1").arg(target);
                if (gameplayTarget(target) && settings.contains(keyKey)) m.keyboard[target] = storedKeyboardKey(settings.value(keyKey).toInt());
            }
            for (ControllerType type : {ControllerType::LightPhaser, ControllerType::Menacer,
                                        ControllerType::Justifier}) {
                if (validControllerType(ConsoleProfile(p), type) != type) continue;
                for (int target : gameTargetRange(ConsoleProfile(p), true, type)) {
                    const QString key = prefix + QStringLiteral("mouse/%1/%2").arg(int(type)).arg(target);
                    if (settings.contains(key)) {
                        const int button = settings.value(key).toInt();
                        if (validMouseButton(button)) m.mouse[int(type)][target] = button;
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
        if (mdPortA.controllerType != ControllerType::Pad) {
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
                mdPortB.pad == defaultB.pad && mdPortB.keyboard == defaultB.keyboard &&
                mdPortB.mouse == defaultB.mouse;
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
                    chained.pad == defaultChained.pad && chained.keyboard == defaultChained.keyboard &&
                    chained.mouse == defaultChained.mouse) {
                    chained = mdPortA;
                    legacyGunMovedToChain = true;
                }
            }
            mdPortA.controllerType = ControllerType::Pad;
            mdPortA.connected = false; // A gun migration must not create an implicit port-A pad.
        }
        for (int p = 0; p < 4; ++p) {
            auto& chained = mappings[p][2];
            chained.controllerType = slotControllerType(ConsoleProfile(p), 2, chained.controllerType);
            if (p != profileIndex(ConsoleProfile::MegaDrive)) chained.connected = false;
        }
        SDL_SetMainReady(); // Qt owns main(), including in SDL's Windows builds.
        if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) == 0) {
            sdlReady = true;
            SDL_JoystickEventState(SDL_ENABLE);
            SDL_GameControllerEventState(SDL_ENABLE);
        } else {
            const QString message = QStringLiteral("Геймпады недоступны: %1").arg(QString::fromUtf8(SDL_GetError()));
            QTimer::singleShot(0, owner, [parent, message] { emit parent->notification(message); });
        }
    }
    ~Impl() {
        devices.clear();
        if (sdlReady) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
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
        const auto& current = mappings[profileIndex(profile)];
        if (player == 2)
            return profile == ConsoleProfile::MegaDrive && current[1].connected &&
                   current[1].controllerType == ControllerType::Justifier && current[2].connected;
        return current[player].connected;
    }
    std::array<Device*, playerCount> assignments() const {
        std::array<Device*, playerCount> out{};
        for (int player = 0; player < playerCount; ++player) {
            const QString key = mappings[profileIndex(profile)][player].deviceKey;
            if (key != "auto" && key != "none") out[player] = findByKey(key);
        }
        for (int pass = 0; pass < 2; ++pass) {
            for (int player = 0; player < playerCount; ++player) {
                if (mappings[profileIndex(profile)][player].deviceKey != "auto" ||
                    connected(player) != (pass == 0)) continue;
                for (const auto& device : devices) {
                    bool usedByActive = false;
                    for (int other = 0; other < playerCount; ++other)
                        usedByActive |= connected(other) && out[other] == device.get();
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
                notices.push_back(QStringLiteral("Геймпад отключён: %1").arg(devices[i]->name));
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
            notices.push_back(QStringLiteral("Геймпад подключён: %1").arg(device->name));
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
        // Only poll() advances aim: key events and settings recomputations cannot
        // multiply movement. Reuse its device assignments; pad-only input has no
        // clock reads or extra SDL axis calls.
        bool needed = false;
        if (enabled && !capture) {
            for (unsigned player = 0; player < playerCount; ++player) {
                const auto& mapping = mappings[profileIndex(profile)][player];
                needed |= connected(player) && mapping.controllerType != ControllerType::Pad &&
                          mapping.aimSource == AimSource::LeftStick && assigned[player];
            }
        }
        if (!needed) { aimTimer.invalidate(); return; }
        if (!aimTimer.isValid()) { aimTimer.start(); return; }
        const double seconds = std::min(aimTimer.nsecsElapsed() * 1e-9, 0.032);
        aimTimer.restart();
        for (unsigned player = 0; player < playerCount; ++player) {
            const auto& mapping = mappings[profileIndex(profile)][player];
            const auto* device = assigned[player];
            if (!connected(player) || mapping.controllerType == ControllerType::Pad ||
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
            constexpr double screenFractionsPerSecond = 0.65;
            const double distance = (std::min(length, 1.0) - deadzone) / (1.0 - deadzone) *
                                    screenFractionsPerSecond * seconds;
            emit owner->aimMoved(player, x / length * distance, y / length * distance);
        }
    }
    unsigned padMask(int player, Device* device) {
        if (!device) return 0;
        const auto& m = mappings[profileIndex(profile)][player];
        unsigned result = 0;
        for (int target : gameTargetRange(profile, m.sixButton, m.controllerType)) {
            const auto configured = m.pad.value(target);
            bool& held = axisHeld[player][target];
            const bool pressed = configured.kind == PadBinding::Automatic ?
                automaticActive(device, target, m.deadzonePercent, held) :
                active(device, configured, m.deadzonePercent, held);
            if (pressed) result |= (1u << target);
        }
        return result;
    }
    void recompute(bool updateAim = false) {
        const auto assigned = assignments();
        std::array<bool, targetCount - 16> nextCommands{};
        for (int player = 0; player < playerCount; ++player) {
            const auto& m = mappings[profileIndex(profile)][player];
            const unsigned pad = connected(player) ? padMask(player, assigned[player]) : 0u;
            if (previews[player] != pad) {
                previews[player] = pad;
                emit owner->previewChanged(player, pad);
            }
            unsigned mask = pad;
            for (auto it = m.keyboard.cbegin(); it != m.keyboard.cend(); ++it)
                if (activeGameplayTarget(profile, m, it.key()) && keys.contains(it.value())) mask |= (1u << it.key());
            if (!enabled || capture || !connected(player)) mask = 0;
            if (masks[player] != mask) {
                masks[player] = mask;
                emit owner->maskChanged(player, mask);
            }
            if (enabled && !capture && assigned[player]) {
                for (int command = 16; command < targetCount; ++command) {
                    const PadBinding b = m.pad.value(command);
                    nextCommands[command - 16] |= active(assigned[player], b, m.deadzonePercent,
                                                         axisHeld[player][command]);
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
        capture = false; captureArmed = false; captureInstance = -1; capturePlayer = -1;
        emit owner->captureCancelled();
        recompute();
    }
    bool captureNeutral(Device* device) const {
        SDL_Joystick* joy = device->joystick;
        for (int i = 0; i < SDL_JoystickNumButtons(joy); ++i)
            if (SDL_JoystickGetButton(joy, i)) return false;
        for (int i = 0; i < SDL_JoystickNumHats(joy); ++i)
            if (SDL_JoystickGetHat(joy, i) != SDL_HAT_CENTERED) return false;
        for (int i = 0; i < SDL_JoystickNumAxes(joy); ++i) {
            const int value = SDL_JoystickGetAxis(joy, i);
            const int neutral = i < neutralAxes.size() ? neutralAxes[i] : 0;
            if (std::abs(value - neutral) > 8000) return false;
        }
        return true;
    }
    void updateCapture() {
        if (!capture) return;
        if (captureTimer.elapsed() >= captureMilliseconds) { cancelCapture(); return; }
        Device* device = findById(captureInstance);
        if (!device) { cancelCapture(); return; }
        if (!captureArmed) {
            neutralPolls = captureNeutral(device) ? neutralPolls + 1 : 0;
            if (neutralPolls >= 2) {
                // A wizard immediately re-arms while the previous input is
                // still held. Keep the last verified neutral for that device.
                device->captureNeutralAxes = neutralAxes;
                captureArmed = true;
                emit owner->captureReady();
            }
            return;
        }
        SDL_Joystick* joy = device->joystick;
        PadBinding found;
        for (int i = 0; i < SDL_JoystickNumButtons(joy); ++i)
            if (SDL_JoystickGetButton(joy, i)) { found = button(i); break; }
        if (found.kind == PadBinding::None) for (int i = 0; i < SDL_JoystickNumHats(joy); ++i) {
            const Uint8 value = SDL_JoystickGetHat(joy, i);
            for (const int direction : {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT})
                if (value & direction) { found = hat(i, direction); break; }
            if (found.kind != PadBinding::None) break;
        }
        if (found.kind == PadBinding::None) for (int i = 0; i < SDL_JoystickNumAxes(joy); ++i) {
            const int value = SDL_JoystickGetAxis(joy, i);
            const int neutral = i < neutralAxes.size() ? neutralAxes[i] : 0;
            if (std::abs(value - neutral) >= captureAxisThreshold &&
                std::abs(value) >= captureAxisThreshold) {
                found = axis(i, value > 0 ? 1 : -1);
                break;
            }
        }
        if (found.kind != PadBinding::None) {
            capture = false; captureArmed = false; captureInstance = -1; capturePlayer = -1;
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
    return validPlayer(player) ? d->mappings[profileIndex(profile)][player] : PlayerMapping{};
}
void InputManager::setMapping(ConsoleProfile profile, int player, const PlayerMapping& value) {
    if (!validPlayer(player)) return;
    auto m = value;
    m.deadzonePercent = std::clamp(m.deadzonePercent, 5, 60);
    m.controllerType = slotControllerType(profile, player, m.controllerType);
    if (player == 2 && profile != ConsoleProfile::MegaDrive) m.connected = false;
    if (m.aimSource != AimSource::LeftStick) m.aimSource = AimSource::Mouse;
    for (auto it = m.keyboard.begin(); it != m.keyboard.end(); ++it)
        it.value() = storedKeyboardKey(it.value());
    for (auto type = m.mouse.begin(); type != m.mouse.end(); ++type)
        for (auto button = type.value().begin(); button != type.value().end(); ++button)
            if (!validMouseButton(button.value())) button.value() = 0;
    const auto defaults = defaultMapping(profile, player);
    const auto& previous = d->mappings[profileIndex(profile)][player];
    if (profile == d->profile && (previous.sixButton != m.sixButton || previous.controllerType != m.controllerType))
        d->keys.clear(); // Release keys hidden by a type change, including their later key-up.
    d->aimTimer.invalidate();
    d->mappings[profileIndex(profile)][player] = m;
    QSettings settings(d->settingsPath, QSettings::IniFormat);
    const QString prefix = settingsPrefix(profile, player);
    settings.setValue(prefix + "device", m.deviceKey);
    settings.setValue(prefix + "connected", m.connected);
    settings.setValue(prefix + "sixButton", m.sixButton);
    settings.setValue(prefix + "controllerType", int(m.controllerType));
    settings.setValue(prefix + "aimSource", int(m.aimSource));
    if (m.showCrosshair) settings.remove(prefix + "showCrosshair");
    else settings.setValue(prefix + "showCrosshair", false);
    settings.setValue(prefix + "deadzone", m.deadzonePercent);
    for (int target = 0; target < targetCount; ++target) {
        const QString key = prefix + QStringLiteral("pad/%1").arg(target);
        if (m.pad.value(target) == defaults.pad.value(target)) settings.remove(key);
        else settings.setValue(key, bindingText(m.pad.value(target)));
        const QString keyKey = prefix + QStringLiteral("key/%1").arg(target);
        if (!gameplayTarget(target) || m.keyboard.value(target, 0) == defaults.keyboard.value(target, 0)) settings.remove(keyKey);
        else settings.setValue(keyKey, m.keyboard.value(target, 0));
    }
    for (ControllerType type : {ControllerType::LightPhaser, ControllerType::Menacer,
                                ControllerType::Justifier}) {
        if (validControllerType(profile, type) != type) continue;
        for (int target : gameTargetRange(profile, true, type)) {
            const QString key = prefix + QStringLiteral("mouse/%1/%2").arg(int(type)).arg(target);
            const int button = m.mouse.value(int(type)).value(target, 0);
            if (button == defaults.mouse.value(int(type)).value(target, 0)) settings.remove(key);
            else settings.setValue(key, button);
        }
    }
    settings.sync();
    if (profile == d->profile) {
        emit connectionChanged(unsigned(player), d->connected(player));
        if (player == 1) emit connectionChanged(2, d->connected(2));
        emit controllerTypeChanged(unsigned(player), m.sixButton);
        emit deviceTypeChanged(unsigned(player), int(m.controllerType));
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
    if (profile == ConsoleProfile::MegaDrive) {
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
    }
    return m;
}
QString InputManager::bindingName(const PadBinding& binding) {
    switch (binding.kind) {
    case PadBinding::Button: return QStringLiteral("Кнопка %1").arg(binding.index + 1);
    case PadBinding::Axis: return QStringLiteral("Ось %1 %2").arg(binding.index + 1).arg(binding.direction > 0 ? '+' : '-');
    case PadBinding::Hat: return QStringLiteral("Крестовина %1 (%2)").arg(binding.index + 1).arg(binding.direction);
    case PadBinding::Automatic: return QStringLiteral("Авто");
    case PadBinding::None: return QStringLiteral("Не назначено");
    }
    return QStringLiteral("Не назначено");
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
    if (validControllerType(profile, type) != ControllerType::Pad) {
        if (target == 1) return QStringLiteral("Курок");
        if (target == 2) return QStringLiteral("Перезарядка");
        if (target == 0) return QStringLiteral("Кнопка 2");
        if (target == 8) return QStringLiteral("Кнопка 3");
    }
    switch (target) {
    case 4: return QStringLiteral("Вверх");
    case 5: return QStringLiteral("Вниз");
    case 6: return QStringLiteral("Влево");
    case 7: return QStringLiteral("Вправо");
    case 0: return profile == ConsoleProfile::MegaDrive ? QStringLiteral("B") : QStringLiteral("1");
    case 8: return profile == ConsoleProfile::MegaDrive ? QStringLiteral("C") : QStringLiteral("2");
    case 1: return QStringLiteral("A");
    case 3: return profile == ConsoleProfile::MasterSystem ? QStringLiteral("Пауза консоли") : QStringLiteral("Start");
    case 2: return QStringLiteral("Mode");
    case 9: return QStringLiteral("Y");
    case 10: return QStringLiteral("X");
    case 11: return QStringLiteral("Z");
    case PauseCommand: return QStringLiteral("Пауза эмуляции");
    case FastForwardCommand: return QStringLiteral("Ускорение");
    case SaveCommand: return QStringLiteral("Сохранить состояние");
    case LoadCommand: return QStringLiteral("Загрузить состояние");
    case ResetCommand: return QStringLiteral("Мягкий сброс");
    case HardResetCommand: return QStringLiteral("Жёсткий сброс");
    case FrameAdvanceCommand: return QStringLiteral("Следующий кадр");
    case FullscreenCommand: return QStringLiteral("Полный экран");
    case MuteCommand: return QStringLiteral("Без звука");
    case NextSlotCommand: return QStringLiteral("Следующий слот");
    case PreviousSlotCommand: return QStringLiteral("Предыдущий слот");
    case MenuCommand: return QStringLiteral("Меню");
    case RewindCommand: return QStringLiteral("Перемотка назад");
    default: return QStringLiteral("Кнопка %1").arg(target);
    }
}
void InputManager::setProfile(ConsoleProfile profile) {
    if (d->profile == profile) return;
    d->profile = profile; d->axisHeld = {}; d->aimTimer.invalidate();
    for (unsigned player = 0; player < playerCount; ++player) {
        const auto& mapping = d->mappings[profileIndex(profile)][player];
        emit connectionChanged(player, d->connected(player));
        emit controllerTypeChanged(player, mapping.sixButton);
        emit deviceTypeChanged(player, int(mapping.controllerType));
    }
    emit configurationChanged();
    d->recompute();
}
ConsoleProfile InputManager::profile() const { return d->profile; }
bool InputManager::isConnected(unsigned player) const {
    return player < playerCount && d->connected(int(player));
}
bool InputManager::sixButton(unsigned player) const {
    return player < playerCount && d->mappings[profileIndex(d->profile)][player].sixButton;
}
ControllerType InputManager::controllerType(unsigned player) const {
    return player < playerCount ? d->mappings[profileIndex(d->profile)][player].controllerType : ControllerType::Pad;
}
bool InputManager::aimWithMouse(unsigned player) const {
    return player < playerCount && d->mappings[profileIndex(d->profile)][player].aimSource == AimSource::Mouse;
}
unsigned InputManager::mouseButtonMask(unsigned player, unsigned qtButtons) const {
    if (player >= playerCount || !d->enabled || d->capture) return 0;
    const auto& mapping = d->mappings[profileIndex(d->profile)][player];
    if (!d->connected(int(player)) || mapping.controllerType == ControllerType::Pad ||
        mapping.aimSource != AimSource::Mouse) return 0;
    const auto bindings = mapping.mouse.constFind(int(mapping.controllerType));
    if (bindings == mapping.mouse.cend()) return 0;
    unsigned mask = 0;
    for (int target : gameTargetRange(d->profile, mapping.sixButton, mapping.controllerType)) {
        const int button = bindings->value(target, 0);
        if (button && (qtButtons & unsigned(button))) mask |= 1u << target;
    }
    return mask;
}
void InputManager::setEnabled(bool enabled) {
    if (d->enabled == enabled) return;
    d->enabled = enabled;
    d->aimTimer.invalidate();
    if (!enabled) d->keys.clear();
    d->commandBlocked.fill(true);
    d->recompute();
}
int InputManager::keyboardKey(int logicalKey, quint32 nativeScanCode, KeyboardScanCodes codes) {
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
        if (scan >= 0x02 && scan <= 0x0d) return "1234567890-="[scan - 0x02];
        if (scan >= 0x10 && scan <= 0x1b) return "QWERTYUIOP[]"[scan - 0x10];
        if (scan >= 0x1e && scan <= 0x28) return "ASDFGHJKL;'"[scan - 0x1e];
        if (scan >= 0x2c && scan <= 0x35) return "ZXCVBNM,./"[scan - 0x2c];
        if (scan == 0x29) return Qt::Key_QuoteLeft;
        if (scan == 0x2b) return Qt::Key_Backslash;
        if (scan == 0x56) return Qt::Key_Less; // extra ISO key, distinct from US backslash
    }
    // Non-text keys and keypad keys retain Qt's identity. Native codes may be
    // absent in synthetic/accessibility events or unsupported platform plugins.
    return legacyKeyboardKey(logicalKey);
}
bool InputManager::reservedKeyboardKey(int key) {
    return (key >= Qt::Key_0 && key <= Qt::Key_9) ||
           key == Qt::Key_Tab || key == Qt::Key_Escape ||
           key == Qt::Key_P || key == Qt::Key_M || key == Qt::Key_R ||
           key == Qt::Key_F5 || key == Qt::Key_F8 || key == Qt::Key_F12 ||
           key == Qt::Key_Period;
}
bool InputManager::keyEvent(int key, bool pressed) {
    key = legacyKeyboardKey(key);
    if (reservedKeyboardKey(key)) return false;
    bool bound = false;
    for (int player = 0; player < playerCount; ++player) {
        const auto& mapping = d->mappings[profileIndex(d->profile)][player];
        for (auto it = mapping.keyboard.cbegin(); it != mapping.keyboard.cend(); ++it)
            if (activeGameplayTarget(d->profile, mapping, it.key()) && it.value() == key && key != 0) bound = true;
    }
    if (!bound) return false;
    if (!d->enabled || d->capture) return true;
    if (pressed) d->keys.insert(key); else d->keys.remove(key);
    d->recompute();
    return true;
}
void InputManager::releaseKeyboard() { d->keys.clear(); d->recompute(); }
void InputManager::beginCapture(const QString& deviceKey, int player) {
    // Replacing an in-progress capture is a re-arm, not a cancellation event.
    d->capture = false; d->captureArmed = false;
    if (!validPlayer(player)) { emit captureCancelled(); return; }
    d->refreshDevices();
    Impl::Device* device = nullptr;
    if (deviceKey == "auto") device = d->assignments()[player];
    else device = d->findByKey(deviceKey);
    if (!device) { emit captureCancelled(); return; }
    d->capture = true; d->captureArmed = false; d->captureInstance = device->instanceId;
    d->commandBlocked.fill(true);
    d->capturePlayer = player; d->neutralPolls = 0;
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
    d->captureTimer.start();
    d->aimTimer.invalidate();
    d->recompute();
}
void InputManager::cancelCapture() { d->cancelCapture(); }
bool InputManager::capturing() const { return d->capture; }
void InputManager::poll() {
    d->refreshDevices();
    d->updateCapture();
    d->recompute(true);
}
