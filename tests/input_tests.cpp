#include "input/InputManager.h"

#include <QCoreApplication>
#include <QSettings>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <SDL.h>

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }

QString keyForId(const QVector<InputDevice>& devices, SDL_JoystickID id) {
    for (const auto& device : devices)
        if (device.instanceId == id) return device.key;
    throw std::runtime_error("virtual joystick instance was not listed");
}

QSet<int> instanceIds(const QVector<InputDevice>& devices) {
    QSet<int> result;
    for (const auto& device : devices) result.insert(device.instanceId);
    return result;
}

struct VirtualPad {
    int index = -1;
    SDL_JoystickID id = -1;
    SDL_Joystick* joystick = nullptr;
    explicit VirtualPad(SDL_JoystickType type = SDL_JOYSTICK_TYPE_GAMECONTROLLER, int buttons = 12) {
        index = SDL_JoystickAttachVirtual(type, 6, buttons, 1);
        require(index >= 0, "could not attach virtual joystick");
        joystick = SDL_JoystickOpen(index);
        require(joystick != nullptr, "could not open virtual joystick");
        id = SDL_JoystickInstanceID(joystick);
    }
    ~VirtualPad() { close(); }
    void close() {
        if (joystick) { SDL_JoystickClose(joystick); joystick = nullptr; }
        if (index >= 0) {
            for (int current = 0; current < SDL_NumJoysticks(); ++current)
                if (SDL_JoystickGetDeviceInstanceID(current) == id) {
                    SDL_JoystickDetachVirtual(current);
                    break;
                }
            index = -1;
        }
    }
    void button(int index, bool pressed) {
        require(SDL_JoystickSetVirtualButton(joystick, index, pressed ? 1 : 0) == 0,
                "could not set virtual button");
    }
    void axis(int index, Sint16 value) {
        require(SDL_JoystickSetVirtualAxis(joystick, index, value) == 0,
                "could not set virtual axis");
    }
    void hat(int value) {
        require(SDL_JoystickSetVirtualHat(joystick, 0, value) == 0,
                "could not set virtual hat");
    }
};

void testInput() {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create settings directory");
    const QString settingsPath = temp.path() + "/settings.ini";
    InputManager manager(settingsPath);
    const QSet<int> baseline = instanceIds(manager.devices());
    std::array<unsigned, 2> masks{};
    std::array<unsigned, 2> previews{};
    int ffPresses = 0, ffReleases = 0, resetPresses = 0, captures = 0, cancellations = 0;
    QStringList notices;
    PadBinding lastCapture;
    QObject::connect(&manager, &InputManager::maskChanged, &manager,
                     [&](unsigned player, unsigned mask) { masks.at(player) = mask; });
    QObject::connect(&manager, &InputManager::previewChanged, &manager,
                     [&](unsigned player, unsigned mask) { previews.at(player) = mask; });
    QObject::connect(&manager, &InputManager::commandChanged, &manager,
                     [&](int id, bool pressed) {
                         if (id == FastForwardCommand) pressed ? ++ffPresses : ++ffReleases;
                         if (id == ResetCommand && pressed) ++resetPresses;
                     });
    QObject::connect(&manager, &InputManager::bindingCaptured, &manager,
                     [&](const PadBinding& binding) { ++captures; lastCapture = binding; });
    QObject::connect(&manager, &InputManager::captureCancelled, &manager,
                     [&] { ++cancellations; });
    QObject::connect(&manager, &InputManager::notification, &manager,
                     [&](const QString& message) { notices.push_back(message); });

    VirtualPad first;
    manager.poll();
    require(manager.devices().size() == baseline.size() + 1,
            "first virtual joystick was not discovered");
    require(!notices.isEmpty() && notices.back().contains(QStringLiteral("подключён")),
            "hotplug did not announce connected joystick");
    VirtualPad second;
    manager.poll();
    const auto devices = manager.devices();
    const QString firstKey = keyForId(devices, first.id);
    const QString secondKey = keyForId(devices, second.id);
    require(devices.size() == baseline.size() + 2 && firstKey != secondKey,
            "duplicate joysticks lack distinct keys");
    if (!baseline.isEmpty()) {
        std::fputs("input tests: physical gamepads present; skipping auto order assertion\n", stderr);
        auto firstMapping = manager.mapping(ConsoleProfile::MegaDrive, 0);
        auto secondMapping = manager.mapping(ConsoleProfile::MegaDrive, 1);
        firstMapping.deviceKey = firstKey;
        secondMapping.deviceKey = secondKey;
        manager.setMapping(ConsoleProfile::MegaDrive, 0, firstMapping);
        manager.setMapping(ConsoleProfile::MegaDrive, 1, secondMapping);
    }
    first.button(0, true);
    manager.poll();
    require((masks[0] & (1u << 0)) && masks[1] == 0,
            "first virtual joystick did not control only player one");
    if (baseline.isEmpty()) {
        require(manager.mapping(ConsoleProfile::MegaDrive, 0).deviceKey == "auto" &&
                    manager.mapping(ConsoleProfile::MegaDrive, 1).deviceKey == "auto",
                "auto assignment test did not use automatic device selection");
    }
    second.button(1, true);
    manager.poll();
    require(masks[1] & (1u << 8), "second joystick did not control player two");
    require(ffPresses == 0, "default gameplay button triggered a command");

    require(manager.keyEvent(Qt::Key_X, true), "default gameplay key not recognized");
    first.button(0, false);
    manager.poll();
    require(masks[0] & 1u, "keyboard and pad did not OR together");
    manager.releaseKeyboard();
    require(!(masks[0] & 1u), "releaseKeyboard left gameplay button held");
    require(manager.mapping(ConsoleProfile::MegaDrive, 0).pad.value(0).kind == PadBinding::Automatic,
            "untouched mapping did not retain Automatic kind");
    first.axis(1, -25000);
    manager.poll();
    require(masks[0] & (1u << 4), "normalized controller left stick did not map up");
    first.axis(1, 0);
    manager.poll();
    require(!(masks[0] & (1u << 4)), "normalized controller axis did not release");
    auto mapping = manager.mapping(ConsoleProfile::MegaDrive, 0);
    mapping.pad[5] = {PadBinding::Hat, 0, SDL_HAT_UP}; // explicit raw hat override
    manager.setMapping(ConsoleProfile::MegaDrive, 0, mapping);
    first.hat(SDL_HAT_UP);
    manager.poll();
    require(masks[0] & (1u << 5), "raw hat binding was not mapped");
    first.hat(SDL_HAT_CENTERED);
    manager.poll();
    require(!(masks[0] & (1u << 5)), "hat release did not clear mask");

    mapping.pad[6] = {PadBinding::Axis, 0, -1};
    mapping.pad[FastForwardCommand] = {PadBinding::Button, 3, 0};
    manager.setMapping(ConsoleProfile::MegaDrive, 0, mapping);
    first.axis(0, -25000);
    first.button(3, true);
    manager.poll();
    require(masks[0] & (1u << 6), "negative axis did not activate mapped direction");
    require(ffPresses == 1, "held fast forward did not emit press edge");
    manager.poll();
    require(ffPresses == 1, "held fast forward repeated press edge");
    first.axis(0, -5000);
    manager.poll();
    require(!(masks[0] & (1u << 6)), "axis did not release below hysteresis threshold");
    first.button(0, true);
    manager.poll();

    manager.setEnabled(false);
    require(masks[0] == 0 && masks[1] == 0 && ffReleases == 1,
            "focus loss did not release game inputs and command");
    require(previews[0] != 0, "disabled gameplay stopped pad preview");
    first.button(0, false);
    first.button(3, false);
    second.button(1, false);
    manager.poll();

    mapping.pad[ResetCommand] = {PadBinding::Button, 4, 0};
    manager.setMapping(ConsoleProfile::MegaDrive, 0, mapping);
    manager.setEnabled(true);
    manager.poll();
    first.button(4, true);
    manager.poll();
    require(resetPresses == 1, "reset command did not emit first press");
    manager.setEnabled(false);
    manager.setEnabled(true);
    manager.poll();
    require(resetPresses == 1, "focus regain retriggered held reset command");
    first.button(4, false);
    manager.poll();
    first.button(4, true);
    manager.poll();
    require(resetPresses == 2, "reset command did not rearm after release");
    first.button(4, false);
    manager.setEnabled(false);
    manager.poll();

    first.button(0, true);
    manager.beginCapture("auto", 0);
    manager.poll();
    require(manager.capturing() && captures == 0, "capture accepted held button before neutral");
    first.button(0, false);
    manager.poll(); manager.poll();
    first.button(5, true);
    manager.poll();
    require(!manager.capturing() && captures == 1 &&
                lastCapture == PadBinding{PadBinding::Button, 5, 0},
            "neutral-armed capture missed new button");
    manager.beginCapture("auto", 0);
    manager.poll();
    require(manager.capturing() && captures == 1, "capture did not rearm after held button");
    first.button(5, false);
    manager.poll(); manager.poll();
    first.axis(2, -32768); // common idle value for an analog trigger
    manager.beginCapture("auto", 0);
    require(cancellations == 0, "re-arming capture emitted a cancellation");
    manager.poll(); manager.poll();
    require(manager.capturing() && captures == 1, "idle trigger was captured as input");
    first.axis(2, 32767);
    manager.poll();
    require(captures == 2 && lastCapture == PadBinding{PadBinding::Axis, 2, 1},
            "trigger movement was not captured");

    first.axis(2, -32768);
    manager.beginCapture("auto", 0);
    first.close();
    manager.poll();
    require(!notices.isEmpty() && notices.back().contains(QStringLiteral("отключён")),
            "hotplug did not announce disconnected joystick");
    require(!manager.capturing() && cancellations == 1,
            "capture was not cancelled on joystick disconnect");
    require(!(masks[1] & (1u << 8)), "disconnected joystick left player mask held");

    mapping.pad[0] = {PadBinding::Button, 5, 0};
    manager.setMapping(ConsoleProfile::MegaDrive, 0, mapping);
    {
        InputManager reloaded(settingsPath);
        const auto saved = reloaded.mapping(ConsoleProfile::MegaDrive, 0);
        require(saved.pad.value(0) == PadBinding{PadBinding::Button, 5, 0},
                "custom mapping did not persist");
        require(saved.pad.value(FastForwardCommand) == PadBinding{PadBinding::Button, 3, 0},
                "command mapping did not persist");
    }
    VirtualPad replacement;
    manager.poll();
    const auto afterReplacement = manager.devices();
    require(afterReplacement.size() == baseline.size() + 2 &&
                keyForId(afterReplacement, second.id) != keyForId(afterReplacement, replacement.id),
            "duplicate GUID reused a live device key after reconnect");
    replacement.close();
    second.close();
    manager.poll();
    require(instanceIds(manager.devices()) == baseline,
            "disconnected virtual joysticks remained listed");
}

void testLegacyKeyboardSettings() {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create legacy settings directory");
    const QString path = temp.path() + "/settings.ini";
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue("keys/button_0", Qt::Key_Q);
        settings.sync();
    }
    InputManager manager(path);
    require(manager.mapping(ConsoleProfile::MegaDrive, 0).keyboard.value(0) == Qt::Key_Q,
            "old keyboard setting was not migrated to Mega Drive");
    require(manager.mapping(ConsoleProfile::MasterSystem, 0).keyboard.value(0) == Qt::Key_Q,
            "old keyboard setting was not migrated to Master System");
    auto m = manager.mapping(ConsoleProfile::MegaDrive, 0);
    m.pad[0] = {PadBinding::Button, 0, 0}; // explicit raw index equal to old fallback
    manager.setMapping(ConsoleProfile::MegaDrive, 0, m);
    InputManager reloaded(path);
    require(reloaded.mapping(ConsoleProfile::MegaDrive, 0).pad.value(0) ==
                PadBinding{PadBinding::Button, 0, 0},
            "explicit raw binding equal to fallback was lost");
}

void testUnknownRawJoystick() {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create raw joystick settings directory");
    InputManager manager(temp.path() + "/settings.ini");
    const int baselineCount = manager.devices().size();
    unsigned mask = 0;
    QObject::connect(&manager, &InputManager::maskChanged, &manager,
                     [&](unsigned player, unsigned value) { if (player == 0) mask = value; });
    VirtualPad pad(SDL_JOYSTICK_TYPE_UNKNOWN);
    manager.poll();
    const auto listed = manager.devices();
    require(listed.size() == baselineCount + 1, "raw joystick was not discovered");
    require(SDL_IsGameController(pad.index) == SDL_FALSE,
            "raw joystick fixture was recognized as a GameController");
    auto mapping = manager.mapping(ConsoleProfile::MegaDrive, 0);
    mapping.deviceKey = keyForId(listed, pad.id);
    manager.setMapping(ConsoleProfile::MegaDrive, 0, mapping);
    pad.button(0, true);
    pad.hat(SDL_HAT_UP);
    pad.axis(0, -25000);
    manager.poll();
    require((mask & (1u << 0)) && (mask & (1u << 4)) && (mask & (1u << 6)),
            "raw defaults did not combine button, hat and stick");
    pad.button(0, false);
    pad.hat(SDL_HAT_CENTERED);
    pad.axis(0, 0);
    manager.poll();
    require(mask == 0, "raw joystick release left game buttons held");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        testInput();
        testLegacyKeyboardSettings();
        testUnknownRawJoystick();
        std::puts("input tests passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "input test failed: %s\n", e.what());
        return 1;
    }
}
