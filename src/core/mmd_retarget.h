#pragma once

#include "core/frame_state.h"
#include "core/solved_motion.h"

#include <vector>

namespace anyadance {

// Tunables for the simplified MMD -> 6-device remapping. The rig itself is
// hardcoded (HMD, two controllers, hip, two feet), so this only carries the few
// knobs a user actually sets in the dance dialog.
struct MmdRetargetParams {
    float targetHeightM = 1.5f;   // floor-to-crown height the dance is scaled to (matches kResetHmdY)
    float handReachScale = 1.22f; // palm overshoot about the shoulder so arms extend
    float headMountUpM = 0.10f;   // head joint -> HMD mount offset, up
    float floorOffsetM = 0.0f;    // raise (+) or lower (-) the whole body
};

// A retargeted dance ready for playback. Each frame is a six-device FrameState in
// dance-local space: the body stands at the origin on the floor (y up), and
// controllers carry per-frame finger bends when available.
struct DanceMotion {
    std::vector<float> times;
    std::vector<FrameState> frames;
    float duration = 0.0f;
    float sourceHeightM = 0.0f;
    float scale = 1.0f;
    bool hasFingers = false;
    bool valid = false;
};

// Build a playable DanceMotion from a solved motion using the simplified remap.
DanceMotion BuildDanceMotion(const SolvedMotion& motion, const MmdRetargetParams& params);

// Sample the dance-local pose at time t (seconds). Loops or clamps to the end.
FrameState SampleDanceMotion(const DanceMotion& dance, float t, bool loop);

// Translate a dance-local frame into driver space so the body stands at the given
// world X/Z (metres). Orientation and height are unchanged.
void AnchorDanceFrame(FrameState& frame, float rootX, float rootZ);

} // namespace anyadance
