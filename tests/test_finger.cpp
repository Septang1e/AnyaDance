#include "test_framework.h"
#include "tests.h"

#include "core/finger_control.h"
#include "core/frame_state.h"

#include <array>

namespace anyadance::tests {

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

    // FingerBendRef and FingerBendValue agree across every finger, and a write
    // through the ref is observed by the value reader.
    for (Finger finger : {Finger::Thumb, Finger::Index, Finger::Middle, Finger::Ring, Finger::Pinky}) {
        FingerBends one{};
        FingerBendRef(one, finger) = 0.5f;
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

    // Just below the threshold on a single finger releases (boundary check).
    c.finger_bends = {0.95f, 0.95f, 0.95f, 0.95f, 0.9499f};
    ApplyFingerGrip(c);
    EXPECT_FALSE(c.grip_click);

    // Dropping any single finger well below the threshold releases grip.
    c.finger_bends = {0.95f, 0.96f, 1.0f, 0.97f, 0.99f};
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

} // namespace anyadance::tests
