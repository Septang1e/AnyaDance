#include "core/tpose.h"

namespace anyadance {
namespace {

Vec3 OffsetFromHmd(Vec3 hmdPosition, float yaw, Vec3 localOffset) {
    return Add(hmdPosition, Rotate(FromYaw(yaw), localOffset));
}

} // namespace

FrameState BuildResetTPose(const FrameState& current, TPoseConstants constants) {
    FrameState reset{};
    const DeviceState& currentHmd = current.devices[DeviceSlot(DeviceIndex::Hmd)];
    Vec3 resetHmdPosition{currentHmd.position.x, kResetHmdY, currentHmd.position.z};
    // Reset to a canonical forward-facing rig: zero the HMD yaw rather than keeping
    // the current heading. The avatar's yaw in VRChat is driven by locomotion, so
    // any leftover rig yaw here would desync the local rig from the in-game heading.
    // Building the whole body at yaw 0 keeps them aligned.
    const float hmdYaw = 0.0f;
    const Quat yawOnly = FromYaw(hmdYaw);

    for (DeviceState& device : reset.devices) {
        device.connected = true;
        device.valid = true;
        device.rotation = yawOnly;
    }

    // Reset uprights the head and faces it forward (no yaw/pitch/roll), so the
    // whole body faces the canonical direction.
    reset.devices[DeviceSlot(DeviceIndex::Hmd)].position = resetHmdPosition;
    reset.devices[DeviceSlot(DeviceIndex::Hmd)].rotation = yawOnly;

    reset.devices[DeviceSlot(DeviceIndex::LeftController)].position = OffsetFromHmd(resetHmdPosition, hmdYaw, constants.leftControllerOffset);
    reset.devices[DeviceSlot(DeviceIndex::RightController)].position = OffsetFromHmd(resetHmdPosition, hmdYaw, constants.rightControllerOffset);
    reset.devices[DeviceSlot(DeviceIndex::Hip)].position = OffsetFromHmd(resetHmdPosition, hmdYaw, constants.hipOffset);
    reset.devices[DeviceSlot(DeviceIndex::LeftFoot)].position = OffsetFromHmd(resetHmdPosition, hmdYaw, constants.leftFootOffset);
    reset.devices[DeviceSlot(DeviceIndex::RightFoot)].position = OffsetFromHmd(resetHmdPosition, hmdYaw, constants.rightFootOffset);

    reset.devices[DeviceSlot(DeviceIndex::LeftController)].rotation = Normalized(Multiply(yawOnly, kLeftControllerCanonicalRotation));
    reset.devices[DeviceSlot(DeviceIndex::RightController)].rotation = Normalized(Multiply(yawOnly, kRightControllerCanonicalRotation));
    reset.devices[DeviceSlot(DeviceIndex::Hip)].rotation = yawOnly;
    reset.devices[DeviceSlot(DeviceIndex::LeftFoot)].rotation = yawOnly;
    reset.devices[DeviceSlot(DeviceIndex::RightFoot)].rotation = yawOnly;

    NeutralizeControllerInputs(reset);
    ClampFrameY(reset);
    return reset;
}

} // namespace anyadance
