# Troubleshooting

## SteamVR Does Not See The Driver

Run:

```powershell
.\scripts\register_driver.ps1
```

Confirm that `%LOCALAPPDATA%\openvr\openvrpaths.vrpath` contains the staged `build\out\anyadance` path under `external_drivers`. Restart SteamVR after registration changes.

## Virtual HMD Does Not Appear

Re-run the registration script, which also applies the fully virtual settings:

```powershell
.\scripts\register_driver.ps1
.\scripts\restart_steamvr.ps1
```

If a physical HMD is connected, SteamVR may reject the second virtual HMD. Disconnect the physical HMD so the virtual one can become active.

## Controllers Or Trackers Do Not Move

Controllers and trackers stay connected and valid even when packets stop. If they do not move, start `AnyaDance.exe` and confirm it is streaming the full six-device frame at 60 Hz.

## HMD Goes Grey When The Tool Closes

With the virtual HMD enabled, the driver keeps the HMD connected and valid when packets stop. Check SteamVR logs for `[anyadance]` messages and verify that the loaded DLL is `driver_anyadance.dll` from the current build output.

## Key Bindings Do Not Work

The test tool only reads keyboard state while its window is focused. It also disables key mappings while an ImGui text edit widget is active. Click the tool window and try again.

## SteamVR Settings Look Wrong

The registration script backs up settings to:

```text
%LOCALAPPDATA%\AnyaDance\steamvr.vrsettings.backup
```

`unregister_driver.ps1` restores that backup and clears the backup file. For a manual restore, inspect the backup first, then copy it back. Restart SteamVR after changing `steamvr.vrsettings`.

## Packet Is Ignored

Check:

- `version` is `1`
- datagram is smaller than 8192 bytes
- device ID is one of the six recognized IDs
- all required fields are present for the device
- position values are finite and within +/-10 m before Y clamp
- quaternion order is XYZW and squared length is between 0.5 and 1.5
