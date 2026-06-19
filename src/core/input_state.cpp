#include "core/input_state.h"

#include <cmath>

namespace anyadance {
namespace {

std::size_t Slot(ToolKey key) {
    return static_cast<std::size_t>(key);
}

float Axis(bool positive, bool negative) {
    return (positive ? 1.0f : 0.0f) + (negative ? -1.0f : 0.0f);
}

} // namespace

void KeyboardInputState::SetFocus(bool focused) {
    m_focused = focused;
    if (!focused) {
        Neutralize();
    }
}

void KeyboardInputState::SetTextEditing(bool textEditing) {
    m_textEditing = textEditing;
}

bool KeyboardInputState::HandleKey(ToolKey key, bool down, bool repeat) {
    if (!m_focused || m_textEditing) {
        return false;
    }
    if (repeat && down) {
        return false;  // ignore OS auto-repeat; a held key is one press, not many
    }
    const std::size_t slot = Slot(key);
    const bool changed = m_down[slot] != down;
    m_down[slot] = down;
    return changed;
}

void KeyboardInputState::UpdateFrameInputs(FrameState& frame) const {
    ControllerState left{};
    ControllerState right{};

    left.joystick_x = Axis(m_down[Slot(ToolKey::D)], m_down[Slot(ToolKey::A)]);
    left.joystick_y = Axis(m_down[Slot(ToolKey::W)], m_down[Slot(ToolKey::S)]);
    const float leftMagnitude = std::sqrt(left.joystick_x * left.joystick_x + left.joystick_y * left.joystick_y);
    if (leftMagnitude > 1.0f) {
        left.joystick_x /= leftMagnitude;
        left.joystick_y /= leftMagnitude;
    }
    left.trackpad_x = left.joystick_x;
    left.trackpad_y = left.joystick_y;

    right.joystick_x = Axis(m_down[Slot(ToolKey::E)], m_down[Slot(ToolKey::Q)]);
    right.trackpad_x = right.joystick_x;
    right.trackpad_y = right.joystick_y;

    // Jump/menu/voice follow their key directly: held key, held button.
    left.a_click = m_down[Slot(ToolKey::V)];
    right.a_click = m_down[Slot(ToolKey::Space)];
    right.b_click = m_down[Slot(ToolKey::M)];

    left.trigger_click = m_down[Slot(ToolKey::Z)];
    left.trigger_value = left.trigger_click ? 1.0f : 0.0f;
    right.trigger_click = m_down[Slot(ToolKey::X)];
    right.trigger_value = right.trigger_click ? 1.0f : 0.0f;

    frame.controllers[0] = left;
    frame.controllers[1] = right;
}

void KeyboardInputState::Neutralize() {
    m_down = {};
}

} // namespace anyadance