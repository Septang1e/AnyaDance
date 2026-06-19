#include "server_driver.h"
#include <cstring>

// Exported factory function for the SteamVR driver loader.

#ifdef _WIN32
#define ANYADANCE_EXPORT __declspec(dllexport)
#else
#define ANYADANCE_EXPORT
#endif

extern "C" {

ANYADANCE_EXPORT void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode) {
    if (0 == std::strcmp(pInterfaceName, vr::IServerTrackedDeviceProvider_Version)) {
        static ServerDriver serverDriver;
        if (pReturnCode) {
            *pReturnCode = vr::VRInitError_None;
        }
        return static_cast<vr::IServerTrackedDeviceProvider*>(&serverDriver);
    }
    if (pReturnCode) {
        *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
    }
    return nullptr;
}

} // extern "C"
