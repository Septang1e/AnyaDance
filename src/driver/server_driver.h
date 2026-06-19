#pragma once

#include <openvr_driver.h>

#include <memory>
#include <string>
#include <vector>

#include "udp_pose_receiver.h"
#include "virtual_device.h"

class ServerDriver : public vr::IServerTrackedDeviceProvider {
public:
    ServerDriver();
    ~ServerDriver() = default;

    vr::EVRInitError Init(vr::IVRDriverContext* pDriverContext) override;
    void Cleanup() override;
    const char* const* GetInterfaceVersions() override;
    void RunFrame() override;
    bool ShouldBlockStandbyMode() override;
    void EnterStandby() override;
    void LeaveStandby() override;

private:
    struct DeviceSlot {
        std::string deviceId;
        std::unique_ptr<VirtualDevice> device;
        bool seenUdpPose = false;
    };

    std::vector<DeviceSlot> m_devices;
    std::unique_ptr<UdpPoseReceiver> m_poseReceiver;
    bool m_hasVirtualHmd = false;
};