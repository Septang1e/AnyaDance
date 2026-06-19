# UDP Protocol

## Transport

```text
UDP
127.0.0.1:39570
UTF-8 JSON
version: 1
fire-and-forget datagrams
accepted datagram size: less than 8192 bytes
```

The driver binds to loopback only. Senders treat `sendto` success as local socket success.

## Coordinate Expectations

Positions are in metres in the driver pose coordinate space expected by SteamVR for this driver. Quaternions use XYZW order:

```json
"rotation_xyzw": [x, y, z, w]
```

## Packet Shape

```json
{
  "version": 1,
  "devices": {
    "hmd": {
      "valid": true,
      "connected": true,
      "pose": {
        "position": [0.0, 1.5, 0.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]
      }
    }
  },
  "inputs": {
    "left_controller": {
      "trigger_click": false,
      "trigger_value": 0.0,
      "menu_click": false,
      "system_click": false,
      "a_click": false,
      "b_click": false,
      "grip_click": false,
      "grip_value": 0.0,
      "joystick_x": 0.0,
      "joystick_y": 0.0,
      "trackpad_x": 0.0,
      "trackpad_y": 0.0,
      "finger_bends": {
        "thumb": 0.0,
        "index": 0.0,
        "middle": 0.0,
        "ring": 0.0,
        "pinky": 0.0
      }
    }
  }
}
```

Recognized device IDs are exactly:

```text
hmd
left_controller
right_controller
hip
left_foot
right_foot
```

The parser uses recognized device IDs and fields. Malformed recognized device entries are skipped; other valid recognized entries in the same packet can still be used.

## Required Device Fields

Each device entry requires:

```json
{
  "valid": true,
  "connected": true,
  "pose": {
    "position": [0.0, 0.0, 0.0],
    "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]
  }
}
```

## Validation

The driver rejects packets with:

- missing or wrong `version`
- malformed JSON shape
- datagram size greater than or equal to 8192 bytes
- zero valid recognized devices

A device entry is ignored if it has:

- missing required fields
- non-finite position or quaternion values
- absolute position component above `10.0 m`
- quaternion squared length outside the accepted `0.5` to `1.5` range

Accepted quaternions are normalized before use.

## Y Clamp

All devices are clamped to a maximum Y of `2.0 m`:

```cpp
position.y = std::min(position.y, 2.0f);
```

The clamp applies to Y. The companion tool clamps before serialization. The native driver clamps again after packet validation and rate-limits repeated clamp warnings.

## Pose Liveness

The driver does not time out device poses. All six virtual devices start connected
and valid at their neutral poses before the first packet. When packets arrive,
the latest valid packet for each device updates its pose and controller inputs.
If packets stop, SteamVR continues to see the device connected and valid at its
last pose.

Accepted device samples are reported as connected, valid, and
`TrackingResult_Running_OK`.

## Controller Inputs

The driver exposes Valve Index-compatible controller components:

```text
/input/trigger/click
/input/trigger/value
/input/application_menu/click
/input/a/click
/input/b/click
/input/grip/click
/input/grip/touch
/input/grip/value
/input/grip/force
/input/thumbstick/x
/input/thumbstick/y
/input/trackpad/x
/input/trackpad/y
/input/skeleton/left
/input/skeleton/right
```

Input values are clamped to their valid ranges. If `trackpad_x` or `trackpad_y` are omitted, the corresponding joystick value is used as a fallback. `finger_bends` is optional; when present, each value is clamped to `0.0` through `1.0` and drives the controller skeleton.
