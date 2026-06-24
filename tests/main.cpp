#include "core/constants.h"
#include "core/finger_control.h"
#include "core/frame_state.h"
#include "core/input_state.h"
#include "core/json.h"
#include "core/manipulation.h"
#include "core/mmd_retarget.h"
#include "core/nya_format.h"
#include "core/protocol.h"
#include "core/solved_motion.h"
#include "core/tpose.h"
#include "core/udp_log.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace anyadance;

namespace {

int g_failures = 0;

void Expect(bool condition, const char* expr, const char* file, int line) {
    if (!condition) {
        std::cerr << file << ':' << line << " failed: " << expr << '\n';
        ++g_failures;
    }
}

void ExpectNear(float lhs, float rhs, float eps, const char* expr, const char* file, int line) {
    if (std::fabs(lhs - rhs) > eps) {
        std::cerr << file << ':' << line << " failed: " << expr << " got " << lhs << " expected " << rhs << '\n';
        ++g_failures;
    }
}

#define EXPECT_TRUE(expr) Expect((expr), #expr, __FILE__, __LINE__)
#define EXPECT_FALSE(expr) Expect(!(expr), "!(" #expr ")", __FILE__, __LINE__)
#define EXPECT_NEAR(lhs, rhs, eps) ExpectNear((lhs), (rhs), (eps), #lhs " ~= " #rhs, __FILE__, __LINE__)

void ExpectVec3Near(Vec3 value, Vec3 expected, const char* expr, const char* file, int line) {
    ExpectNear(value.x, expected.x, 0.0001f, expr, file, line);
    ExpectNear(value.y, expected.y, 0.0001f, expr, file, line);
    ExpectNear(value.z, expected.z, 0.0001f, expr, file, line);
}

#define EXPECT_VEC3_NEAR(value, expected) ExpectVec3Near((value), (expected), #value " ~= " #expected, __FILE__, __LINE__)

void ExpectSameRotation(Quat lhs, Quat rhs, const char* expr, const char* file, int line) {
    lhs = Normalized(lhs);
    rhs = Normalized(rhs);
    const float dot = lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z + lhs.w * rhs.w;
    if (std::fabs(std::fabs(dot) - 1.0f) > 0.0001f) {
        std::cerr << file << ':' << line << " failed: " << expr << " quaternion dot " << dot << '\n';
        ++g_failures;
    }
}

Quat Canonical(Quat q) {
    q = Normalized(q);
    if (q.w < 0.0f) {
        return {-q.x, -q.y, -q.z, -q.w};
    }
    return q;
}

Quat ToHmdYawLocal(Quat rotation, float hmdYaw) {
    return Canonical(Multiply(Conjugate(FromYaw(hmdYaw)), rotation));
}

void ExpectMirroredInBasis(const FrameState& frame, float yawBasis, DeviceIndex activeDevice, DeviceIndex mirroredDevice, const char* file, int line) {
    const Quat activeLocal = ToHmdYawLocal(frame.devices[DeviceSlot(activeDevice)].rotation, yawBasis);
    const Quat mirroredLocal = ToHmdYawLocal(frame.devices[DeviceSlot(mirroredDevice)].rotation, yawBasis);
    const Quat expected = Canonical({activeLocal.x, -activeLocal.y, -activeLocal.z, activeLocal.w});
    ExpectSameRotation(mirroredLocal, expected, "mirrored hand rotation", file, line);
}

void ExpectMirroredInYawBasis(const FrameState& frame, const DragSnapshot& drag, DeviceIndex activeDevice, DeviceIndex mirroredDevice, const char* file, int line) {
    ExpectMirroredInBasis(frame, drag.hmdYawBasis, activeDevice, mirroredDevice, file, line);
}

#define EXPECT_SAME_ROTATION(lhs, rhs) ExpectSameRotation((lhs), (rhs), #lhs " ~= " #rhs, __FILE__, __LINE__)
#define EXPECT_MIRRORED_IN_YAW_BASIS(frame, drag, active, mirrored) ExpectMirroredInYawBasis((frame), (drag), (active), (mirrored), __FILE__, __LINE__)
#define EXPECT_MIRRORED_IN_BASIS(frame, yawBasis, active, mirrored) ExpectMirroredInBasis((frame), (yawBasis), (active), (mirrored), __FILE__, __LINE__)

std::string ValidPacket() {
    FrameState frame = BuildResetTPose(MakeNeutralFrame());
    frame.controllers[0].trigger_click = true;
    frame.controllers[0].trigger_value = 3.0f;
    frame.controllers[0].joystick_x = 2.0f;
    frame.controllers[0].joystick_y = -2.0f;
    return SerializeFrame(frame);
}

void TestProtocol() {
    ParsedFrame parsed;
    const std::string valid = ValidPacket();
    EXPECT_TRUE(ParsePoseFrame(valid, parsed));
    EXPECT_TRUE(parsed.present[DeviceSlot(DeviceIndex::Hmd)]);
    EXPECT_TRUE(parsed.present[DeviceSlot(DeviceIndex::LeftController)]);

    std::string wrongVersion = valid;
    wrongVersion.replace(wrongVersion.find("\"version\":1"), 11, "\"version\":2");
    EXPECT_FALSE(ParsePoseFrame(wrongVersion, parsed));
    EXPECT_FALSE(ParsePoseFrame("{not json", parsed));

    std::string oversized(static_cast<std::size_t>(kMaxPacketBytes), ' ');
    EXPECT_FALSE(ParsePoseFrameBytes(oversized.data(), static_cast<int>(oversized.size()), parsed));

    const std::string missingHmd = "{\"version\":1,\"devices\":{\"hmd\":{\"valid\":true},\"left_controller\":{\"valid\":true,\"connected\":true,\"pose\":{\"position\":[0,1,0],\"rotation_xyzw\":[0,0,0,1]}}}}";
    EXPECT_TRUE(ParsePoseFrame(missingHmd, parsed));
    EXPECT_FALSE(parsed.present[DeviceSlot(DeviceIndex::Hmd)]);
    EXPECT_TRUE(parsed.present[DeviceSlot(DeviceIndex::LeftController)]);

    const std::string unknownOnly = "{\"version\":1,\"devices\":{\"unknown\":{\"valid\":true,\"connected\":true,\"pose\":{\"position\":[0,1,0],\"rotation_xyzw\":[0,0,0,1]}}}}";
    EXPECT_FALSE(ParsePoseFrame(unknownOnly, parsed));

    const std::string nonFinite = "{\"version\":1,\"devices\":{\"hmd\":{\"valid\":true,\"connected\":true,\"pose\":{\"position\":[0,1e999,0],\"rotation_xyzw\":[0,0,0,1]}}}}";
    EXPECT_FALSE(ParsePoseFrame(nonFinite, parsed));

    const std::string normalized = "{\"version\":1,\"devices\":{\"hmd\":{\"valid\":true,\"connected\":true,\"pose\":{\"position\":[0,1,0],\"rotation_xyzw\":[0,0,0,0.8]}}}}";
    EXPECT_TRUE(ParsePoseFrame(normalized, parsed));
    EXPECT_NEAR(parsed.samples[DeviceSlot(DeviceIndex::Hmd)].rotation_xyzw[3], 1.0f, 0.0001f);

    const std::string invalidQuat = "{\"version\":1,\"devices\":{\"hmd\":{\"valid\":true,\"connected\":true,\"pose\":{\"position\":[0,1,0],\"rotation_xyzw\":[0,0,0,0.1]}}}}";
    EXPECT_FALSE(ParsePoseFrame(invalidQuat, parsed));

    const std::string clampedInput = "{\"version\":1,\"devices\":{\"left_controller\":{\"valid\":true,\"connected\":true,\"pose\":{\"position\":[0,1,0],\"rotation_xyzw\":[0,0,0,1]}}},\"inputs\":{\"left_controller\":{\"trigger_click\":true,\"trigger_value\":3,\"joystick_x\":2,\"joystick_y\":-2}}}";
    EXPECT_TRUE(ParsePoseFrame(clampedInput, parsed));
    const PoseSample& left = parsed.samples[DeviceSlot(DeviceIndex::LeftController)];
    EXPECT_NEAR(left.trigger_value, 1.0f, 0.0001f);
    EXPECT_NEAR(left.joystick_x, 1.0f, 0.0001f);
    EXPECT_NEAR(left.joystick_y, -1.0f, 0.0001f);
    EXPECT_NEAR(left.trackpad_x, left.joystick_x, 0.0001f);
    EXPECT_NEAR(left.trackpad_y, left.joystick_y, 0.0001f);

    FrameState serialized = BuildResetTPose(MakeNeutralFrame());
    serialized.devices[DeviceSlot(DeviceIndex::Hmd)].position.y = kMaxDeviceY + 1.0f;
    serialized.controllers[0].grip_click = true;
    serialized.controllers[0].grip_value = 2.0f;
    serialized.controllers[0].has_finger_bends = true;
    serialized.controllers[0].finger_bends = {-0.5f, 0.25f, 0.5f, 0.75f, 1.5f};
    EXPECT_TRUE(ParsePoseFrame(SerializeFrame(serialized), parsed));
    const PoseSample& serializedHmd = parsed.samples[DeviceSlot(DeviceIndex::Hmd)];
    EXPECT_NEAR(serializedHmd.position[1], kMaxDeviceY, 0.0001f);
    const PoseSample& serializedLeft = parsed.samples[DeviceSlot(DeviceIndex::LeftController)];
    EXPECT_TRUE(serializedLeft.grip_click);
    EXPECT_NEAR(serializedLeft.grip_value, 1.0f, 0.0001f);
    EXPECT_TRUE(serializedLeft.has_finger_bends);
    EXPECT_NEAR(serializedLeft.finger_bends.thumb, 0.0f, 0.0001f);
    EXPECT_NEAR(serializedLeft.finger_bends.index, 0.25f, 0.0001f);
    EXPECT_NEAR(serializedLeft.finger_bends.pinky, 1.0f, 0.0001f);

    const std::string overHeight = "{\"version\":1,\"devices\":{\"hmd\":{\"valid\":true,\"connected\":true,\"pose\":{\"position\":[0,3,0],\"rotation_xyzw\":[0,0,0,1]}}}}";
    EXPECT_TRUE(ParsePoseFrame(overHeight, parsed));
    EXPECT_NEAR(parsed.samples[DeviceSlot(DeviceIndex::Hmd)].position[1], kMaxDeviceY, 0.0001f);
    EXPECT_TRUE(parsed.y_clamped[DeviceSlot(DeviceIndex::Hmd)]);

    const std::string pretty = PrettyPrintJson("{\"text\":\"brace } and quote \\\" stays string\",\"items\":[1,2]}");
    EXPECT_TRUE(pretty.find("brace } and quote \\\" stays string") != std::string::npos);
    EXPECT_TRUE(pretty.find("\n  \"items\"") != std::string::npos);
}

void TestSafety() {
    FrameState frame = MakeNeutralFrame();
    frame.devices[DeviceSlot(DeviceIndex::Hmd)].position = {1.0f, 4.0f, -3.0f};
    ClampFrameY(frame);
    const DeviceState& hmd = frame.devices[DeviceSlot(DeviceIndex::Hmd)];
    EXPECT_NEAR(hmd.position.x, 1.0f, 0.0001f);
    EXPECT_NEAR(hmd.position.y, kMaxDeviceY, 0.0001f);
    EXPECT_NEAR(hmd.position.z, -3.0f, 0.0001f);

    for (DeviceState& device : frame.devices) {
        device.position.y = 6.0f;
    }
    ClampFrameY(frame);
    for (const DeviceState& device : frame.devices) {
        EXPECT_NEAR(device.position.y, 2.0f, 0.0001f);
    }
}

void TestTPose() {
    FrameState frame = MakeNeutralFrame();
    frame.devices[DeviceSlot(DeviceIndex::Hmd)].position = {2.0f, 1.8f, -1.0f};
    frame.devices[DeviceSlot(DeviceIndex::Hmd)].rotation = FromYaw(DegToRad(90.0f));
    FrameState reset = BuildResetTPose(frame);
    const DeviceState& hmd = reset.devices[DeviceSlot(DeviceIndex::Hmd)];
    EXPECT_NEAR(hmd.position.x, 2.0f, 0.0001f);
    EXPECT_NEAR(hmd.position.y, 1.50f, 0.0001f);
    EXPECT_NEAR(hmd.position.z, -1.0f, 0.0001f);
    // Reset zeroes the HMD yaw so the rig faces the canonical forward, regardless
    // of the input heading (the in-game yaw is driven by locomotion).
    EXPECT_SAME_ROTATION(hmd.rotation, FromYaw(0.0f));

    // Reset uprights the head and zeroes its yaw: a tilted/rolled/yawed HMD resets
    // to identity orientation, and the body follows.
    FrameState tilted = MakeNeutralFrame();
    const Quat tiltedRotation = Multiply(
        Multiply(FromYaw(DegToRad(40.0f)), FromAxisAngle({1.0f, 0.0f, 0.0f}, DegToRad(25.0f))),
        FromAxisAngle({0.0f, 0.0f, 1.0f}, DegToRad(30.0f)));
    tilted.devices[DeviceSlot(DeviceIndex::Hmd)].rotation = tiltedRotation;
    FrameState tiltedReset = BuildResetTPose(tilted);
    const Quat expectedYaw = FromYaw(0.0f);
    EXPECT_SAME_ROTATION(tiltedReset.devices[DeviceSlot(DeviceIndex::Hmd)].rotation, expectedYaw);
    EXPECT_SAME_ROTATION(tiltedReset.devices[DeviceSlot(DeviceIndex::Hip)].rotation, expectedYaw);

    const DeviceState& left = reset.devices[DeviceSlot(DeviceIndex::LeftController)];
    const DeviceState& right = reset.devices[DeviceSlot(DeviceIndex::RightController)];
    const DeviceState& hip = reset.devices[DeviceSlot(DeviceIndex::Hip)];
    const DeviceState& leftFoot = reset.devices[DeviceSlot(DeviceIndex::LeftFoot)];
    const DeviceState& rightFoot = reset.devices[DeviceSlot(DeviceIndex::RightFoot)];
    // With the heading zeroed, the rig is built at yaw 0, so offsets land in body
    // space directly (HMD at x=2.0, z=-1.0).
    EXPECT_VEC3_NEAR(left.position, (Vec3{1.38f, 1.33f, -1.10f}));
    EXPECT_VEC3_NEAR(right.position, (Vec3{2.62f, 1.33f, -1.10f}));
    EXPECT_VEC3_NEAR(hip.position, (Vec3{2.0f, 1.07f, -1.05f}));
    EXPECT_VEC3_NEAR(leftFoot.position, (Vec3{1.91f, 0.26f, -0.90f}));
    EXPECT_VEC3_NEAR(rightFoot.position, (Vec3{2.09f, 0.26f, -0.90f}));
    EXPECT_NEAR(LengthSquared(left.rotation), 1.0f, 0.0001f);
    EXPECT_NEAR(LengthSquared(right.rotation), 1.0f, 0.0001f);
    EXPECT_SAME_ROTATION(left.rotation, kLeftControllerCanonicalRotation);

    FrameState neutralReset = BuildResetTPose(MakeNeutralFrame());
    EXPECT_VEC3_NEAR(neutralReset.devices[DeviceSlot(DeviceIndex::LeftController)].position, (Vec3{-0.62f, 1.33f, -0.10f}));
    EXPECT_VEC3_NEAR(neutralReset.devices[DeviceSlot(DeviceIndex::RightController)].position, (Vec3{0.62f, 1.33f, -0.10f}));
    EXPECT_VEC3_NEAR(neutralReset.devices[DeviceSlot(DeviceIndex::Hip)].position, (Vec3{0.0f, 1.07f, -0.05f}));
    EXPECT_VEC3_NEAR(neutralReset.devices[DeviceSlot(DeviceIndex::LeftFoot)].position, (Vec3{-0.09f, 0.26f, 0.10f}));
    EXPECT_VEC3_NEAR(neutralReset.devices[DeviceSlot(DeviceIndex::RightFoot)].position, (Vec3{0.09f, 0.26f, 0.10f}));
    EXPECT_FALSE(reset.controllers[0].trigger_click);
    EXPECT_FALSE(reset.controllers[1].a_click);
}

void TestInput() {
    KeyboardInputState keys;
    FrameState frame = MakeNeutralFrame();
    keys.HandleKey(ToolKey::W, true, false);
    keys.HandleKey(ToolKey::D, true, false);
    keys.UpdateFrameInputs(frame);
    EXPECT_NEAR(std::sqrt(frame.controllers[0].joystick_x * frame.controllers[0].joystick_x + frame.controllers[0].joystick_y * frame.controllers[0].joystick_y), 1.0f, 0.0001f);
    keys.HandleKey(ToolKey::A, true, false);
    keys.UpdateFrameInputs(frame);
    EXPECT_NEAR(frame.controllers[0].joystick_x, 0.0f, 0.0001f);

    keys.HandleKey(ToolKey::Q, true, false);
    keys.UpdateFrameInputs(frame);
    EXPECT_NEAR(frame.controllers[1].joystick_x, -1.0f, 0.0001f);
    keys.HandleKey(ToolKey::E, true, false);
    keys.UpdateFrameInputs(frame);
    EXPECT_NEAR(frame.controllers[1].joystick_x, 0.0f, 0.0001f);

    // Jump/menu/voice follow their key directly: down while held, up on release.
    // OS auto-repeat (repeat == true) must neither drop nor re-fire the press.
    keys.HandleKey(ToolKey::Space, true, false);
    keys.HandleKey(ToolKey::Space, true, true);
    keys.UpdateFrameInputs(frame);
    EXPECT_TRUE(frame.controllers[1].a_click);
    keys.HandleKey(ToolKey::Space, false, false);
    keys.UpdateFrameInputs(frame);
    EXPECT_FALSE(frame.controllers[1].a_click);

    keys.HandleKey(ToolKey::M, true, false);
    keys.UpdateFrameInputs(frame);
    EXPECT_TRUE(frame.controllers[1].b_click);
    keys.HandleKey(ToolKey::V, true, false);
    keys.UpdateFrameInputs(frame);
    EXPECT_TRUE(frame.controllers[0].a_click);

    keys.HandleKey(ToolKey::Z, true, false);
    keys.HandleKey(ToolKey::X, true, false);
    keys.UpdateFrameInputs(frame);
    EXPECT_TRUE(frame.controllers[0].trigger_click);
    EXPECT_NEAR(frame.controllers[0].trigger_value, 1.0f, 0.0001f);
    EXPECT_TRUE(frame.controllers[1].trigger_click);
    keys.SetFocus(false);
    keys.UpdateFrameInputs(frame);
    EXPECT_FALSE(frame.controllers[0].trigger_click);
    EXPECT_FALSE(frame.controllers[1].trigger_click);
}

void TestManipulation() {
    FrameState frame = BuildResetTPose(MakeNeutralFrame());
    const Vec3 hmdStart = frame.devices[DeviceSlot(DeviceIndex::Hmd)].position;
    DragSnapshot hmdDrag = BeginDrag(frame, DeviceIndex::Hmd);
    ApplyDragDelta(hmdDrag, frame, 100.0f, -50.0f, ManipulationModifier_None);
    EXPECT_NEAR(frame.devices[DeviceSlot(DeviceIndex::Hmd)].position.x, hmdStart.x, 0.0001f);
    EXPECT_NEAR(frame.devices[DeviceSlot(DeviceIndex::Hmd)].position.y, hmdStart.y, 0.0001f);
    EXPECT_TRUE(frame.devices[DeviceSlot(DeviceIndex::Hmd)].rotation.y < 0.0f);

    hmdDrag = BeginDrag(frame, DeviceIndex::Hmd);
    const Quat beforeRoll = frame.devices[DeviceSlot(DeviceIndex::Hmd)].rotation;
    ApplyDragDelta(hmdDrag, frame, 100.0f, 0.0f, ManipulationModifier_Ctrl | ManipulationModifier_Shift);
    EXPECT_TRUE(std::fabs(frame.devices[DeviceSlot(DeviceIndex::Hmd)].rotation.z - beforeRoll.z) > 0.0f);

    // The head allows vertical (Y) translation with the shift gesture; X/Z stay
    // locked and the rotation is left untouched.
    frame = BuildResetTPose(MakeNeutralFrame());
    const Vec3 hmdYStart = frame.devices[DeviceSlot(DeviceIndex::Hmd)].position;
    const Quat hmdRotStart = frame.devices[DeviceSlot(DeviceIndex::Hmd)].rotation;
    hmdDrag = BeginDrag(frame, DeviceIndex::Hmd);
    ApplyDragDelta(hmdDrag, frame, 25.0f, -50.0f, ManipulationModifier_Shift);
    EXPECT_NEAR(frame.devices[DeviceSlot(DeviceIndex::Hmd)].position.x, hmdYStart.x, 0.0001f);
    EXPECT_NEAR(frame.devices[DeviceSlot(DeviceIndex::Hmd)].position.z, hmdYStart.z, 0.0001f);
    EXPECT_TRUE(frame.devices[DeviceSlot(DeviceIndex::Hmd)].position.y > hmdYStart.y);
    EXPECT_SAME_ROTATION(frame.devices[DeviceSlot(DeviceIndex::Hmd)].rotation, hmdRotStart);

    // Head Y is clamped to the shared 2 m ceiling, like every other device.
    hmdDrag = BeginDrag(frame, DeviceIndex::Hmd);
    ApplyDragDelta(hmdDrag, frame, 0.0f, -100000.0f, ManipulationModifier_Shift);
    EXPECT_NEAR(frame.devices[DeviceSlot(DeviceIndex::Hmd)].position.y, kMaxDeviceY, 0.0001f);
    EXPECT_TRUE(frame.devices[DeviceSlot(DeviceIndex::Hmd)].y_clamped);

    // Re-anchor at the ceiling: after overshooting far past 2 m, a small reverse
    // drag must descend immediately from the cap, not unwind the phantom overshoot.
    ApplyDragDelta(hmdDrag, frame, 0.0f, 20.0f, ManipulationModifier_Shift);
    EXPECT_NEAR(frame.devices[DeviceSlot(DeviceIndex::Hmd)].position.y,
                kMaxDeviceY - 20.0f * kTranslationMetersPerCount, 0.0001f);
    EXPECT_FALSE(frame.devices[DeviceSlot(DeviceIndex::Hmd)].y_clamped);

    frame = BuildResetTPose(MakeNeutralFrame());
    DragSnapshot leftDrag = BeginDrag(frame, DeviceIndex::LeftController);
    const Vec3 leftStart = frame.devices[DeviceSlot(DeviceIndex::LeftController)].position;
    ApplyDragDelta(leftDrag, frame, 10.0f, -10.0f, ManipulationModifier_None);
    EXPECT_TRUE(frame.devices[DeviceSlot(DeviceIndex::LeftController)].position.x > leftStart.x);
    EXPECT_TRUE(frame.devices[DeviceSlot(DeviceIndex::LeftController)].position.y > leftStart.y);

    leftDrag = BeginDrag(frame, DeviceIndex::LeftController);
    const float zStart = frame.devices[DeviceSlot(DeviceIndex::LeftController)].position.z;
    ApplyDragDelta(leftDrag, frame, 0.0f, -10.0f, ManipulationModifier_Shift);
    EXPECT_TRUE(frame.devices[DeviceSlot(DeviceIndex::LeftController)].position.z < zStart);

    leftDrag = BeginDrag(frame, DeviceIndex::LeftController);
    const Quat rotStart = frame.devices[DeviceSlot(DeviceIndex::LeftController)].rotation;
    ApplyDragDelta(leftDrag, frame, 10.0f, -10.0f, ManipulationModifier_Ctrl);
    EXPECT_TRUE(std::fabs(frame.devices[DeviceSlot(DeviceIndex::LeftController)].rotation.y - rotStart.y) > 0.0f);

    // Rotation respects the head yaw basis like translation: a pure pitch produces
    // the same head-local delta whether or not the body is yawed. (Driving the
    // delta about world axes would make this differ once the head is turned.)
    auto headLocalPitchDelta = [](float hmdYawDeg) {
        FrameState f = BuildResetTPose(MakeNeutralFrame());
        f.devices[DeviceSlot(DeviceIndex::Hmd)].rotation = FromYaw(DegToRad(hmdYawDeg));
        DragSnapshot d = BeginDrag(f, DeviceIndex::LeftController);
        const Quat start = f.devices[DeviceSlot(DeviceIndex::LeftController)].rotation;
        ApplyDragDelta(d, f, 0.0f, -40.0f, ManipulationModifier_Ctrl);
        const Quat basis = FromYaw(d.hmdYawBasis);
        const Quat worldDelta = Normalized(Multiply(
            f.devices[DeviceSlot(DeviceIndex::LeftController)].rotation, Conjugate(start)));
        return Canonical(Normalized(Multiply(Multiply(Conjugate(basis), worldDelta), basis)));
    };
    EXPECT_SAME_ROTATION(headLocalPitchDelta(0.0f), headLocalPitchDelta(90.0f));

    // The Global frame ignores HMD heading: the world-space rotation delta is
    // identical regardless of yaw (unlike the Hmd frame above).
    auto worldPitchDelta = [](float hmdYawDeg) {
        FrameState f = BuildResetTPose(MakeNeutralFrame());
        f.devices[DeviceSlot(DeviceIndex::Hmd)].rotation = FromYaw(DegToRad(hmdYawDeg));
        DragSnapshot d = BeginDrag(f, DeviceIndex::LeftController);
        const Quat start = f.devices[DeviceSlot(DeviceIndex::LeftController)].rotation;
        ApplyDragDelta(d, f, 0.0f, -40.0f, ManipulationModifier_Ctrl, ManipulationFrame::Global);
        return Canonical(Normalized(Multiply(
            f.devices[DeviceSlot(DeviceIndex::LeftController)].rotation, Conjugate(start))));
    };
    EXPECT_SAME_ROTATION(worldPitchDelta(0.0f), worldPitchDelta(90.0f));

    // The frame governs translation too: a horizontal drag in Global mode moves
    // along world X regardless of HMD yaw, whereas the Hmd frame follows the head.
    auto horizontalMove = [](float hmdYawDeg, ManipulationFrame mf) {
        FrameState f = BuildResetTPose(MakeNeutralFrame());
        f.devices[DeviceSlot(DeviceIndex::Hmd)].rotation = FromYaw(DegToRad(hmdYawDeg));
        DragSnapshot d = BeginDrag(f, DeviceIndex::LeftController);
        const Vec3 s = f.devices[DeviceSlot(DeviceIndex::LeftController)].position;
        ApplyDragDelta(d, f, 30.0f, 0.0f, ManipulationModifier_None, mf);
        const Vec3 e = f.devices[DeviceSlot(DeviceIndex::LeftController)].position;
        return Vec3{e.x - s.x, e.y - s.y, e.z - s.z};
    };
    const Vec3 globalMove0 = horizontalMove(0.0f, ManipulationFrame::Global);
    const Vec3 globalMove90 = horizontalMove(90.0f, ManipulationFrame::Global);
    EXPECT_NEAR(globalMove90.x, globalMove0.x, 0.0001f);
    EXPECT_NEAR(globalMove90.z, globalMove0.z, 0.0001f);
    EXPECT_TRUE(std::fabs(globalMove0.x) > 0.0f);
    // The Hmd frame instead rotates the drag axis with the head, so a 90 deg yaw
    // turns the same X drag into world-Z motion.
    const Vec3 hmdMove90 = horizontalMove(90.0f, ManipulationFrame::Hmd);
    EXPECT_TRUE(std::fabs(hmdMove90.z) > std::fabs(hmdMove90.x));

    frame.devices[DeviceSlot(DeviceIndex::LeftController)].position.y = 1.99f;
    leftDrag = BeginDrag(frame, DeviceIndex::LeftController);
    ApplyDragDelta(leftDrag, frame, 50.0f, -1000.0f, ManipulationModifier_None);
    EXPECT_NEAR(frame.devices[DeviceSlot(DeviceIndex::LeftController)].position.y, 2.0f, 0.0001f);

    frame = BuildResetTPose(MakeNeutralFrame());
    leftDrag = BeginDrag(frame, DeviceIndex::LeftController);
    ApplyDragDelta(leftDrag, frame, 20.0f, -10.0f, ManipulationModifier_None);
    ApplySymmetricMirror(leftDrag, frame, DeviceIndex::RightController);
    const Vec3 leftLocal = frame.devices[DeviceSlot(DeviceIndex::LeftController)].position;
    const Vec3 rightLocal = frame.devices[DeviceSlot(DeviceIndex::RightController)].position;
    EXPECT_NEAR(rightLocal.x, -leftLocal.x, 0.0001f);
    EXPECT_NEAR(rightLocal.y, leftLocal.y, 0.0001f);
    EXPECT_NEAR(rightLocal.z, leftLocal.z, 0.0001f);

    frame = BuildResetTPose(MakeNeutralFrame());
    frame.devices[DeviceSlot(DeviceIndex::Hmd)].rotation = FromYaw(DegToRad(90.0f));
    frame = BuildResetTPose(frame);
    leftDrag = BeginDrag(frame, DeviceIndex::LeftController);
    ApplyDragDelta(leftDrag, frame, 20.0f, -10.0f, ManipulationModifier_None);
    ApplySymmetricMirror(leftDrag, frame, DeviceIndex::RightController);
    const Quat yawBasis = FromYaw(leftDrag.hmdYawBasis);
    const Vec3 hmdOrigin = frame.devices[DeviceSlot(DeviceIndex::Hmd)].position;
    const Vec3 yawLeftLocal = Rotate(Conjugate(yawBasis), {
        frame.devices[DeviceSlot(DeviceIndex::LeftController)].position.x - hmdOrigin.x,
        frame.devices[DeviceSlot(DeviceIndex::LeftController)].position.y - hmdOrigin.y,
        frame.devices[DeviceSlot(DeviceIndex::LeftController)].position.z - hmdOrigin.z,
    });
    const Vec3 yawRightLocal = Rotate(Conjugate(yawBasis), {
        frame.devices[DeviceSlot(DeviceIndex::RightController)].position.x - hmdOrigin.x,
        frame.devices[DeviceSlot(DeviceIndex::RightController)].position.y - hmdOrigin.y,
        frame.devices[DeviceSlot(DeviceIndex::RightController)].position.z - hmdOrigin.z,
    });
    EXPECT_NEAR(yawRightLocal.x, -yawLeftLocal.x, 0.0001f);
    EXPECT_NEAR(yawRightLocal.y, yawLeftLocal.y, 0.0001f);
    EXPECT_NEAR(yawRightLocal.z, yawLeftLocal.z, 0.0001f);

    frame = MakeNeutralFrame();
    frame.devices[DeviceSlot(DeviceIndex::Hmd)].position = {2.0f, 1.5f, -1.0f};
    frame.devices[DeviceSlot(DeviceIndex::Hmd)].rotation = FromYaw(DegToRad(90.0f));
    frame.devices[DeviceSlot(DeviceIndex::LeftController)].position =
        Add(frame.devices[DeviceSlot(DeviceIndex::Hmd)].position, Rotate(FromYaw(DegToRad(90.0f)), {0.30f, 0.10f, 0.40f}));
    frame.devices[DeviceSlot(DeviceIndex::LeftController)].rotation =
        Normalized(Multiply(FromYaw(DegToRad(90.0f)), FromAxisAngle({1.0f, 0.0f, 0.0f}, DegToRad(20.0f))));
    leftDrag = BeginDrag(frame, DeviceIndex::LeftController);
    ApplySymmetricMirror(leftDrag, frame, DeviceIndex::RightController, ManipulationFrame::Global);
    const Vec3 globalOrigin = frame.devices[DeviceSlot(DeviceIndex::Hmd)].position;
    const Vec3 globalLeft = frame.devices[DeviceSlot(DeviceIndex::LeftController)].position;
    const Vec3 globalRight = frame.devices[DeviceSlot(DeviceIndex::RightController)].position;
    EXPECT_NEAR(globalRight.x - globalOrigin.x, -(globalLeft.x - globalOrigin.x), 0.0001f);
    EXPECT_NEAR(globalRight.y - globalOrigin.y, globalLeft.y - globalOrigin.y, 0.0001f);
    EXPECT_NEAR(globalRight.z - globalOrigin.z, globalLeft.z - globalOrigin.z, 0.0001f);
    EXPECT_MIRRORED_IN_BASIS(frame, 0.0f, DeviceIndex::LeftController, DeviceIndex::RightController);

    frame = BuildResetTPose(MakeNeutralFrame());
    leftDrag = BeginDrag(frame, DeviceIndex::LeftFoot);
    ApplyDragDelta(leftDrag, frame, 0.0f, -20.0f, ManipulationModifier_Shift);
    ApplySymmetricMirror(leftDrag, frame, DeviceIndex::RightFoot);
    EXPECT_NEAR(frame.devices[DeviceSlot(DeviceIndex::RightFoot)].position.x, -frame.devices[DeviceSlot(DeviceIndex::LeftFoot)].position.x, 0.0001f);
    EXPECT_NEAR(frame.devices[DeviceSlot(DeviceIndex::RightFoot)].position.z, frame.devices[DeviceSlot(DeviceIndex::LeftFoot)].position.z, 0.0001f);

    frame = BuildResetTPose(MakeNeutralFrame());
    frame.devices[DeviceSlot(DeviceIndex::Hmd)].rotation = FromYaw(DegToRad(90.0f));
    frame = BuildResetTPose(frame);
    const Quat rightStartRotation = frame.devices[DeviceSlot(DeviceIndex::RightController)].rotation;
    leftDrag = BeginDrag(frame, DeviceIndex::LeftController);
    ApplySymmetricMirror(leftDrag, frame, DeviceIndex::RightController);
    EXPECT_SAME_ROTATION(frame.devices[DeviceSlot(DeviceIndex::RightController)].rotation, rightStartRotation);

    leftDrag = BeginDrag(frame, DeviceIndex::LeftController);
    ApplyDragDelta(leftDrag, frame, 0.0f, -30.0f, ManipulationModifier_Ctrl);
    ApplySymmetricMirror(leftDrag, frame, DeviceIndex::RightController);
    EXPECT_MIRRORED_IN_YAW_BASIS(frame, leftDrag, DeviceIndex::LeftController, DeviceIndex::RightController);

    leftDrag = BeginDrag(frame, DeviceIndex::LeftController);
    ApplyDragDelta(leftDrag, frame, 30.0f, 0.0f, ManipulationModifier_Ctrl);
    ApplySymmetricMirror(leftDrag, frame, DeviceIndex::RightController);
    EXPECT_MIRRORED_IN_YAW_BASIS(frame, leftDrag, DeviceIndex::LeftController, DeviceIndex::RightController);

    leftDrag = BeginDrag(frame, DeviceIndex::LeftController);
    ApplyDragDelta(leftDrag, frame, 30.0f, 0.0f, ManipulationModifier_Ctrl | ManipulationModifier_Shift);
    ApplySymmetricMirror(leftDrag, frame, DeviceIndex::RightController);
    EXPECT_MIRRORED_IN_YAW_BASIS(frame, leftDrag, DeviceIndex::LeftController, DeviceIndex::RightController);

    DeviceIndex mirroredDevice = DeviceIndex::Hmd;
    EXPECT_TRUE(MirroredDeviceFor(DeviceIndex::LeftController, mirroredDevice));
    EXPECT_TRUE(mirroredDevice == DeviceIndex::RightController);
    EXPECT_FALSE(MirroredDeviceFor(DeviceIndex::Hip, mirroredDevice));
}

void TestFingerBend() {
    // A single-finger adjust touches only that finger and clamps to [0, 1].
    FingerBends bends{};
    AdjustFingerBend(bends, Finger::Middle, 0.3f);
    EXPECT_NEAR(bends.middle, 0.3f, 0.0001f);
    EXPECT_NEAR(bends.thumb, 0.0f, 0.0001f);
    EXPECT_NEAR(bends.index, 0.0f, 0.0001f);
    EXPECT_NEAR(bends.ring, 0.0f, 0.0001f);
    EXPECT_NEAR(bends.pinky, 0.0f, 0.0001f);

    AdjustFingerBend(bends, Finger::Middle, 5.0f);  // clamps high
    EXPECT_NEAR(bends.middle, 1.0f, 0.0001f);
    AdjustFingerBend(bends, Finger::Middle, -5.0f);  // clamps low
    EXPECT_NEAR(bends.middle, 0.0f, 0.0001f);

    // FingerBendRef and FingerBendValue agree across every finger.
    for (Finger finger : {Finger::Thumb, Finger::Index, Finger::Middle, Finger::Ring, Finger::Pinky}) {
        FingerBends one{};
        AdjustFingerBend(one, finger, 0.5f);
        EXPECT_NEAR(FingerBendValue(one, finger), 0.5f, 0.0001f);
    }

    // Adjusting all fingers moves both hands together and clamps; scrolling far
    // enough drives every finger to fully open (0) or fully closed (1), which is
    // how the user resets the hand pose.
    FingerBends left{};
    FingerBends right{};
    left.thumb = 0.2f;
    right.pinky = 0.9f;
    AdjustAllFingerBends(left, right, 0.5f);
    EXPECT_NEAR(left.thumb, 0.7f, 0.0001f);
    EXPECT_NEAR(left.index, 0.5f, 0.0001f);
    EXPECT_NEAR(right.pinky, 1.0f, 0.0001f);  // 0.9 + 0.5 clamps to 1.0

    AdjustAllFingerBends(left, right, 10.0f);  // everything closes to 1.0
    for (Finger finger : {Finger::Thumb, Finger::Index, Finger::Middle, Finger::Ring, Finger::Pinky}) {
        EXPECT_NEAR(FingerBendValue(left, finger), 1.0f, 0.0001f);
        EXPECT_NEAR(FingerBendValue(right, finger), 1.0f, 0.0001f);
    }
    AdjustAllFingerBends(left, right, -10.0f);  // everything opens to 0.0
    for (Finger finger : {Finger::Thumb, Finger::Index, Finger::Middle, Finger::Ring, Finger::Pinky}) {
        EXPECT_NEAR(FingerBendValue(left, finger), 0.0f, 0.0001f);
        EXPECT_NEAR(FingerBendValue(right, finger), 0.0f, 0.0001f);
    }
}

void TestFingerGrip() {
    // A fully closed fist presses grip at full value.
    ControllerState c{};
    c.has_finger_bends = true;
    c.finger_bends = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    ApplyFingerGrip(c);
    EXPECT_TRUE(c.grip_click);
    EXPECT_NEAR(c.grip_value, 1.0f, 0.0001f);

    // Near-full (>= 0.95) still counts as a fist.
    c.finger_bends = {0.95f, 0.96f, 1.0f, 0.97f, 0.99f};
    ApplyFingerGrip(c);
    EXPECT_TRUE(c.grip_click);

    // Dropping any single finger below the threshold releases grip.
    c.finger_bends.index = 0.9f;
    ApplyFingerGrip(c);
    EXPECT_FALSE(c.grip_click);
    EXPECT_NEAR(c.grip_value, 0.0f, 0.0001f);

    // A controller with no finger data is left untouched.
    ControllerState none{};
    none.grip_click = true;
    ApplyFingerGrip(none);
    EXPECT_TRUE(none.grip_click);

    // Grip is per hand: applying to one controller does not touch another.
    std::array<ControllerState, 2> dance{};
    dance[0].has_finger_bends = true;
    dance[0].finger_bends = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};  // left fist
    dance[1].has_finger_bends = true;
    dance[1].finger_bends = {1.0f, 1.0f, 1.0f, 0.5f, 1.0f};  // right open ring
    std::array<ControllerState, 2> controllers{};
    std::array<FingerBends, 2> store{};
    ApplyDanceFingerBends(dance, controllers, store);
    EXPECT_TRUE(controllers[0].grip_click);
    EXPECT_FALSE(controllers[1].grip_click);
}

void TestApplyDanceFingerBends() {
    // A dance that drives the left hand pushes its fingers onto both the live
    // controllers and the persistent store, so Save Pose (which reads the store)
    // captures the dance's hands instead of a stale wheel value. The right hand,
    // which the dance does not drive, keeps its existing state untouched.
    std::array<ControllerState, 2> dance{};
    dance[0].has_finger_bends = true;
    dance[0].finger_bends = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};

    std::array<ControllerState, 2> controllers{};
    controllers[1].has_finger_bends = true;
    controllers[1].finger_bends = {0.6f, 0.6f, 0.6f, 0.6f, 0.6f};

    std::array<FingerBends, 2> store{};
    store[0] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f};  // stale manual-scroll value

    ApplyDanceFingerBends(dance, controllers, store);

    EXPECT_TRUE(controllers[0].has_finger_bends);
    EXPECT_NEAR(controllers[0].finger_bends.thumb, 0.1f, 0.0001f);
    EXPECT_NEAR(controllers[0].finger_bends.pinky, 0.5f, 0.0001f);
    EXPECT_NEAR(store[0].thumb, 0.1f, 0.0001f);  // the fix: store mirrors the dance
    EXPECT_NEAR(store[0].pinky, 0.5f, 0.0001f);

    // Undriven controller and its store slot are left as they were.
    EXPECT_TRUE(controllers[1].has_finger_bends);
    EXPECT_NEAR(controllers[1].finger_bends.thumb, 0.6f, 0.0001f);
    EXPECT_NEAR(store[1].thumb, 0.0f, 0.0001f);
}

void TestLog() {
    UdpLog log;
    log.Add("Reset to T-Pose", "Sent", "payload1");
    EXPECT_TRUE(log.Entries().size() == 1);
    EXPECT_TRUE(log.Entries().back().payload == "payload1");
    log.AddManipulation("Sent", "payload2");
    log.AddManipulation("Sent", "payload3");
    EXPECT_TRUE(log.Entries().back().payload == "payload3");
    log.Add("Socket error", "Failed", "payload4", "err");
    EXPECT_TRUE(log.Entries().back().detail == "err");
    for (int i = 0; i < 1100; ++i) {
        log.Add("Keyboard/button state", "Sent", "x");
    }
    EXPECT_TRUE(log.Entries().size() == 1000);
}

void TestJson() {
    // Round-trips and preserves unrelated keys while editing nested values, the
    // way registration edits openvrpaths.vrpath and steamvr.vrsettings.
    const std::string source =
        "{ \"config\" : [ \"C:\\\\Steam\\\\config\" ], \"external_drivers\" : null, "
        "\"steamvr\" : { \"installID\" : \"abc\", \"requireHmd\" : false }, \"version\" : 1 }";
    auto parsed = anyadance::json::Parse(source);
    EXPECT_TRUE(parsed.has_value());

    anyadance::json::Value root = *parsed;
    EXPECT_TRUE(root.IsObject());
    EXPECT_TRUE(root.Find("version") != nullptr && root.Find("version")->number == 1.0);

    // Add a driver path to a previously-null external_drivers.
    anyadance::json::Value drivers = anyadance::json::Value::Array();
    drivers.array.push_back(anyadance::json::Value::String("C:\\app\\anyadance"));
    root.Set("external_drivers", drivers);

    // Edit a nested object value without disturbing siblings.
    anyadance::json::Value* steamvr = root.Find("steamvr");
    EXPECT_TRUE(steamvr != nullptr);
    steamvr->Set("requireHmd", anyadance::json::Value::Bool(true));
    steamvr->Set("forcedDriver", anyadance::json::Value::String("anyadance"));

    auto reparsed = anyadance::json::Parse(anyadance::json::Serialize(root));
    EXPECT_TRUE(reparsed.has_value());
    const anyadance::json::Value* cfg = reparsed->Find("config");
    EXPECT_TRUE(cfg != nullptr && cfg->IsArray() && cfg->array.size() == 1);
    EXPECT_TRUE(cfg->array[0].string == "C:\\Steam\\config");
    const anyadance::json::Value* ed = reparsed->Find("external_drivers");
    EXPECT_TRUE(ed != nullptr && ed->IsArray() && ed->array.size() == 1);
    EXPECT_TRUE(ed->array[0].string == "C:\\app\\anyadance");
    const anyadance::json::Value* sv = reparsed->Find("steamvr");
    EXPECT_TRUE(sv != nullptr);
    EXPECT_TRUE(sv->Find("requireHmd") != nullptr && sv->Find("requireHmd")->boolean);
    EXPECT_TRUE(sv->Find("forcedDriver") != nullptr && sv->Find("forcedDriver")->string == "anyadance");
    EXPECT_TRUE(sv->Find("installID") != nullptr && sv->Find("installID")->string == "abc");

    EXPECT_TRUE(!anyadance::json::Parse("{ bad json ").has_value());
}

// Build a simple forward-facing standing skeleton in OpenVR convention so the
// retarget has a known rest to scale against.
SolvedJoint MakeJoint(Vec3 position) {
    return SolvedJoint{position, Quat{}};
}

SolvedMotion MakeStandingMotion() {
    SolvedMotion motion;
    motion.fps = 30.0f;
    motion.hasRest = true;
    auto& r = motion.rest;
    r[JointSlot(SolvedJointId::Pelvis)] = MakeJoint({0.0f, 0.90f, 0.0f});
    r[JointSlot(SolvedJointId::Head)] = MakeJoint({0.0f, 1.50f, 0.0f});
    r[JointSlot(SolvedJointId::LeftShoulder)] = MakeJoint({-0.18f, 1.35f, 0.0f});
    r[JointSlot(SolvedJointId::RightShoulder)] = MakeJoint({0.18f, 1.35f, 0.0f});
    r[JointSlot(SolvedJointId::LeftElbow)] = MakeJoint({-0.40f, 1.35f, 0.0f});
    r[JointSlot(SolvedJointId::RightElbow)] = MakeJoint({0.40f, 1.35f, 0.0f});
    r[JointSlot(SolvedJointId::LeftWrist)] = MakeJoint({-0.60f, 1.35f, 0.0f});
    r[JointSlot(SolvedJointId::RightWrist)] = MakeJoint({0.60f, 1.35f, 0.0f});
    r[JointSlot(SolvedJointId::LeftAnkle)] = MakeJoint({-0.10f, 0.10f, 0.0f});
    r[JointSlot(SolvedJointId::RightAnkle)] = MakeJoint({0.10f, 0.10f, 0.0f});
    r[JointSlot(SolvedJointId::LeftToe)] = MakeJoint({-0.10f, 0.00f, 0.10f});
    r[JointSlot(SolvedJointId::RightToe)] = MakeJoint({0.10f, 0.00f, 0.10f});

    SolvedFrame frame0;
    frame0.t = 0.0f;
    frame0.joints = motion.rest;
    SolvedFrame frame1 = frame0;
    frame1.t = 1.0f;
    frame1.joints[JointSlot(SolvedJointId::Head)] = MakeJoint({0.0f, 1.60f, 0.0f});
    motion.frames = {frame0, frame1};
    return motion;
}

void TestMmdParse() {
    const std::string json =
        "{ \"format\":\"anyadance_mmd_solved\", \"version\":1, \"fps\":60, \"model\":\"Miku\", "
        "\"has_fingers\":true, "
        "\"rest\": { "
        "\"pelvis\":{\"p\":[0,0.9,0],\"q\":[0,0,0,1]}, \"head\":{\"p\":[0,1.5,0],\"q\":[0,0,0,1]}, "
        "\"left_shoulder\":{\"p\":[-0.18,1.35,0],\"q\":[0,0,0,1]}, \"right_shoulder\":{\"p\":[0.18,1.35,0],\"q\":[0,0,0,1]}, "
        "\"left_elbow\":{\"p\":[-0.4,1.35,0],\"q\":[0,0,0,1]}, \"right_elbow\":{\"p\":[0.4,1.35,0],\"q\":[0,0,0,1]}, "
        "\"left_wrist\":{\"p\":[-0.6,1.35,0],\"q\":[0,0,0,1]}, \"right_wrist\":{\"p\":[0.6,1.35,0],\"q\":[0,0,0,1]}, "
        "\"left_ankle\":{\"p\":[-0.1,0.1,0],\"q\":[0,0,0,1]}, \"right_ankle\":{\"p\":[0.1,0.1,0],\"q\":[0,0,0,1]}, "
        "\"left_toe\":{\"p\":[-0.1,0,0.1],\"q\":[0,0,0,1]}, \"right_toe\":{\"p\":[0.1,0,0.1],\"q\":[0,0,0,1]} }, "
        "\"frames\": [ { \"t\":0, \"j\": { "
        "\"pelvis\":{\"p\":[0,0.9,0],\"q\":[0,0,0,1]}, \"head\":{\"p\":[0,1.5,0],\"q\":[0,0,0,1]}, "
        "\"left_shoulder\":{\"p\":[-0.18,1.35,0],\"q\":[0,0,0,1]}, \"right_shoulder\":{\"p\":[0.18,1.35,0],\"q\":[0,0,0,1]}, "
        "\"left_elbow\":{\"p\":[-0.4,1.35,0],\"q\":[0,0,0,1]}, \"right_elbow\":{\"p\":[0.4,1.35,0],\"q\":[0,0,0,1]}, "
        "\"left_wrist\":{\"p\":[-0.6,1.35,0],\"q\":[0,0,0,1]}, \"right_wrist\":{\"p\":[0.6,1.35,0],\"q\":[0,0,0,1]}, "
        "\"left_ankle\":{\"p\":[-0.1,0.1,0],\"q\":[0,0,0,1]}, \"right_ankle\":{\"p\":[0.1,0.1,0],\"q\":[0,0,0,1]}, "
        "\"left_toe\":{\"p\":[-0.1,0,0.1],\"q\":[0,0,0,1]}, \"right_toe\":{\"p\":[0.1,0,0.1],\"q\":[0,0,0,1]} }, "
        "\"fl\":[0.1,0.2,0.3,0.4,0.5], \"fr\":[0.5,0.4,0.3,0.2,0.1] } ] }";
    SolvedMotion motion;
    std::string error;
    EXPECT_TRUE(ParseSolvedMotion(json, motion, error));
    EXPECT_NEAR(motion.fps, 60.0f, 0.001f);
    EXPECT_TRUE(motion.hasRest);
    EXPECT_TRUE(motion.hasFingers);
    EXPECT_TRUE(motion.frames.size() == 1);
    EXPECT_TRUE(motion.frames[0].hasFingers);
    EXPECT_NEAR(motion.frames[0].leftFingers[2], 0.3f, 0.001f);
    EXPECT_NEAR(motion.rest[JointSlot(SolvedJointId::Head)].position.y, 1.5f, 0.001f);

    SolvedMotion bad;
    EXPECT_FALSE(ParseSolvedMotion("{ \"frames\": [] }", bad, error));
}

void TestMmdRetarget() {
    SolvedMotion motion = MakeStandingMotion();
    MmdRetargetParams params;
    params.targetHeightM = 1.62f;  // matches the source height so scale == 1
    params.handReachScale = 1.0f;
    params.headMountUpM = 0.10f;
    params.floorOffsetM = 0.0f;

    DanceMotion dance = BuildDanceMotion(motion, params);
    EXPECT_TRUE(dance.valid);
    EXPECT_TRUE(dance.hasFingers == false);  // synthetic frames carry no fingers
    EXPECT_NEAR(dance.scale, 1.0f, 0.01f);
    EXPECT_NEAR(dance.sourceHeightM, 1.62f, 0.01f);
    EXPECT_TRUE(dance.frames.size() == 2);

    // At the rest pose, devices sit at their scaled joints (feet on the floor, HMD
    // lifted by the head-mount offset) and every rotation is upright.
    const FrameState& rest = dance.frames[0];
    EXPECT_VEC3_NEAR(rest.devices[DeviceSlot(DeviceIndex::Hmd)].position, (Vec3{0.0f, 1.60f, 0.0f}));
    EXPECT_VEC3_NEAR(rest.devices[DeviceSlot(DeviceIndex::Hip)].position, (Vec3{0.0f, 0.90f, 0.0f}));
    EXPECT_VEC3_NEAR(rest.devices[DeviceSlot(DeviceIndex::LeftFoot)].position, (Vec3{-0.10f, 0.10f, 0.0f}));
    EXPECT_VEC3_NEAR(rest.devices[DeviceSlot(DeviceIndex::RightFoot)].position, (Vec3{0.10f, 0.10f, 0.0f}));
    // Hands target the palm (wrist extended along the forearm by 0.21).
    EXPECT_NEAR(rest.devices[DeviceSlot(DeviceIndex::RightController)].position.x, 0.642f, 0.001f);
    EXPECT_SAME_ROTATION(rest.devices[DeviceSlot(DeviceIndex::Hmd)].rotation, Quat{});
    EXPECT_SAME_ROTATION(rest.devices[DeviceSlot(DeviceIndex::LeftFoot)].rotation, Quat{});

    // Steering is controller input, so pelvis orientation keeps controller
    // positions left/right in solve coordinates.
    SolvedMotion orientedMotion = MakeStandingMotion();
    orientedMotion.rest[JointSlot(SolvedJointId::Pelvis)].rotation = FromYaw(DegToRad(90.0f));
    orientedMotion.frames[0].joints[JointSlot(SolvedJointId::Pelvis)].rotation = FromYaw(DegToRad(90.0f));
    orientedMotion.frames[1].joints[JointSlot(SolvedJointId::Pelvis)].rotation = FromYaw(DegToRad(90.0f));
    DanceMotion orientedDance = BuildDanceMotion(orientedMotion, params);
    EXPECT_TRUE(orientedDance.valid);
    EXPECT_TRUE(orientedDance.frames[0].devices[DeviceSlot(DeviceIndex::RightController)].position.x > 0.5f);
    EXPECT_TRUE(orientedDance.frames[0].devices[DeviceSlot(DeviceIndex::LeftController)].position.x < -0.5f);

    // Sampling the head bob halfway lands between the two frames.
    FrameState mid = SampleDanceMotion(dance, 0.5f, false);
    EXPECT_NEAR(mid.devices[DeviceSlot(DeviceIndex::Hmd)].position.y, 1.65f, 0.01f);

    // Anchoring shifts X/Z without touching height.
    AnchorDanceFrame(mid, 2.0f, -3.0f);
    EXPECT_NEAR(mid.devices[DeviceSlot(DeviceIndex::Hip)].position.x, 2.0f, 0.001f);
    EXPECT_NEAR(mid.devices[DeviceSlot(DeviceIndex::Hip)].position.z, -3.0f, 0.001f);
    EXPECT_NEAR(mid.devices[DeviceSlot(DeviceIndex::Hmd)].position.y, 1.65f, 0.01f);

    motion = MakeStandingMotion();
    motion.hasFingers = true;
    motion.frames[0].hasFingers = true;
    motion.frames[0].leftFingers = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f};
    motion.frames[0].rightFingers = {0.8f, 0.6f, 0.4f, 0.2f, 0.0f};
    motion.frames[1].hasFingers = true;
    motion.frames[1].leftFingers = {1.0f, 0.8f, 0.6f, 0.4f, 0.2f};
    motion.frames[1].rightFingers = {0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
    dance = BuildDanceMotion(motion, params);
    EXPECT_TRUE(dance.valid);
    EXPECT_TRUE(dance.hasFingers);
    mid = SampleDanceMotion(dance, 0.5f, false);
    EXPECT_TRUE(mid.controllers[0].has_finger_bends);
    EXPECT_TRUE(mid.controllers[1].has_finger_bends);
    EXPECT_NEAR(mid.controllers[0].finger_bends.thumb, 0.5f, 0.001f);
    EXPECT_NEAR(mid.controllers[0].finger_bends.index, 0.5f, 0.001f);
    EXPECT_NEAR(mid.controllers[1].finger_bends.thumb, 0.5f, 0.001f);
    EXPECT_NEAR(mid.controllers[1].finger_bends.pinky, 0.5f, 0.001f);
}

void TestNya() {
    // A saved pose is a one-frame looping clip; round-tripping it restores the
    // device poses and finger bends exactly.
    FrameState frame = BuildResetTPose(MakeNeutralFrame());
    frame.devices[DeviceSlot(DeviceIndex::LeftFoot)].position = {0.3f, 0.1f, -0.2f};
    frame.devices[DeviceSlot(DeviceIndex::Hmd)].position.y = 1.7f;
    frame.controllers[0].has_finger_bends = true;
    frame.controllers[0].finger_bends = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
    frame.controllers[1].has_finger_bends = true;
    frame.controllers[1].finger_bends = {0.5f, 0.4f, 0.3f, 0.2f, 0.1f};

    NyaClip pose = MakePoseClip(frame);
    EXPECT_TRUE(pose.IsPose());
    EXPECT_TRUE(pose.loop);

    NyaClip roundTrip;
    std::string error;
    EXPECT_TRUE(ParseNya(SerializeNya(pose), roundTrip, error));
    EXPECT_TRUE(roundTrip.IsPose());
    EXPECT_TRUE(roundTrip.motion.valid);
    const FrameState& got = roundTrip.motion.frames[0];
    EXPECT_VEC3_NEAR(got.devices[DeviceSlot(DeviceIndex::LeftFoot)].position, (Vec3{0.3f, 0.1f, -0.2f}));
    EXPECT_NEAR(got.devices[DeviceSlot(DeviceIndex::Hmd)].position.y, 1.7f, 0.0001f);
    EXPECT_TRUE(got.controllers[0].has_finger_bends);
    EXPECT_NEAR(got.controllers[0].finger_bends.thumb, 0.1f, 0.0001f);
    EXPECT_NEAR(got.controllers[1].finger_bends.thumb, 0.5f, 0.0001f);

    // A multi-frame animation round-trips its times and frame count, and Y is
    // clamped to the 2 m ceiling on load even if the source exceeds it.
    DanceMotion motion;
    for (int i = 0; i < 3; ++i) {
        FrameState f = BuildResetTPose(MakeNeutralFrame());
        f.devices[DeviceSlot(DeviceIndex::Hmd)].position.y = 5.0f;  // over the ceiling
        motion.frames.push_back(f);
        motion.times.push_back(static_cast<float>(i) * 0.5f);
    }
    motion.valid = true;
    NyaClip anim = MakeAnimationClip(motion, 60.0f, "test_model");
    NyaClip animBack;
    EXPECT_TRUE(ParseNya(SerializeNya(anim), animBack, error));
    EXPECT_FALSE(animBack.IsPose());
    EXPECT_TRUE(animBack.motion.frames.size() == 3);
    EXPECT_NEAR(animBack.motion.times[2], 1.0f, 0.0001f);
    EXPECT_TRUE(animBack.model == "test_model");
    EXPECT_NEAR(animBack.motion.frames[0].devices[DeviceSlot(DeviceIndex::Hmd)].position.y, kMaxDeviceY, 0.0001f);

    // A document without the format tag is rejected.
    NyaClip rejected;
    EXPECT_FALSE(ParseNya("{\"frames\":[]}", rejected, error));
}

} // namespace

int main() {
    TestProtocol();
    TestSafety();
    TestTPose();
    TestInput();
    TestManipulation();
    TestFingerBend();
    TestFingerGrip();
    TestApplyDanceFingerBends();
    TestLog();
    TestJson();
    TestMmdParse();
    TestMmdRetarget();
    TestNya();
    if (g_failures != 0) {
        std::cerr << g_failures << " test failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "All AnyaDance tests passed\n";
    return EXIT_SUCCESS;
}
