#pragma once

#include "core/constants.h"
#include "core/pose_sample.h"

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class UdpPoseReceiver {
public:
    UdpPoseReceiver() = default;
    ~UdpPoseReceiver();

    UdpPoseReceiver(const UdpPoseReceiver&) = delete;
    UdpPoseReceiver& operator=(const UdpPoseReceiver&) = delete;

    bool Start(unsigned short port);
    void Stop();
    bool TryGetLatest(const std::string& deviceId, anyadance::PoseSample& sample) const;

private:
    void Run(unsigned short port);
    bool StoreIfValid(const char* data, int size);

    mutable std::mutex m_mutex;
    std::array<anyadance::PoseSample, anyadance::kDevices.size()> m_latest{};
    std::array<bool, anyadance::kDevices.size()> m_hasLatest{};
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};