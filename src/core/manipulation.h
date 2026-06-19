#pragma once

#include "core/frame_state.h"

namespace anyadance {

enum ManipulationModifiers {
    ManipulationModifier_None = 0,
    ManipulationModifier_Ctrl = 1 << 0,
    ManipulationModifier_Shift = 1 << 1,
};

// Frame the translation and rotation gestures act in: tied to the head heading
// (Hmd) or to fixed world axes (Global). Vertical (Y) translation is always world
// up regardless of the frame.
enum class ManipulationFrame {
    Hmd,
    Global,
};

struct DragSnapshot {
    FrameState startFrame{};
    DeviceIndex device = DeviceIndex::Hmd;
    float hmdYawBasis = 0.0f;
    float accumulatedDx = 0.0f;
    float accumulatedDy = 0.0f;
};

DragSnapshot BeginDrag(const FrameState& frame, DeviceIndex device);
void ApplyDragDelta(DragSnapshot& drag, FrameState& frame, float dxCounts, float dyCounts, int modifiers,
                    ManipulationFrame manipulationFrame = ManipulationFrame::Hmd);
bool MirroredDeviceFor(DeviceIndex device, DeviceIndex& mirroredDevice);
void ApplySymmetricMirror(const DragSnapshot& drag, FrameState& frame, DeviceIndex mirroredDevice,
                          ManipulationFrame manipulationFrame = ManipulationFrame::Hmd);

} // namespace anyadance
