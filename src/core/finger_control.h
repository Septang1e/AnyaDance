#pragma once

#include "core/math3d.h"
#include "core/pose_sample.h"

namespace anyadance {

// Per-finger identity, ordered to match the members of FingerBends
// (thumb, index, middle, ring, pinky).
enum class Finger {
    Thumb = 0,
    Index = 1,
    Middle = 2,
    Ring = 3,
    Pinky = 4,
};

inline float& FingerBendRef(FingerBends& bends, Finger finger) {
    switch (finger) {
    case Finger::Thumb:  return bends.thumb;
    case Finger::Index:  return bends.index;
    case Finger::Middle: return bends.middle;
    case Finger::Ring:   return bends.ring;
    case Finger::Pinky:  return bends.pinky;
    }
    return bends.pinky;  // unreachable; satisfies non-void return
}

inline float FingerBendValue(const FingerBends& bends, Finger finger) {
    switch (finger) {
    case Finger::Thumb:  return bends.thumb;
    case Finger::Index:  return bends.index;
    case Finger::Middle: return bends.middle;
    case Finger::Ring:   return bends.ring;
    case Finger::Pinky:  return bends.pinky;
    }
    return bends.pinky;  // unreachable
}

// Add `increment` to one finger's bend, clamped to the [0, 1] range the protocol
// accepts. A positive increment closes the finger; a negative one opens it.
inline void AdjustFingerBend(FingerBends& bends, Finger finger, float increment) {
    float& value = FingerBendRef(bends, finger);
    value = ClampFloat(value + increment, 0.0f, 1.0f);
}

// Add `increment` to every finger of both hands, clamped to [0, 1]. Scrolling
// far enough in one direction drives all fingers to 0 or all to 1, which is how
// the user resets the whole hand pose.
inline void AdjustAllFingerBends(FingerBends& left, FingerBends& right, float increment) {
    for (Finger finger : {Finger::Thumb, Finger::Index, Finger::Middle, Finger::Ring, Finger::Pinky}) {
        AdjustFingerBend(left, finger, increment);
        AdjustFingerBend(right, finger, increment);
    }
}

} // namespace anyadance
