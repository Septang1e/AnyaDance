#pragma once

#include <array>
#include <chrono>

namespace anyadance {

struct FingerBends {
    float thumb = 0.0f;
    float index = 0.0f;
    float middle = 0.0f;
    float ring = 0.0f;
    float pinky = 0.0f;
};

struct PoseSample {
    bool valid = false;
    bool connected = true;
    std::array<float, 3> position = {0.0f, 1.0f, 0.0f};
    std::array<float, 4> rotation_xyzw = {0.0f, 0.0f, 0.0f, 1.0f};
    bool trigger_click = false;
    float trigger_value = 0.0f;
    bool menu_click = false;
    bool system_click = false;
    bool a_click = false;
    bool b_click = false;
    bool grip_click = false;
    float grip_value = 0.0f;
    float joystick_x = 0.0f;
    float joystick_y = 0.0f;
    float trackpad_x = 0.0f;
    float trackpad_y = 0.0f;
    bool has_finger_bends = false;
    FingerBends finger_bends{};
    bool y_clamped = false;
    std::chrono::steady_clock::time_point received_at{};
};

} // namespace anyadance