#include "udp_pose_receiver.h"

#include "core/constants.h"
#include "core/protocol.h"
#include "log.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

namespace {
constexpr int kSocketTimeoutMs = 100;

bool SlotForDeviceId(const std::string& deviceId, std::size_t& slot) {
    for (const anyadance::DeviceInfo& device : anyadance::kDevices) {
        if (deviceId == device.id) {
            slot = anyadance::DeviceSlot(device.index);
            return true;
        }
    }
    return false;
}
}

UdpPoseReceiver::~UdpPoseReceiver() {
    Stop();
}

bool UdpPoseReceiver::Start(unsigned short port) {
    if (m_running.exchange(true)) {
        return true;
    }

    m_thread = std::thread(&UdpPoseReceiver::Run, this, port);
    return true;
}

void UdpPoseReceiver::Stop() {
    if (!m_running.exchange(false)) {
        return;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool UdpPoseReceiver::TryGetLatest(const std::string& deviceId, anyadance::PoseSample& sample) const {
    std::size_t slot = 0;
    if (!SlotForDeviceId(deviceId, slot)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hasLatest[slot]) {
        return false;
    }
    sample = m_latest[slot];
    return true;
}

void UdpPoseReceiver::Run(unsigned short port) {
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        DriverLog("[anyadance] WSAStartup failed\n");
        return;
    }

    SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET) {
        DriverLog("[anyadance] Failed to create UDP socket\n");
        WSACleanup();
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        DriverLog("[anyadance] Failed to bind UDP port %u\n", port);
        closesocket(socketHandle);
        WSACleanup();
        return;
    }

    setsockopt(
        socketHandle,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&kSocketTimeoutMs),
        sizeof(kSocketTimeoutMs));

    DriverLog("[anyadance] Listening for UDP pose frames on 127.0.0.1:%u\n", port);

    char buffer[anyadance::kMaxPacketBytes + 1]{};
    while (m_running.load()) {
        sockaddr_in sender{};
        int senderLen = sizeof(sender);
        const int size = recvfrom(
            socketHandle,
            buffer,
            anyadance::kMaxPacketBytes,
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &senderLen);

        if (size == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
                continue;
            }
            DriverLog("[anyadance] UDP receive error %d\n", error);
            continue;
        }

        if (size <= 0 || size >= anyadance::kMaxPacketBytes) {
            continue;
        }
        buffer[size] = '\0';
        StoreIfValid(buffer, size);
    }

    closesocket(socketHandle);
    WSACleanup();
}

bool UdpPoseReceiver::StoreIfValid(const char* data, int size) {
    anyadance::ParsedFrame parsed;
    if (!anyadance::ParsePoseFrameBytes(data, size, parsed)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::size_t slot = 0; slot < parsed.samples.size(); ++slot) {
        if (parsed.present[slot]) {
            m_latest[slot] = parsed.samples[slot];
            m_hasLatest[slot] = true;
        }
    }
    return true;
}