# Device Model

## Public Identifiers

```text
Repository/project:   AnyaDance
Driver slug:          anyadance
CMake project:        AnyaDance
Driver target:        driver_anyadance
Driver DLL:           driver_anyadance.dll
Driver folder:        anyadance
Settings section:     driver_anyadance
Manifest name:        anyadance
Serial prefix:        anyadance_
Resource namespace:   {anyadance}
Log prefix:           [anyadance]
Tool executable:      AnyaDance.exe
```

## Devices

```text
anyadance_hmd_001                    HMD
anyadance_left_controller_001        left controller
anyadance_right_controller_001       right controller
anyadance_hip_001                    generic tracker
anyadance_left_foot_001              generic tracker
anyadance_right_foot_001             generic tracker
```

The controllers advertise `knuckles` controller type and use Valve Index render models through `{indexcontroller}`. The input profile lives under the public `{anyadance}` resource namespace.

## T-Pose Constants

The companion tool starts and resets to a canonical test T-pose. These HMD-local position offsets are intentionally centralized and test-tunable:

```text
HMD Y:            1.50 m
Left controller:  (-0.62, -0.17, -0.20) m
Right controller: ( 0.62, -0.17,  0.20) m
Hip:              ( 0.00, -0.43, -0.05) m
Left foot:        (-0.09, -1.24,  0.10) m
Right foot:       ( 0.09, -1.24,  0.10) m
```

With a neutral HMD at `(0.00, 1.50, 0.00)`, this puts the feet at `(-0.09, 0.26, 0.10)` and `(0.09, 0.26, 0.10)`.

Reset preserves HMD X/Z and the HMD yaw only — it uprights the head, dropping pitch and roll, so the whole body faces one consistent direction. Other device positions are built from these HMD-yaw-relative offsets. Other device rotations use HMD yaw only, plus canonical local controller rotations:

```text
Left controller:  (0.0, 0.0, -0.7071067811865475, 0.7071067811865475)
Right controller: (0.0, 0.0,  0.7071067811865475, 0.7071067811865475)
```

Hip and feet use identity local rotations composed with HMD yaw.

## Virtual HMD Display

The virtual HMD reports a default `3840x1080` desktop window split into two
`1920x1080` eye viewports, preserving a `16:9` aspect ratio per eye. The
recommended render target remains `1920x1080` per eye. SteamVR settings can
override the desktop window through `headset_window_width`,
`headset_window_height`, `headset_window_eye_mode`, and
`headset_window_preserve_aspect` in the `driver_anyadance` settings section.

## Tool Mirroring

The companion tool has one mirror checkbox between the controller boxes and one between the foot boxes. When enabled, dragging either side makes the opposite side the absolute reflected pose. In HMD frame mode, the reflection uses the local YZ plane defined by the current HMD yaw and HMD position. In Global frame mode, it uses world axes with the HMD position as the center. This keeps the pair symmetric rather than copying or negating only the drag delta.

The capture panel reserves a fixed-height mouse-help area, keeping the device rows stable while switching languages. The boxes resize to fit the panel height. Left mouse drag moves non-HMD devices in local X/Y, middle mouse drag rotates, and right mouse drag moves depth. The HMD allows rotation, plus vertical (Y) movement with a left+right mouse drag, clamped to `kMaxDeviceY`.

Dragging the empty area of the panel drives the right controller thumbstick: the press point is the neutral center and the drag offset maps to the stick axes, clamped to ±1, recentering on release. It is intended for aiming the right-hand quick menu (held `M`).

## Tool Log

The companion tool keeps the UDP log in English regardless of selected UI language. Hovering any part of a log row shows the transmitted JSON. Clicking a row highlights it and opens a pinned detail window with a scrollable JSON view; clicking the highlighted row again closes the detail window. Focus changes are logged only when they release active controller input.

## Localization

Tool UI strings live in `src/tool/localization.*`. The row-per-string table owns the available language definitions by code and display name; UI rendering iterates that table, while UDP log labels intentionally use the English strings.
