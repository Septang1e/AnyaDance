# AnyaDance

![Project Anya banner](driver/resources/images/anya_banner.png)

**English** | [简体中文](README.zh-CN.md)

AnyaDance is a Windows toolkit for driving and animating a VRChat avatar's full body — pose it by hand, drive it live, or play an MMD dance on it. At its core is a SteamVR/OpenVR virtual-device driver plus a companion tool (`AnyaDance.exe`) that streams to it. The driver exposes six virtual devices for VRChat full-body testing:

- HMD
- left controller
- right controller
- hip tracker
- left foot tracker
- right foot tracker

The driver receives pose and controller-input frames over UDP JSON on `127.0.0.1:39570`. The companion executable, `AnyaDance.exe`, starts streaming a six-device T-pose at 60 Hz as soon as it opens.

AnyaDance is released as part of **Project Anya**, a larger project by Pipira. Project Anya itself is proprietary. Crediting Project Anya when you use or build on this driver is highly appreciated.

## Disclaimer

This software is provided for legitimate, authorized testing and development only.

Feeding virtual devices or spoofed tracking into a live online game may violate that game's Terms of Service and can be detected by its anti-cheat system, which may result in the suspension or permanent ban of your account.

You use this software entirely at your own risk. It is provided "as is" without warranty of any kind, and the authors accept no responsibility or liability for any consequences of use or misuse, including account bans or loss of access. You agree to hold the authors harmless from any claim arising out of your use.

This project is not affiliated with or endorsed by VRChat, Valve, Steam, or SteamVR. All trademarks belong to their respective owners.

See [DISCLAIMER.md](DISCLAIMER.md) for the full text. The companion tool also shows this disclaimer and requires acceptance on first launch.

## Status

The code builds and its automated tests pass on Windows with Visual Studio 2022. The shipped tool registers the SteamVR driver, streams six virtual devices, and packages the driver bundle from the same build.

## Requirements

- Windows 10 or newer
- SteamVR installed
- Visual Studio 2022 or Visual Studio Build Tools with Desktop development with C++
- CMake 3.22 or newer
- Network access for the default first build, unless local dependency paths are supplied

Pinned dependencies:

- Valve OpenVR SDK `2.2.3`
- Dear ImGui `v1.90.9`

## Build

```powershell
.\scripts\build_driver.ps1
```

Outputs:

```text
build\out\anyadance\AnyaDance.exe
build\out\anyadance\driver.vrdrivermanifest
build\out\anyadance\bin\win64\driver_anyadance.dll
build\out\anyadance\resources\...
build\out\AnyaDance.zip
```

The tool ships inside the driver folder, so `build\out\anyadance\` is one
self-contained, shippable bundle, and the build also zips it into
`build\out\AnyaDance.zip` to hand to others directly. The exe registers its own folder as the SteamVR
driver, so OpenVR finds `driver.vrdrivermanifest` and `bin\win64\driver_anyadance.dll`
beside it. OpenVR loads the driver DLL from `bin\win64\`, with the manifest at
the driver root.

To use local dependency checkouts:

```powershell
.\scripts\build_driver.ps1 -OpenVRSdkRoot F:\deps\openvr -ImguiRoot F:\deps\imgui
```

## Test

```powershell
cmake --build build --config Release --target anyadance_tests
ctest --test-dir build -C Release --output-on-failure
```

The tests cover protocol validation, safety clamping, T-pose reset math, keyboard controls, mouse manipulation math, MMD remapping, `.nya` clip handling, and UDP log behavior.

## Register

```powershell
.\scripts\register_driver.ps1
.\scripts\restart_steamvr.ps1
```

`register_driver.ps1` registers the driver and applies the fully virtual settings (virtual HMD, controllers, and trackers enabled), backing up `steamvr.vrsettings` to `%LOCALAPPDATA%\AnyaDance\steamvr.vrsettings.backup` first.

SteamVR must be restarted after registration changes and after rebuilding the driver DLL.

## Run The Test UI

```powershell
.\build\out\anyadance\AnyaDance.exe
```

The UI:

- starts 60 Hz UDP streaming automatically
- continues streaming while minimized or unfocused
- sends a final neutral-input frame on normal exit
- keeps the UDP log in English, omits unchanged keepalive packets, and names what each entry was: the key and action that changed (for example `Z left trigger down`), the device that was dragged (for example `Hip manipulated`), or a finger-bend change
- shows hover or pinned row detail with three payload actions: Copy (the raw request body), Copy resend command (a runnable PowerShell UDP one-liner), and Resend (replays the exact datagram from the tool over its own socket)
- can register/unregister its own folder as the SteamVR driver and restart SteamVR (with a confirmation) from its own buttons
- has an Always on top checkbox that pins the window above other windows; the choice is remembered between runs
- can play an MMD dance on the fly: the **Dance (MMD)** button opens a dialog to pick a `.vmd` motion and a `.pmx`/`.pmd` model, then Analyze and Play stream the dance onto the six devices (see [docs/mmd-dance.md](docs/mmd-dance.md))
- can save and restore poses and dances as `.nya` clips: **Save Pose** / **Load Pose** in the main window capture and restore the current pose, and the Dance dialog can **Save .nya** of an analyzed dance and **Load .nya** to play it again without re-solving
- supports English and Simplified Chinese UI strings through `src/tool/localization.*`

Key bindings:

```text
WASD  left joystick movement
Q/E   right joystick turn
Space right A while held
M     right B while held
V     left A while held
Z     left trigger while held
X     right trigger while held
```

Mouse manipulation uses the six device boxes. The capture panel and boxes resize with the tool window. The HMD box allows rotation, plus vertical (Y) movement with a left+right mouse drag (clamped to the 2 m Y limit). Other devices use left mouse drag for local X/Y movement, middle mouse drag for rotation, and right mouse drag for depth movement. The HMD/Global frame radio buttons choose whether manipulation uses the HMD yaw basis or fixed world axes. The hand and foot pair mirror checkboxes use the same frame setting: HMD mode mirrors across the HMD-yaw YZ plane, and Global mode mirrors across world axes centered on the HMD position. The mouse wheel opens and closes both hands' fingers. Hold a number key while scrolling to bend a single finger: `1`-`5` are the left hand from pinky to thumb, `6`-`0` are the right hand from thumb to pinky (so `5`/`6` are the thumbs and `1`/`0` the pinkies). Each finger is clamped to `[0, 1]`, and scrolling all the way in one direction resets every finger to fully open or fully closed.

Dragging the empty area of the body panel acts as the right thumbstick: the press point is the stick center, and dragging deflects it within ±1 on each axis, returning to neutral on release. This is meant for navigating the right-hand quick menu (opened by holding `M`).

## MMD Dance

The **Dance (MMD)** button plays an MMD dance on the six virtual devices live in
memory. Blender + MMD Tools solves the `.vmd` motion against a model you supply
(PMX/PMD), and the tool does a small remapping onto the hardcoded rig and streams
it at 60 Hz. Pick the VMD and model in the dialog, hit Analyze, then Play. Use
**Advanced** to set Blender and MMD Tools paths for custom installs; the tool
remembers those paths.

Requirements: [Blender](https://www.blender.org/) and the
[MMD Tools](https://github.com/MMD-Blender/blender_mmd_tools) add-on (both
auto-detected), plus your own model. MMD models are third-party works with their
own licenses. See [docs/mmd-dance.md](docs/mmd-dance.md) for details, parameters,
and limits.

Once a dance is analyzed, **Save .nya** writes the result to a clip file. **Load
.nya** reads one back and enables Play immediately — loading skips both the
Blender solve and the remap, so a saved dance plays instantly.

## Clip files (.nya)

A `.nya` file is a small JSON clip of device-level frames — the six device poses
plus per-hand finger bends — ready to stream with no further conversion. The
format is the same for poses and animations: a **pose** is a one-frame clip
(played as a held loop of that single frame) and an **animation** (such as a
saved MMD dance) is many timed frames. Device Y is clamped to the 2 m limit and
finger bends to `[0, 1]` on load, so an edited file can never exceed the safe
ranges.

## Safety And Liveness

All six devices have a hard maximum Y value of `2.0 m`. The tool clamps before serialization and the native driver clamps again after packet validation.

All six devices start connected and valid at neutral poses. Accepted packets update the latest pose and controller inputs. If packets stop, SteamVR continues to see each device connected, valid, and `TrackingResult_Running_OK` at its last accepted pose.

## Protocol Overview

- UDP to `127.0.0.1:39570`
- UTF-8 JSON
- `version` must be `1`
- fire-and-forget datagrams
- accepted datagrams are smaller than 8192 bytes
- quaternion order is XYZW
- recognized device IDs are `hmd`, `left_controller`, `right_controller`, `hip`, `left_foot`, `right_foot`

See [docs/protocol.md](docs/protocol.md) for the full protocol.

## Unregister

```powershell
.\scripts\unregister_driver.ps1
.\scripts\restart_steamvr.ps1
```

## License

AnyaDance is open source, licensed under the Apache License, Version 2.0.

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) and the
[NOTICE](NOTICE) attribution file. Redistributions must preserve the NOTICE
contents and mark any modified files. Bundled third-party components keep their
own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
