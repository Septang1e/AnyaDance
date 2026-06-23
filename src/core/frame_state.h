#pragma once

#include "core/constants.h"
#include "core/math3d.h"
#include "core/pose_sample.h"

#include <array>

namespace anyadance {

struct ControllerState {
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
};

struct DeviceState {
    bool valid = true;
    bool connected = true;
    Vec3 position{};
    Quat rotation{};
    bool y_clamped = false;
};

struct FrameState {
    std::array<DeviceState, 6> devices{};
    std::array<ControllerState, 2> controllers{};
};

FrameState MakeNeutralFrame();
void NeutralizeControllerInputs(FrameState& frame);
bool ClampFrameY(FrameState& frame);
PoseSample ToPoseSample(const FrameState& frame, DeviceIndex index);

// Copy the dance's finger bends (where present) onto the live controllers and
// into the persistent finger store, leaving controllers the dance does not drive
// untouched. Keeping the store in sync makes it the single source of truth that
// Save Pose and paused streaming read, instead of a stale manual-scroll value.
void ApplyDanceFingerBends(const std::array<ControllerState, 2>& danceControllers,
                           std::array<ControllerState, 2>& controllers,
                           std::array<FingerBends, 2>& fingerStore);

} // namespace anyadance