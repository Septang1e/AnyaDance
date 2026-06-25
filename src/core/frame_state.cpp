#include "core/frame_state.h"

namespace anyadance {

FrameState MakeNeutralFrame() {
    FrameState frame{};
    frame.devices[DeviceSlot(DeviceIndex::Hmd)] = {true, true, {0.0f, 1.50f, 0.0f}, {}, false};
    frame.devices[DeviceSlot(DeviceIndex::LeftController)] = {true, true, {-0.45f, 1.15f, 0.0f}, {}, false};
    frame.devices[DeviceSlot(DeviceIndex::RightController)] = {true, true, {0.45f, 1.15f, 0.0f}, {}, false};
    frame.devices[DeviceSlot(DeviceIndex::Hip)] = {true, true, {0.0f, 0.85f, 0.0f}, {}, false};
    frame.devices[DeviceSlot(DeviceIndex::LeftFoot)] = {true, true, {-0.12f, -0.01f, 0.0f}, {}, false};
    frame.devices[DeviceSlot(DeviceIndex::RightFoot)] = {true, true, {0.12f, -0.01f, 0.0f}, {}, false};
    return frame;
}

void NeutralizeControllerInputs(FrameState& frame) {
    frame.controllers = {};
}

bool ClampFrameY(FrameState& frame) {
    bool anyClamped = false;
    for (DeviceState& device : frame.devices) {
        const float originalY = device.position.y;
        device.position.y = ClampDeviceY(device.position.y);
        device.y_clamped = device.position.y != originalY;
        anyClamped = anyClamped || device.y_clamped;
    }
    return anyClamped;
}

void ApplyFingerGrip(ControllerState& controller) {
    if (!controller.has_finger_bends) {
        return;
    }
    // Near-full counts as a fist, so a hand curled to ~0.95+ (or a loaded pose
    // that lands just shy of 1.0) still grabs.
    constexpr float kFistThreshold = 0.95f;
    const FingerBends& f = controller.finger_bends;
    const bool fist = f.thumb >= kFistThreshold && f.index >= kFistThreshold &&
                      f.middle >= kFistThreshold && f.ring >= kFistThreshold &&
                      f.pinky >= kFistThreshold;
    controller.grip_click = fist;
    controller.grip_value = fist ? 1.0f : 0.0f;
}

void ApplyDanceFingerBends(const std::array<ControllerState, 2>& danceControllers,
                           std::array<ControllerState, 2>& controllers,
                           std::array<FingerBends, 2>& fingerStore) {
    for (std::size_t i = 0; i < danceControllers.size(); ++i) {
        if (danceControllers[i].has_finger_bends) {
            controllers[i].has_finger_bends = true;
            controllers[i].finger_bends = danceControllers[i].finger_bends;
            ApplyFingerGrip(controllers[i]);
            fingerStore[i] = danceControllers[i].finger_bends;
        }
    }
}

} // namespace anyadance