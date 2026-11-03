#pragma once

// Shared by the input UI and the emulation host; no Qt dependency.
enum class ControllerType {
    Pad = 0, LightPhaser = 1, Menacer = 2, Justifier = 3, Mouse = 4, Paddle = 5
};
constexpr bool isLightgun(ControllerType type) {
    return type == ControllerType::LightPhaser || type == ControllerType::Menacer ||
           type == ControllerType::Justifier;
}
constexpr bool isAnalogController(ControllerType type) {
    return type == ControllerType::Mouse || type == ControllerType::Paddle;
}
enum class AimSource { Mouse = 0, LeftStick = 1 };
