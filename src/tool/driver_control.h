#pragma once

#include <string>

namespace anyadance::tool {

// Outcome codes; the UI maps these to localized strings so driver_control stays
// free of presentation concerns.
enum class DriverStatus {
    Registered,
    Unregistered,
    ManifestMissing,
    DriverDllMissing,
    OpenvrPathsMissing,
    ConfigWriteFailed,
    Restarting,
    RestartFailed,
    Failed,
};

struct DriverActionResult {
    bool ok = false;
    DriverStatus status = DriverStatus::Failed;
    std::string detail; // optional extra context (e.g. an exception message)
};

// Register/unregister the driver by editing the user's openvrpaths.vrpath and
// steamvr.vrsettings, the same files vrpathreg manipulates. The driver root is
// the tool executable's own folder, so the shipped layout is a single flat
// folder: the exe alongside driver.vrdrivermanifest, bin/win64/driver_anyadance.dll,
// and resources/. RestartSteamVR stops the SteamVR processes and relaunches via
// Steam.
DriverActionResult RegisterDriver();
DriverActionResult UnregisterDriver();
DriverActionResult RestartSteamVR();

} // namespace anyadance::tool
