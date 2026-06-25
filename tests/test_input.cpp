#include "test_framework.h"
#include "tests.h"

#include "core/frame_state.h"
#include "core/input_state.h"

#include <cmath>

namespace anyadance::tests {

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

    // S is backward: with W released, holding S drives the left stick to -Y.
    keys.HandleKey(ToolKey::W, false, false);
    keys.HandleKey(ToolKey::A, false, false);
    keys.HandleKey(ToolKey::D, false, false);
    keys.HandleKey(ToolKey::S, true, false);
    keys.UpdateFrameInputs(frame);
    EXPECT_NEAR(frame.controllers[0].joystick_y, -1.0f, 0.0001f);
    keys.HandleKey(ToolKey::S, false, false);

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

    // While an ImGui text edit is active, key edges are dropped so typing does not
    // leak into the avatar. A press made during editing never registers.
    KeyboardInputState editing;
    editing.SetTextEditing(true);
    EXPECT_FALSE(editing.HandleKey(ToolKey::W, true, false));
    editing.UpdateFrameInputs(frame);
    EXPECT_NEAR(frame.controllers[0].joystick_y, 0.0f, 0.0001f);
    // Once editing ends, keys work again.
    editing.SetTextEditing(false);
    editing.HandleKey(ToolKey::W, true, false);
    editing.UpdateFrameInputs(frame);
    EXPECT_NEAR(frame.controllers[0].joystick_y, 1.0f, 0.0001f);
    // Neutralize releases every held key.
    editing.Neutralize();
    editing.UpdateFrameInputs(frame);
    EXPECT_NEAR(frame.controllers[0].joystick_y, 0.0f, 0.0001f);

    // Losing focus neutralizes held keys and suppresses further mapping.
    keys.SetFocus(false);
    keys.UpdateFrameInputs(frame);
    EXPECT_FALSE(frame.controllers[0].trigger_click);
    EXPECT_FALSE(frame.controllers[1].trigger_click);
}

} // namespace anyadance::tests
