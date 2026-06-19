#pragma once

#include "core/frame_state.h"

#include <array>
#include <cstdint>

namespace anyadance {

enum class ToolKey : std::size_t {
    W = 0,
    A,
    S,
    D,
    Q,
    E,
    Space,
    M,
    V,
    Z,
    X,
    Count,
};

class KeyboardInputState {
public:
    void SetFocus(bool focused);
    void SetTextEditing(bool textEditing);
    // Record a key edge. Returns true if it changed the pressed state. OS key
    // auto-repeat (repeat == true) is ignored so a held key stays a single press.
    bool HandleKey(ToolKey key, bool down, bool repeat);
    // Map the current key state onto both controllers' axes and buttons. Every
    // input follows its key directly: held key means held button/axis.
    void UpdateFrameInputs(FrameState& frame) const;
    void Neutralize();

private:
    std::array<bool, static_cast<std::size_t>(ToolKey::Count)> m_down{};
    bool m_focused = true;
    bool m_textEditing = false;
};

} // namespace anyadance