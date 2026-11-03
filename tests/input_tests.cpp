#include "input/InputManager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSettings>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <SDL.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
static_assert(PauseCommand == 16 && ResetCommand == 20 && MenuCommand == 25 &&
              RewindCommand == 26 && HardResetCommand == 27,
              "appended commands must preserve existing saved target IDs");
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

void testConnectedPorts() {
    QTemporaryDir temp;
    require(temp.isValid(), "connection settings directory");
    const QString path=temp.filePath("settings.ini");
    {
        QSettings legacy(path,QSettings::IniFormat);
        legacy.setValue("input/md/player2/device","none");
        legacy.setValue("input/md/player2/key/0",Qt::Key_Q);
    }
    InputManager manager(path);
    for (auto profile : {ConsoleProfile::MegaDrive,ConsoleProfile::MasterSystem,
                         ConsoleProfile::GameGear,ConsoleProfile::SG1000}) {
        require(manager.mapping(profile,0).connected && !manager.mapping(profile,1).connected,
                "absent connection settings did not default to P1 only");
    }
    require(manager.isConnected(0) && !manager.isConnected(1),"initial connection query");
    unsigned mask=0;
    QMap<unsigned, bool> connections;
    QObject::connect(&manager,&InputManager::maskChanged,&manager,
                     [&](unsigned p,unsigned value){if(p==0)mask=value;});
    QObject::connect(&manager,&InputManager::connectionChanged,&manager,
                     [&](unsigned player,bool connected){connections[player]=connected;});
    auto first=manager.mapping(ConsoleProfile::MegaDrive,0);
    first.deviceKey="none";first.pad[0]={PadBinding::Button,7,0};
    manager.setMapping(ConsoleProfile::MegaDrive,0,first);
    require(manager.keyEvent(Qt::Key_X,true) && mask==1,"physical none disabled keyboard input");
    first.connected=false;manager.setMapping(ConsoleProfile::MegaDrive,0,first);
    require(!manager.isConnected(0) && mask==0,"disconnected virtual port left gameplay input held");
    first.connected=true;manager.setMapping(ConsoleProfile::MegaDrive,0,first);
    require(manager.isConnected(0) && mask==1,"reconnect did not retain keyboard binding");
    manager.releaseKeyboard();
    auto second=manager.mapping(ConsoleProfile::MegaDrive,1);
    second.connected=true;manager.setMapping(ConsoleProfile::MegaDrive,1,second);
    auto sms=manager.mapping(ConsoleProfile::MasterSystem,0);sms.connected=false;
    connections.clear();
    manager.setMapping(ConsoleProfile::MasterSystem,0,sms);
    require(connections.isEmpty(),"inactive profile changed live ports");
    manager.setProfile(ConsoleProfile::MasterSystem);
    require(connections.size() == int(MaxControllerPlayers) &&
            !manager.isConnected(0) && !manager.isConnected(1),
            "profile switch did not publish all logical port connections");
    for (unsigned player = 0; player < MaxControllerPlayers; ++player)
        require(connections.contains(player) && connections.value(player) == manager.isConnected(player),
                "profile switch published an incorrect logical port state");
    InputManager reopened(path);
    require(reopened.mapping(ConsoleProfile::MegaDrive,0).connected &&
            reopened.mapping(ConsoleProfile::MegaDrive,1).connected &&
            !reopened.mapping(ConsoleProfile::MasterSystem,0).connected,
            "per-profile connection settings were not persisted");
    const auto preserved=reopened.mapping(ConsoleProfile::MegaDrive,0);
    require(preserved.deviceKey=="none" && preserved.keyboard.value(0)==Qt::Key_X &&
            preserved.pad.value(0)==PadBinding{PadBinding::Button,7,0} &&
            reopened.mapping(ConsoleProfile::MegaDrive,1).keyboard.value(0)==Qt::Key_Q,
            "connection changes lost existing device/key/pad bindings");
}

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
    auto enabledSecond = manager.mapping(ConsoleProfile::MegaDrive, 1);
    enabledSecond.connected = true;
    manager.setMapping(ConsoleProfile::MegaDrive, 1, enabledSecond);
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

    const auto triggerBinding = SDL_GameControllerGetBindForAxis(
        SDL_GameControllerFromInstanceID(first.id), SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    require(triggerBinding.bindType == SDL_CONTROLLER_BINDTYPE_AXIS,
            "virtual controller does not have an analog trigger");
    const int triggerAxis = triggerBinding.value.axis;
    first.axis(triggerAxis, -32768); // A real mapped trigger rests at its negative endpoint.
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
    first.axis(triggerAxis, -32768); // common idle value for an analog trigger
    manager.beginCapture("auto", 0);
    require(cancellations == 0, "re-arming capture emitted a cancellation");
    manager.poll(); manager.poll();
    require(manager.capturing() && captures == 1, "idle trigger was captured as input");
    first.axis(triggerAxis, 32767);
    manager.poll();
    require(captures == 2 && lastCapture == PadBinding{PadBinding::Axis, triggerAxis, 1},
            "trigger movement was not captured");

    first.axis(triggerAxis, -32768);
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

void testSequentialAxisCapture() {
    for (const auto type : {SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_JOYSTICK_TYPE_UNKNOWN}) {
        QTemporaryDir temp;
        require(temp.isValid(), "cannot create sequential capture settings directory");
        InputManager manager(temp.path() + "/settings.ini");
        VirtualPad pad(type);
        manager.poll();
        const QString key = keyForId(manager.devices(), pad.id);
        int triggerAxis = -1;
        if (type == SDL_JOYSTICK_TYPE_GAMECONTROLLER) {
            const auto binding = SDL_GameControllerGetBindForAxis(
                SDL_GameControllerFromInstanceID(pad.id), SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            require(binding.bindType == SDL_CONTROLLER_BINDTYPE_AXIS,
                    "sequential capture fixture has no analog trigger");
            triggerAxis = binding.value.axis;
            pad.axis(triggerAxis, -32768);
            manager.poll();
        }
        QVector<PadBinding> captured;
        QObject::connect(&manager, &InputManager::bindingCaptured, &manager,
                         [&](const PadBinding& binding) { captured.push_back(binding); });
        // The right stick/raw higher-numbered axis is centered like axis 1.
        // The next wizard step begins before the previous direction is released.
        for (const int axis : {1, 3}) {
            manager.beginCapture(key, 0);
            manager.poll(); manager.poll();
            pad.axis(axis, -32768);
            manager.poll();
            require(!manager.capturing() && !captured.isEmpty() &&
                        captured.back() == PadBinding{PadBinding::Axis, axis, -1},
                    "sequential capture missed the first axis direction");
            const auto before = captured.size();
            manager.beginCapture(key, 0);
            manager.poll(); // Release before two polls can mistake the held endpoint for neutral.
            require(manager.capturing() && captured.size() == before,
                    "next capture accepted the previous held direction");
            pad.axis(axis, 0);
            manager.poll(); manager.poll();
            pad.axis(axis, 32767);
            manager.poll();
            require(!manager.capturing() && captured.size() == before + 1 &&
                        captured.back() == PadBinding{PadBinding::Axis, axis, 1},
                    "sequential capture missed Down after releasing Up on a centered axis");
            pad.axis(axis, 0);
            manager.poll();
        }
        if (triggerAxis >= 0) {
            manager.beginCapture(key, 0);
            manager.poll(); manager.poll();
            pad.axis(triggerAxis, 32767);
            manager.poll();
            require(!manager.capturing() &&
                        captured.back() == PadBinding{PadBinding::Axis, triggerAxis, 1},
                    "mapped trigger was not captured after centered stick bindings");
            const auto before = captured.size();
            manager.beginCapture(key, 0); // The previous trigger is still pressed.
            manager.poll();
            pad.axis(triggerAxis, -32768);
            manager.poll(); manager.poll();
            pad.button(9, true);
            manager.poll();
            require(!manager.capturing() && captured.size() == before + 1 &&
                        captured.back() == PadBinding{PadBinding::Button, 9, 0},
                    "held trigger poisoned the next wizard step's neutral baseline");
            pad.button(9, false);
            manager.poll();
        }
    }
}

void testIndependentCapture() {
    for (const auto type : {SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_JOYSTICK_TYPE_UNKNOWN}) {
        QTemporaryDir temp;
        require(temp.isValid(), "independent capture directory");
        InputManager manager(temp.filePath("settings.ini"));
        VirtualPad pad(type);
        manager.poll();
        const auto key=keyForId(manager.devices(),pad.id);
        QVector<PadBinding> captured;
        QObject::connect(&manager,&InputManager::bindingCaptured,&manager,
                         [&](const PadBinding& value) { captured.append(value); });
        // Configuring the second logical port must accept a button even when
        // another button, a hat and an analog axis remain held.
        pad.axis(1,-32768); pad.button(0,true); pad.hat(SDL_HAT_UP);
        manager.poll(); manager.beginCapture(key,1);
        for (int i=0;i<4;++i) manager.poll();
        require(captured.isEmpty(), "capture accepted an input already held at entry");
        pad.button(7,true); manager.poll();
        require(captured.size()==1 && captured.back()==PadBinding{PadBinding::Button,7,0},
                "held axis/button/hat blocked a fresh second-player button");
        manager.beginCapture(key,1); manager.poll();
        pad.button(8,true); manager.poll();
        require(captured.size()==2 && captured.back()==PadBinding{PadBinding::Button,8,0},
                "previous wizard button blocked a different fresh button");
        pad.button(0,false);pad.button(7,false);pad.button(8,false);
        pad.hat(SDL_HAT_CENTERED);pad.axis(1,0);manager.poll();
        manager.beginCapture(key,1);pad.axis(1,-32768);manager.poll();
        require(captured.size()==3 && captured.back()==PadBinding{PadBinding::Axis,1,-1},
                "centered axis did not bind Up");
        manager.beginCapture(key,1);manager.poll();manager.poll();
        require(captured.size()==3,"wizard rebound the previous held axis direction");
        pad.axis(1,32767);manager.poll();
        require(captured.size()==4 && captured.back()==PadBinding{PadBinding::Axis,1,1},
                "direct Up-to-Down movement was lost between polls");
        pad.axis(1,0);pad.axis(0,12000);pad.hat(SDL_HAT_LEFT);manager.poll();
        manager.beginCapture(key,1);manager.poll();
        pad.hat(SDL_HAT_RIGHT);manager.poll();
        require(captured.size()==5 && captured.back()==PadBinding{PadBinding::Hat,0,SDL_HAT_RIGHT},
                "drifting stick or held hat blocked a new hat direction");
        manager.beginCapture(key,1);manager.poll();
        pad.button(9,true);manager.poll();
        require(captured.size()==6 && captured.back()==PadBinding{PadBinding::Button,9,0},
                "drifting stick blocked button capture");
        pad.button(0,true); manager.poll();
        const PadBinding autoB{PadBinding::Automatic,0,0};
        require(manager.capturedBindingConflicts(ConsoleProfile::MegaDrive,key,0,autoB,
                                                {PadBinding::Button,0,0}),
                "automatic face-button conflict was not detected");
        require(!manager.capturedBindingConflicts(ConsoleProfile::MegaDrive,key,0,autoB,
                                                 {PadBinding::Button,1,0}),
                "unrelated automatic face button was removed");
        pad.axis(1,-32768);manager.poll();
        require(manager.capturedBindingConflicts(ConsoleProfile::MegaDrive,key,4,
                    {PadBinding::Automatic,4,0},{PadBinding::Axis,1,-1}),
                "automatic stick direction conflict was not detected");
        require(!manager.capturedBindingConflicts(ConsoleProfile::MegaDrive,key,5,
                    {PadBinding::Automatic,5,0},{PadBinding::Axis,1,-1}),
                "automatic opposite direction was incorrectly removed");
    }
}

void testRewindCommand() {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create rewind command settings directory");
    const QString path = temp.path() + "/settings.ini";
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue("input/md/player1/pad/25", "b:10"); // existing Menu ID
        settings.sync();
    }
    InputManager manager(path);
    VirtualPad pad;
    manager.poll();
    auto mapping = manager.mapping(ConsoleProfile::MegaDrive, 0);
    mapping.deviceKey = keyForId(manager.devices(), pad.id);
    for (int target : InputManager::gameTargets(ConsoleProfile::MegaDrive))
        mapping.pad[target] = {};
    mapping.pad[RewindCommand] = {PadBinding::Button, 11, 0};
    manager.setMapping(ConsoleProfile::MegaDrive, 0, mapping);
    manager.poll(); // A neutral poll arms the newly added command.
    int presses = 0, releases = 0;
    QObject::connect(&manager, &InputManager::commandChanged, &manager,
                     [&](int command, bool pressed) {
                         if (command == RewindCommand) pressed ? ++presses : ++releases;
                     });

    pad.button(11, true);
    manager.poll();
    manager.poll();
    require(presses == 1 && releases == 0,
            "held rewind did not produce exactly one press edge");
    pad.button(11, false);
    manager.poll();
    require(presses == 1 && releases == 1, "rewind button did not release");

    pad.button(11, true);
    manager.poll();
    manager.setEnabled(false);
    require(presses == 2 && releases == 2,
            "focus loss did not release held rewind");
    manager.setEnabled(true);
    manager.poll();
    require(presses == 2, "focus regain retriggered held rewind");
    pad.button(11, false);
    manager.poll();
    pad.button(11, true);
    manager.poll();
    require(presses == 3 && releases == 2,
            "rewind did not rearm after focus regained and button released");

    manager.beginCapture(mapping.deviceKey, 0);
    require(releases == 3, "input capture did not release held rewind");
    manager.cancelCapture();
    manager.poll();
    require(presses == 3, "ending capture retriggered held rewind");
    pad.button(11, false);
    manager.poll();
    pad.button(11, true);
    manager.poll();
    require(presses == 4, "rewind did not rearm after capture");
    pad.close();
    manager.poll();
    require(releases == 4, "disconnected joystick left rewind held");

    InputManager reloaded(path);
    const auto saved = reloaded.mapping(ConsoleProfile::MegaDrive, 0);
    require(saved.pad.value(RewindCommand) == PadBinding{PadBinding::Button, 11, 0},
            "rewind command mapping did not persist");
    require(saved.pad.value(MenuCommand) == PadBinding{PadBinding::Button, 10, 0},
            "appending rewind changed existing Menu command mapping");
}

void testHardResetCommand() {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create hard reset settings directory");
    const QString path = temp.path()+"/settings.ini";
    {
        QSettings settings(path,QSettings::IniFormat);
        settings.setValue("input/md/player1/pad/20","b:4"); // Existing soft reset ID.
        settings.setValue("input/md/player1/pad/26","b:10"); // Existing rewind ID.
        settings.sync();
    }
    InputManager manager(path);
    VirtualPad pad;
    manager.poll();
    auto mapping = manager.mapping(ConsoleProfile::MegaDrive,0);
    mapping.deviceKey = keyForId(manager.devices(),pad.id);
    for (int target : InputManager::gameTargets(ConsoleProfile::MegaDrive)) mapping.pad[target] = {};
    mapping.pad[HardResetCommand] = {PadBinding::Button,11,0};
    manager.setMapping(ConsoleProfile::MegaDrive,0,mapping);
    manager.poll();
    int hardPresses = 0, hardReleases = 0, softPresses = 0;
    QObject::connect(&manager,&InputManager::commandChanged,&manager,[&](int command,bool pressed) {
        if (command == HardResetCommand) pressed ? ++hardPresses : ++hardReleases;
        if (command == ResetCommand && pressed) ++softPresses;
    });
    pad.button(11,true); manager.poll(); manager.poll(); manager.poll();
    require(hardPresses == 1 && !hardReleases && !softPresses,
            "held hard reset did not produce exactly one distinct press");
    manager.setEnabled(false); manager.setEnabled(true); manager.poll();
    require(hardPresses == 1 && hardReleases == 1,
            "focus regained retriggered held hard reset");
    pad.button(11,false); manager.poll();
    pad.button(11,true); manager.poll(); manager.poll();
    require(hardPresses == 2 && hardReleases == 1,
            "hard reset did not rearm after release");
    pad.button(11,false); manager.poll();
    pad.button(4,true); manager.poll();
    require(softPresses == 1 && hardPresses == 2,"old soft reset mapping changed command ID");
    pad.button(4,false); manager.poll();
    InputManager reloaded(path);
    const auto saved = reloaded.mapping(ConsoleProfile::MegaDrive,0);
    require(saved.pad.value(HardResetCommand) == PadBinding{PadBinding::Button,11,0},
            "hard reset binding did not persist");
    require(saved.pad.value(ResetCommand) == PadBinding{PadBinding::Button,4,0} &&
            saved.pad.value(RewindCommand) == PadBinding{PadBinding::Button,10,0},
            "adding hard reset changed existing soft reset or rewind bindings");
}

void testReservedRewindKey() {
    using Codes = KeyboardScanCodes;
    for (const auto codes : {Codes::Windows, Codes::Xkb}) {
        const int scan = 0x13 + (codes == Codes::Xkb ? 8 : 0);
        for (const int logical : {int(Qt::Key_R), 0x041a}) {
            const int key = InputManager::keyboardKey(logical, scan, codes);
            require(key == Qt::Key_R && InputManager::reservedKeyboardKey(key),
                    "physical Russian/English R was not reserved for rewind");
        }
    }
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create reserved rewind settings directory");
    const QString path = temp.path() + "/settings.ini";
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue("input/md/player1/device", "none");
        settings.setValue("input/md/player1/key/0", Qt::Key_R);
        settings.setValue("input/md/player1/key/8", 0x041a); // Russian К -> R
        settings.setValue("keys/button_0", 0x043a); // legacy lowercase к -> R
        settings.sync();
    }
    InputManager manager(path);
    require(manager.mapping(ConsoleProfile::MegaDrive, 0).keyboard.value(0) == 0 &&
                manager.mapping(ConsoleProfile::MegaDrive, 0).keyboard.value(8) == 0,
            "saved English/Russian R remained bound to gameplay");
    for (const auto profile : {ConsoleProfile::MasterSystem, ConsoleProfile::GameGear,
                              ConsoleProfile::SG1000})
        require(manager.mapping(profile, 0).keyboard.value(0) == 0,
                "legacy Russian R remained bound to gameplay");

    auto mapping = manager.mapping(ConsoleProfile::MegaDrive, 0);
    mapping.keyboard[0] = Qt::Key_R;
    mapping.keyboard[8] = 0x041a;
    manager.setMapping(ConsoleProfile::MegaDrive, 0, mapping);
    require(manager.mapping(ConsoleProfile::MegaDrive, 0).keyboard.value(0) == 0 &&
                manager.mapping(ConsoleProfile::MegaDrive, 0).keyboard.value(8) == 0,
            "mapping update accepted reserved English/Russian R");
    require(!manager.keyEvent(Qt::Key_R, true) && !manager.keyEvent(0x041a, true),
            "reserved rewind key was treated as gameplay input");
    InputManager reloaded(path);
    require(reloaded.mapping(ConsoleProfile::MegaDrive, 0).keyboard.value(0) == 0 &&
                reloaded.mapping(ConsoleProfile::MegaDrive, 0).keyboard.value(8) == 0,
            "reserved rewind keyboard mappings reappeared after reload");
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

void testPhysicalKeyboard() {
    for (int key : {Qt::Key_Plus,Qt::Key_Equal,Qt::Key_Minus})
        require(InputManager::reservedKeyboardKey(key),"master volume key was not reserved");

    using Codes = KeyboardScanCodes;
    struct Key { int scan; int english; int russian; };
    const Key keys[] = {
        {0x10, Qt::Key_Q, 0x0419}, {0x11, Qt::Key_W, 0x0426},
        {0x12, Qt::Key_E, 0x0423}, {0x13, Qt::Key_R, 0x041a},
        {0x14, Qt::Key_T, 0x0415}, {0x15, Qt::Key_Y, 0x041d},
        {0x16, Qt::Key_U, 0x0413}, {0x17, Qt::Key_I, 0x0428},
        {0x18, Qt::Key_O, 0x0429}, {0x19, Qt::Key_P, 0x0417},
        {0x1a, Qt::Key_BracketLeft, 0x0425}, {0x1b, Qt::Key_BracketRight, 0x042a},
        {0x1e, Qt::Key_A, 0x0424}, {0x1f, Qt::Key_S, 0x042b},
        {0x20, Qt::Key_D, 0x0412}, {0x21, Qt::Key_F, 0x0410},
        {0x22, Qt::Key_G, 0x041f}, {0x23, Qt::Key_H, 0x0420},
        {0x24, Qt::Key_J, 0x041e}, {0x25, Qt::Key_K, 0x041b},
        {0x26, Qt::Key_L, 0x0414}, {0x27, Qt::Key_Semicolon, 0x0416},
        {0x28, Qt::Key_Apostrophe, 0x042d}, {0x29, Qt::Key_QuoteLeft, 0x0401},
        {0x2c, Qt::Key_Z, 0x042f}, {0x2d, Qt::Key_X, 0x0427},
        {0x2e, Qt::Key_C, 0x0421}, {0x2f, Qt::Key_V, 0x041c},
        {0x30, Qt::Key_B, 0x0418}, {0x31, Qt::Key_N, 0x0422},
        {0x32, Qt::Key_M, 0x042c}, {0x33, Qt::Key_Comma, 0x0411},
        {0x34, Qt::Key_Period, 0x042e}
    };
    for (const Key& key : keys) {
        for (const auto codes : {Codes::Windows, Codes::Xkb}) {
            const int scan = key.scan + (codes == Codes::Xkb ? 8 : 0);
            require(InputManager::keyboardKey(key.russian, scan, codes) == key.english,
                    "Russian physical key did not match English position");
            require(InputManager::keyboardKey(key.english, scan, codes) == key.english,
                    "English physical key changed position");
        }
        require(InputManager::keyboardKey(key.russian, 0) == key.english,
                "legacy Russian key did not migrate to its physical position");
    }
    // Scan identity wins over text, even on layouts other than Russian/English.
    require(InputManager::keyboardKey(Qt::Key_A, 0x2d, Codes::Windows) == Qt::Key_X,
            "keyboard normalization used text instead of physical scan code");
    require(InputManager::keyboardKey(Qt::Key_A, 0x2d + 8, Codes::Xkb) == Qt::Key_X,
            "XKB normalization used text instead of physical scan code");
    require(InputManager::keyboardKey(Qt::Key_Home, 0xe010, Codes::Windows) == Qt::Key_Home,
            "Windows extended scan code was mistaken for a letter");
    require(InputManager::keyboardKey(Qt::Key_Return, 0x1c, Codes::Windows) == Qt::Key_Return &&
                InputManager::keyboardKey(Qt::Key_Enter, 0xe01c, Codes::Windows) == Qt::Key_Enter,
            "main Enter and keypad Enter lost their existing key identities");
    require(InputManager::keyboardKey(Qt::Key_7, 0x47, Codes::Windows) == Qt::Key_7 &&
                InputManager::keyboardKey(Qt::Key_7, 0x47 + 8, Codes::Xkb) == Qt::Key_7,
            "numeric keypad digit stopped selecting a slot");
    require(InputManager::keyboardKey(Qt::Key_Left, 0, Codes::Unavailable) == Qt::Key_Left,
            "key without native scan code did not retain Qt identity");
    require(InputManager::keyboardKey(0x0447, 0) == Qt::Key_X,
            "lowercase legacy Russian binding did not migrate");
    for (int digit = 0; digit < 10; ++digit) {
        const int scan = digit == 0 ? 0x0b : digit + 1;
        require(InputManager::keyboardKey(Qt::Key_unknown, scan, Codes::Windows) == Qt::Key_0 + digit &&
                    InputManager::keyboardKey(Qt::Key_unknown, scan + 8, Codes::Xkb) == Qt::Key_0 + digit,
                "digit scan code did not map to its save slot");
        require(InputManager::reservedKeyboardKey(Qt::Key_0 + digit),
                "save slot digit was available for gameplay binding");
    }

    QTemporaryDir temp;
    require(temp.isValid(), "cannot create physical keyboard settings directory");
    InputManager manager(temp.path() + "/settings.ini");
    std::array<unsigned, 2> masks{};
    QObject::connect(&manager, &InputManager::maskChanged, &manager,
                     [&](unsigned player, unsigned value) { masks.at(player) = value; });
    auto mapping = manager.mapping(ConsoleProfile::MegaDrive, 0);
    mapping.deviceKey = "none";
    manager.setMapping(ConsoleProfile::MegaDrive, 0, mapping);
    require(manager.keyEvent(InputManager::keyboardKey(0x0427, 0x2d, Codes::Windows), true) &&
                (masks[0] & 1u), "Russian key did not activate default gameplay binding");
    require(manager.keyEvent(InputManager::keyboardKey(Qt::Key_X, 0x2d, Codes::Windows), false) &&
                masks[0] == 0, "layout switch before release left gameplay input held");
    manager.keyEvent(InputManager::keyboardKey(Qt::Key_X, 0x2d + 8, Codes::Xkb), true);
    require(masks[0] & 1u, "English XKB key did not activate gameplay binding");
    manager.keyEvent(InputManager::keyboardKey(0x0427, 0x2d + 8, Codes::Xkb), false);
    require(masks[0] == 0, "English-to-Russian layout switch left key held");
    manager.keyEvent(Qt::Key_X, true);
    manager.setEnabled(false);
    require(masks[0] == 0, "focus loss left physical keyboard input held");
    manager.setEnabled(true);
    require(masks[0] == 0, "focus regain resurrected released keyboard input");
}

void testKeyboardSettingsMigration() {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create keyboard migration directory");
    const QString path = temp.path() + "/settings.ini";
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue("input/md/player1/device", "none");
        settings.setValue("input/md/player1/key/0", 0x0427); // Russian Ч -> X
        settings.setValue("input/md/player1/key/8", Qt::Key_3);
        settings.setValue("input/md/player2/device", "none");
        settings.setValue("input/md/player2/key/0", 0x0419); // Russian Й -> Q
        settings.setValue("keys/button_8", Qt::Key_8); // pre-profile slot binding
        settings.setValue("keys/button_0", 0x0424); // Russian Ф -> A
        settings.sync();
    }
    InputManager manager(path);
    require(manager.mapping(ConsoleProfile::MegaDrive, 0).keyboard.value(0) == Qt::Key_X,
            "profile Cyrillic binding did not migrate");
    require(manager.mapping(ConsoleProfile::MegaDrive, 1).keyboard.value(0) == Qt::Key_Q,
            "player two Cyrillic binding did not migrate");
    for (const auto profile : {ConsoleProfile::MegaDrive, ConsoleProfile::MasterSystem,
                              ConsoleProfile::GameGear, ConsoleProfile::SG1000}) {
        require(manager.mapping(profile, 0).keyboard.value(8) == 0,
                "saved slot digit remained bound to gameplay");
    }
    require(manager.mapping(ConsoleProfile::MasterSystem, 0).keyboard.value(0) == Qt::Key_A,
            "pre-profile Cyrillic binding did not migrate");
    auto mapping = manager.mapping(ConsoleProfile::MegaDrive, 0);
    mapping.keyboard[0] = 0x041b; // Russian Л -> K
    mapping.keyboard[8] = Qt::Key_9;
    manager.setMapping(ConsoleProfile::MegaDrive, 0, mapping);
    require(manager.mapping(ConsoleProfile::MegaDrive, 0).keyboard.value(0) == Qt::Key_K &&
                manager.mapping(ConsoleProfile::MegaDrive, 0).keyboard.value(8) == 0,
            "mapping update did not canonicalize keys and reject slot digit");
    require(!manager.keyEvent(Qt::Key_9, true), "reserved digit was treated as gameplay input");
    InputManager reloaded(path);
    require(reloaded.mapping(ConsoleProfile::MegaDrive, 0).keyboard.value(0) == Qt::Key_K &&
                reloaded.mapping(ConsoleProfile::MegaDrive, 0).keyboard.value(8) == 0,
            "normalized keyboard binding did not persist");
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

void testSnapshotPreservesHeldKeys() {
    QTemporaryDir temp;
    require(temp.isValid(), "snapshot input settings directory");
    InputManager manager(temp.filePath("settings.ini"));
    auto second = manager.mapping(ConsoleProfile::MegaDrive, 1);
    second.connected = true;
    second.keyboard[0] = Qt::Key_Q;
    manager.setMapping(ConsoleProfile::MegaDrive, 1, second);
    unsigned mask = 0;
    QObject::connect(&manager, &InputManager::maskChanged, &manager,
                     [&](unsigned player, unsigned value) { if (player == 1) mask = value; });
    QJsonArray hardware;
    for (int player = 0; player < 3; ++player) {
        const auto mapping = manager.mapping(ConsoleProfile::MegaDrive, player);
        hardware.append(QJsonObject{{"connected", mapping.connected},
                                    {"sixButton", mapping.sixButton},
                                    {"kind", int(mapping.controllerType)}});
    }
    const QByteArray snapshot = QJsonDocument(hardware).toJson(QJsonDocument::Compact);
    manager.setMovieControllers(snapshot);
    require(manager.keyEvent(Qt::Key_Q, true) && (mask & 1u),
            "held second-player button not applied");
    for (int i = 0; i < 5; ++i) {
        manager.setMovieControllers(snapshot);
        manager.poll();
        require(mask & 1u, "unchanged network snapshot released a held key");
    }
    auto changed = hardware[1].toObject();
    changed.insert("connected", false);
    hardware[1] = changed;
    manager.setMovieControllers(QJsonDocument(hardware).toJson(QJsonDocument::Compact));
    require(mask == 0, "actual controller change did not release the key");
}

void testControllerAdapters() {
    QTemporaryDir temp;
    require(temp.isValid(), "adapter settings directory");
    const QString path = temp.filePath("settings.ini");
    InputManager manager(path);
    require(manager.adapter() == ControllerAdapter::None &&
            manager.isConnected(0) && !manager.isConnected(1) &&
            !manager.isConnected(2) && !manager.isConnected(7),
            "ordinary ports did not retain P1-only defaults");

    auto port2 = manager.mapping(ConsoleProfile::MegaDrive, 1);
    port2.connected = true;
    port2.controllerType = ControllerType::Justifier;
    manager.setMapping(ConsoleProfile::MegaDrive, 1, port2);
    auto chained = manager.mapping(ConsoleProfile::MegaDrive, 2);
    chained.connected = true;
    chained.aimSource = AimSource::LeftStick;
    manager.setMapping(ConsoleProfile::MegaDrive, 2, chained);
    require(manager.isConnected(2), "legacy chained Justifier not connected");

    QVector<QString> events;
    QObject::connect(&manager, &InputManager::adapterChanged, &manager,
                     [&](int) { events.append("adapter"); });
    QObject::connect(&manager, &InputManager::connectionChanged, &manager,
                     [&](unsigned, bool) { events.append("connection"); });
    manager.setAdapter(ConsoleProfile::MegaDrive, ControllerAdapter::TeamPlayerBoth);
    require(!events.isEmpty() && events.first() == "adapter",
            "adapter signal did not precede port changes");
    require(manager.adapter() == ControllerAdapter::TeamPlayerBoth,
            "Mega Drive adapter not selected");
    for (unsigned player = 0; player < MaxControllerPlayers; ++player) {
        require(manager.isConnected(player) && manager.controllerType(player) == ControllerType::Pad,
                "multitap player was not ready as a pad");
        if (player >= 2)
            require(manager.mapping(ConsoleProfile::MegaDrive, int(player)).keyboard.isEmpty(),
                    "extra player default keyboard conflicts with legacy players");
    }
    auto eighth = manager.mapping(ConsoleProfile::MegaDrive, 7);
    eighth.deviceKey = "none";
    eighth.keyboard[0] = Qt::Key_T;
    manager.setMapping(ConsoleProfile::MegaDrive, 7, eighth);
    unsigned eighthMask = 0;
    QObject::connect(&manager, &InputManager::maskChanged, &manager,
                     [&](unsigned player, unsigned mask) {
                         if (player == 7) eighthMask = mask;
                     });
    require(manager.keyEvent(Qt::Key_T, true) && (eighthMask & 1u),
            "eighth-player keyboard binding did not reach its mask");
    manager.releaseKeyboard();

    {
        InputManager reopened(path);
        require(reopened.adapter() == ControllerAdapter::TeamPlayerBoth &&
                reopened.isConnected(7) &&
                reopened.mapping(ConsoleProfile::MegaDrive, 7).keyboard.value(0) == Qt::Key_T,
                "adapter and eighth-player settings did not persist");
    }
    manager.setAdapter(ConsoleProfile::MegaDrive, ControllerAdapter::None);
    require(manager.isConnected(2) && manager.controllerType(2) == ControllerType::Justifier &&
            manager.mapping(ConsoleProfile::MegaDrive, 2).aimSource == AimSource::LeftStick,
            "leaving multitap lost chained Justifier settings");
    manager.setAdapter(ConsoleProfile::MegaDrive, ControllerAdapter::TeamPlayerPort1);
    require(manager.isConnected(4) && !manager.isConnected(5),
            "single Team Player did not expose five logical players");
    manager.setAdapter(ConsoleProfile::MegaDrive, ControllerAdapter::FourWayPlay);
    require(manager.isConnected(3) && !manager.isConnected(4),
            "4 Way Play did not expose four logical players");
    manager.setAdapter(ConsoleProfile::MasterSystem, ControllerAdapter::MasterTapBoth);
    manager.setProfile(ConsoleProfile::MasterSystem);
    require(manager.adapter() == ControllerAdapter::MasterTapBoth &&
            manager.isConnected(7) && manager.controllerType(2) == ControllerType::Pad,
            "Master Tap did not expose eight pads");
    manager.setAdapter(ConsoleProfile::GameGear, ControllerAdapter::TeamPlayerBoth);
    require(manager.adapter(ConsoleProfile::GameGear) == ControllerAdapter::None,
            "unsupported Game Gear adapter was accepted");

    manager.setProfile(ConsoleProfile::MegaDrive);
    manager.setAdapter(ConsoleProfile::MegaDrive, ControllerAdapter::None);
    QJsonArray controllers;
    for (int player = 0; player < int(MaxControllerPlayers); ++player)
        controllers.append(QJsonObject{{"connected", true}, {"sixButton", true},
                                       {"kind", int(ControllerType::Pad)}});
    const QByteArray snapshot = QJsonDocument(QJsonObject{
        {"adapter", int(ControllerAdapter::TeamPlayerBoth)},
        {"controllers", controllers}}).toJson(QJsonDocument::Compact);
    manager.setMovieControllers(snapshot);
    require(manager.adapter() == ControllerAdapter::TeamPlayerBoth &&
            manager.isConnected(7), "recorded adapter override not applied");
    manager.setMovieControllers(snapshot);
    require(manager.adapter() == ControllerAdapter::TeamPlayerBoth,
            "repeated recorded adapter override changed state");
    manager.setMovieControllers({});
    require(manager.adapter() == ControllerAdapter::None &&
            manager.isConnected(2) && !manager.isConnected(7),
            "ending recorded override did not restore ordinary ports");

    QJsonArray legacy;
    for (int player = 0; player < 3; ++player)
        legacy.append(QJsonObject{{"connected", player == 0}, {"sixButton", true},
                                  {"kind", int(player == 2 ? ControllerType::Justifier
                                                            : ControllerType::Pad)}});
    manager.setMovieControllers(QJsonDocument(legacy).toJson(QJsonDocument::Compact));
    require(manager.adapter() == ControllerAdapter::None &&
            !manager.isConnected(7), "legacy three-port recording was not accepted");
    manager.setMovieControllers({});
}

void testNetworkInputSource() {
    QTemporaryDir temp;
    require(temp.isValid(), "network source settings directory");
    const QString path = temp.filePath("settings.ini");
    InputManager manager(path);
    QJsonArray controllers;
    for (int player = 0; player < int(MaxControllerPlayers); ++player)
        controllers.append(QJsonObject{{"connected", player != 0}, {"sixButton", true},
                                       {"kind", int(ControllerType::Pad)}});
    const QByteArray hardware = QJsonDocument(QJsonObject{
        {"adapter", int(ControllerAdapter::TeamPlayerBoth)},
        {"controllers", controllers}}).toJson(QJsonDocument::Compact);
    manager.setMovieControllers(hardware);
    require(!manager.isConnected(0) && manager.isConnected(3),
            "network fixture did not disconnect emulated P1");
    unsigned localMask = 0;
    QObject::connect(&manager, &InputManager::maskChanged, &manager,
                     [&](unsigned player, unsigned mask) {
                         if (player == 0) localMask = mask;
                     });
    manager.setNetworkInputSourceEnabled(true);
    require(!manager.isConnected(0), "network source changed emulated port connection");
    require(manager.keyEvent(Qt::Key_X, true) && (localMask & 1u),
            "disconnected P1 could not provide local online input");
    manager.setNetworkInputSourceEnabled(false);
    require(localMask == 0 && !manager.isConnected(0),
            "disabling network source left local input held");
    manager.releaseKeyboard();
    manager.setMovieControllers({});

    manager.setProfile(ConsoleProfile::MasterSystem);
    QJsonArray smsControllers;
    for (int player = 0; player < int(MaxControllerPlayers); ++player)
        smsControllers.append(QJsonObject{
            {"connected", player < 2}, {"sixButton", false},
            {"kind", int(player == 0 ? ControllerType::LightPhaser : ControllerType::Pad)}});
    manager.setMovieControllers(QJsonDocument(QJsonObject{
        {"adapter", int(ControllerAdapter::None)},
        {"controllers", smsControllers}}).toJson(QJsonDocument::Compact));
    require(manager.controllerType(0) == ControllerType::LightPhaser &&
            (manager.mouseButtonMask(0, unsigned(Qt::LeftButton)) & (1u << 1)),
            "physical P1 Light Phaser fixture was not active");
    manager.setNetworkInputSourceEnabled(true);
    require(manager.controllerType(0) == ControllerType::LightPhaser &&
            manager.mouseButtonMask(0, unsigned(Qt::LeftButton)) == 0,
            "online pad source changed physical gun hardware or accepted gun mouse input");
    require(manager.keyEvent(Qt::Key_Up, true) && (localMask & (1u << 4)),
            "online pad source filtered P1 directional input through Light Phaser targets");
    manager.setNetworkInputSourceEnabled(false);
    require(localMask == 0 && manager.controllerType(0) == ControllerType::LightPhaser,
            "ending online pad role did not restore Light Phaser input targets");
    manager.releaseKeyboard();
    manager.setMovieControllers({});
    InputManager reopened(path);
    require(reopened.isConnected(0) && reopened.adapter() == ControllerAdapter::None &&
            reopened.adapter(ConsoleProfile::MasterSystem) == ControllerAdapter::None,
            "transient network input source was persisted");
}

void testAnalogControllers() {
    QTemporaryDir temp;
    require(temp.isValid(), "analog settings directory");
    const QString path = temp.filePath("settings.ini");
    InputManager manager(path);
    auto mouse = manager.mapping(ConsoleProfile::MegaDrive, 0);
    mouse.controllerType = ControllerType::Mouse;
    mouse.deviceKey = "none";
    manager.setMapping(ConsoleProfile::MegaDrive, 0, mouse);
    require(manager.controllerType(0) == ControllerType::Mouse &&
            InputManager::gameTargets(ConsoleProfile::MegaDrive, true, ControllerType::Mouse)
                == QVector<int>({1, 0, 8, 3}),
            "MD Mouse on physical port A was not available");
    require(manager.mouseButtonMask(0, unsigned(Qt::LeftButton)) == (1u << 1) &&
            manager.mouseButtonMask(0, unsigned(Qt::RightButton)) == 1u &&
            manager.mouseButtonMask(0, unsigned(Qt::MiddleButton)) == (1u << 8),
            "MD Mouse button defaults are incorrect");
    require(!manager.keyEvent(Qt::Key_Up, true), "MD Mouse accepted pad directions");
    auto invalid = manager.mapping(ConsoleProfile::MegaDrive, 2);
    invalid.controllerType = ControllerType::Mouse;
    manager.setMapping(ConsoleProfile::MegaDrive, 2, invalid);
    require(manager.mapping(ConsoleProfile::MegaDrive, 2).controllerType == ControllerType::Justifier,
            "MD Mouse was accepted behind the physical ports");

    VirtualPad mouseStick(SDL_JOYSTICK_TYPE_UNKNOWN);
    manager.poll();
    mouse = manager.mapping(ConsoleProfile::MegaDrive, 0);
    mouse.deviceKey = keyForId(manager.devices(), mouseStick.id);
    mouse.aimSource = AimSource::LeftStick;
    mouse.analogSensitivity = 400;
    manager.setMapping(ConsoleProfile::MegaDrive, 0, mouse);
    double mouseDx = 0, mouseDy = 0;
    QObject::connect(&manager, &InputManager::mouseMotion, &manager,
                     [&](unsigned player, double dx, double dy) {
                         if (player == 0) { mouseDx += dx; mouseDy += dy; }
                     });
    mouseStick.axis(0, 32767);
    manager.poll();
    QThread::msleep(20);
    manager.poll();
    require(mouseDx > 0 && std::abs(mouseDy) < 0.01,
            "MD Mouse stick did not emit relative motion");
    {
        InputManager reopened(path);
        require(reopened.mapping(ConsoleProfile::MegaDrive, 0).controllerType == ControllerType::Mouse &&
                reopened.mapping(ConsoleProfile::MegaDrive, 0).analogSensitivity == 400,
                "MD Mouse type or sensitivity did not persist");
    }

    VirtualPad paddleStick(SDL_JOYSTICK_TYPE_UNKNOWN);
    manager.poll();
    auto paddle = manager.mapping(ConsoleProfile::MasterSystem, 0);
    paddle.controllerType = ControllerType::Paddle;
    paddle.aimSource = AimSource::LeftStick;
    paddle.deviceKey = keyForId(manager.devices(), paddleStick.id);
    manager.setMapping(ConsoleProfile::MasterSystem, 0, paddle);
    manager.setProfile(ConsoleProfile::MasterSystem);
    require(manager.controllerType(0) == ControllerType::Paddle &&
            InputManager::gameTargets(ConsoleProfile::MasterSystem, true, ControllerType::Paddle)
                == QVector<int>({0}),
            "SMS Paddle target or physical type is incorrect");
    int paddlePosition = 999999, paddleChanges = 0;
    QObject::connect(&manager, &InputManager::paddleMoved, &manager,
                     [&](unsigned player, int position) {
                         if (player == 0) { paddlePosition = position; ++paddleChanges; }
                     });
    manager.poll();
    require(paddlePosition == 0, "centered Paddle did not emit its initial position");
    paddleStick.axis(0, 32767);
    manager.poll();
    require(paddlePosition > 30000, "Paddle stick did not reach its right edge");
    const int unchanged = paddleChanges;
    manager.poll();
    require(paddleChanges == unchanged, "unchanged Paddle position emitted repeatedly");
    paddleStick.axis(0, -32768);
    manager.poll();
    require(paddlePosition < -30000, "Paddle stick did not reach its left edge");
    manager.setEnabled(false);
    paddleStick.axis(0, 0);
    manager.poll();
    require(paddlePosition < -30000, "disabled Paddle continued to update");
    manager.setEnabled(true);
    manager.poll();
    require(paddlePosition == 0, "Paddle did not resume after input was reenabled");
    paddle.aimSource = AimSource::Mouse;
    paddle.deviceKey = "none";
    manager.setMapping(ConsoleProfile::MasterSystem, 0, paddle);
    require(manager.mouseButtonMask(0, unsigned(Qt::LeftButton)) == 1u,
            "Paddle mouse action did not map to joypad B");
    auto invalidPaddle = manager.mapping(ConsoleProfile::MasterSystem, 2);
    invalidPaddle.controllerType = ControllerType::Paddle;
    manager.setMapping(ConsoleProfile::MasterSystem, 2, invalidPaddle);
    require(manager.mapping(ConsoleProfile::MasterSystem, 2).controllerType == ControllerType::Pad,
            "Paddle was accepted behind physical ports");
}

void testJCartTopology() {
    QTemporaryDir temp;
    require(temp.isValid(), "J-Cart settings directory");
    const QString path = temp.filePath("settings.ini");
    InputManager manager(path);
    require(!manager.jcart() && manager.jcartPlayerBase() == 0, "J-Cart enabled by default");
    manager.setJCart(true);
    require(manager.jcart() && manager.jcartPlayerBase() == 2 &&
            manager.isConnected(2) && manager.isConnected(3) &&
            manager.controllerType(2) == ControllerType::Pad &&
            !manager.isConnected(4), "ordinary J-Cart pads did not append at P3/P4");
    manager.setAdapter(ConsoleProfile::MegaDrive, ControllerAdapter::TeamPlayerPort1);
    require(manager.jcartPlayerBase() == 5 && manager.isConnected(5) &&
            manager.isConnected(6) && !manager.isConnected(7),
            "single Team Player plus J-Cart did not expose seven roles");
    manager.setAdapter(ConsoleProfile::MegaDrive, ControllerAdapter::FourWayPlay);
    require(manager.jcartPlayerBase() == 4 && manager.isConnected(4) &&
            manager.isConnected(5) && !manager.isConnected(6),
            "4 Way Play plus J-Cart did not expose six roles");
    manager.setAdapter(ConsoleProfile::MegaDrive, ControllerAdapter::TeamPlayerBoth);
    require(manager.adapter() == ControllerAdapter::None &&
            manager.jcartPlayerBase() == 2 && manager.isConnected(3) &&
            !manager.isConnected(7),
            "dual Team Player was not suppressed while J-Cart is active");
    manager.setJCart(false);
    require(!manager.jcart() && manager.adapter() == ControllerAdapter::TeamPlayerBoth &&
            manager.isConnected(7), "stored dual adapter was not restored after J-Cart");

    manager.setAdapter(ConsoleProfile::MegaDrive, ControllerAdapter::None);
    auto parent = manager.mapping(ConsoleProfile::MegaDrive, 1);
    parent.connected = true;
    parent.controllerType = ControllerType::Justifier;
    manager.setMapping(ConsoleProfile::MegaDrive, 1, parent);
    auto chain = manager.mapping(ConsoleProfile::MegaDrive, 2);
    chain.connected = true;
    manager.setMapping(ConsoleProfile::MegaDrive, 2, chain);
    manager.setJCart(true);
    require(manager.jcartPlayerBase() == 3 && manager.isConnected(2) &&
            manager.controllerType(2) == ControllerType::Justifier &&
            manager.isConnected(3) && manager.isConnected(4),
            "J-Cart did not append after chained Justifier");

    QJsonArray recorded;
    for (int player = 0; player < int(MaxControllerPlayers); ++player)
        recorded.append(QJsonObject{{"connected", player < 5}, {"sixButton", true},
                                    {"kind", int(player == 1 || player == 2
                                        ? ControllerType::Justifier : ControllerType::Pad)}});
    const QByteArray snapshot = QJsonDocument(QJsonObject{
        {"adapter", 0}, {"jcart", true}, {"jcartBase", 3},
        {"controllers", recorded}}).toJson(QJsonDocument::Compact);
    manager.setJCart(false);
    manager.setMovieControllers(snapshot);
    require(manager.jcart() && manager.jcartPlayerBase() == 3 &&
            manager.controllerType(3) == ControllerType::Pad && manager.isConnected(4),
            "recorded J-Cart topology was not restored");
    manager.setMovieControllers({});
    require(!manager.jcart() && manager.jcartPlayerBase() == 0,
            "ending recorded J-Cart override did not restore local topology");
    InputManager reopened(path);
    require(!reopened.jcart() && reopened.jcartPlayerBase() == 0,
            "transient J-Cart hardware was persisted");
}

void testBackgroundGamepad() {
    QTemporaryDir temp;
    require(temp.isValid(), "cannot create background gamepad settings directory");
    const QString path = temp.filePath("settings.ini");
    InputManager manager(path);
    require(!manager.backgroundGamepadEnabled(), "background gamepad was not off by default");
    VirtualPad pad(SDL_JOYSTICK_TYPE_UNKNOWN);
    manager.poll();
    auto mapping = manager.mapping(ConsoleProfile::MegaDrive, 0);
    mapping.deviceKey = keyForId(manager.devices(), pad.id);
    mapping.pad[0] = {PadBinding::Button, 0, 0};
    mapping.pad[PauseCommand] = {PadBinding::Button, 1, 0};
    manager.setMapping(ConsoleProfile::MegaDrive, 0, mapping);
    unsigned mask = 0;
    int commandPresses = 0;
    QObject::connect(&manager, &InputManager::maskChanged, &manager,
                     [&](unsigned player, unsigned value) { if (player == 0) mask = value; });
    QObject::connect(&manager, &InputManager::commandChanged, &manager,
                     [&](int command, bool pressed) {
                         if (command == PauseCommand && pressed) ++commandPresses;
                     });
    manager.setEnabled(false);
    pad.button(0, true);
    pad.button(1, true);
    manager.poll();
    manager.setBackgroundGamepadActive(true);
    require(mask == 0, "background pad activated without its preference");
    manager.setBackgroundGamepadEnabled(true);
    manager.setBackgroundGamepadActive(true);
    manager.poll();
    require(mask & 1u, "background pad did not drive gameplay");
    require(commandPresses == 0, "background pad triggered an emulator command");
    const unsigned padOnlyMask = mask;
    require(manager.keyEvent(Qt::Key_X, true) && mask == padOnlyMask,
            "background keyboard changed the game mask");
    manager.setBackgroundGamepadActive(false);
    require(mask == 0, "modal gate did not release background pad");
    manager.setBackgroundGamepadActive(true);
    require(mask & 1u, "background pad did not resume after modal gate");
    manager.setBackgroundGamepadEnabled(false);
    require(mask == 0, "turning off background pad left a game button held");
    InputManager reopened(path);
    require(!reopened.backgroundGamepadEnabled(), "background pad preference did not persist");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--capture")) {
            testInput(); testSequentialAxisCapture(); testIndependentCapture();
            std::puts("capture held inputs, independent buttons, triggers and sequential axes passed");
            return 0;
        }
        if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--keyboard")) {
            testPhysicalKeyboard();
            testKeyboardSettingsMigration();
            std::puts("physical keyboard and reserved hotkey tests passed");
            return 0;
        }
        if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--analog")) {
            testAnalogControllers();
            std::puts("analog input tests passed");
            return 0;
        }
        if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--jcart")) {
            testJCartTopology();
            std::puts("J-Cart topology tests passed");
            return 0;
        }
        if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--multitap")) {
            testControllerAdapters();
            testNetworkInputSource();
            std::puts("multitap input tests passed");
            return 0;
        }
        if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--background")) {
            testBackgroundGamepad();
            std::puts("background gamepad tests passed");
            return 0;
        }
        if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--snapshot-hold")) {
            testSnapshotPreservesHeldKeys();
            std::puts("snapshot held-key test passed");
            return 0;
        }
        testConnectedPorts();
        if (argc==2 && QString::fromLocal8Bit(argv[1])==QLatin1String("--connections")) {
            std::puts("input port defaults, migration, persistence and keyboard separation passed");return 0;
        }
        testInput();
        testSequentialAxisCapture();
        testIndependentCapture();
        testRewindCommand();
        testHardResetCommand();
        testReservedRewindKey();
        testLegacyKeyboardSettings();
        testUnknownRawJoystick();
        testBackgroundGamepad();
        testPhysicalKeyboard();
        testKeyboardSettingsMigration();
        std::puts("input tests passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "input test failed: %s\n", e.what());
        return 1;
    }
}
