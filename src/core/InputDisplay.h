#pragma once

#include "../input/ControllerTopology.h"
#include <QtGlobal>
#include <array>

enum class InputDisplayDevice : quint8 {
    None, MegaDrive3, MegaDrive6, MasterSystem, GameGear, SG1000,
    LightPhaser, Menacer, Justifier, Mouse, Paddle
};

struct InputDisplaySlot {
    InputDisplayDevice device = InputDisplayDevice::None;
    quint16 buttons = 0;
    qint16 x = 0, y = 0;
    bool offscreen = false;

    bool operator==(const InputDisplaySlot& other) const {
        return device == other.device && buttons == other.buttons && x == other.x &&
               y == other.y && offscreen == other.offscreen;
    }
};

struct InputDisplayState {
    std::array<InputDisplaySlot,MaxControllerPlayers> players{};
    bool operator==(const InputDisplayState& other) const { return players == other.players; }
};
