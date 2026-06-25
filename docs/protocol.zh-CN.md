# UDP 协议

[English](protocol.md) | **简体中文**

## 传输

```text
UDP
127.0.0.1:39570
UTF-8 JSON
version: 1
发送即完成（fire-and-forget）的数据报
被接受的数据报大小：小于 8192 字节
```

驱动仅绑定回环地址。发送端将 `sendto` 成功视为本地套接字成功。

## 坐标约定

位置以米为单位，处于该驱动向 SteamVR 提供的驱动姿态坐标空间中。四元数使用 XYZW 顺序：

```json
"rotation_xyzw": [x, y, z, w]
```

## 数据包结构

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

可识别的设备 ID 恰好为：

```text
hmd
left_controller
right_controller
hip
left_foot
right_foot
```

解析器使用可识别的设备 ID 与字段。格式错误的可识别设备条目会被跳过；同一数据包中其他有效的可识别条目仍可使用。

## 必需的设备字段

每个设备条目都需要：

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

## 校验

驱动会拒绝具有以下情况的数据包：

- 缺少或错误的 `version`
- JSON 结构格式错误
- 数据报大小大于或等于 8192 字节
- 没有任何有效的可识别设备

设备条目在以下情况会被忽略：

- 缺少必需字段
- 位置或四元数为非有限值
- 位置分量绝对值超过 `10.0 m`
- 四元数平方长度超出可接受的 `0.5` 到 `1.5` 范围

被接受的四元数在使用前会被归一化。

## Y 钳制

所有设备都被钳制到最大 Y 值 `2.0 m`：

```cpp
position.y = std::min(position.y, 2.0f);
```

钳制作用于 Y。伴随 UI 在序列化前钳制。原生驱动在数据包校验后再次钳制，并对重复的钳制警告进行限频。

## 姿态存活

驱动不会让设备姿态超时。在第一个数据包之前，所有六个虚拟设备都以中性姿态开始且保持连接、有效。当数据包到达时，每个设备最新的有效数据包会更新其姿态与控制器输入。如果数据包停止，SteamVR 仍会看到设备在其最后姿态保持连接且有效。

被接受的设备样本被报告为已连接、有效且 `TrackingResult_Running_OK`。

## 控制器输入

驱动暴露与 Valve Index 兼容的控制器组件：

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

输入值会被钳制到其有效范围。如果省略 `trackpad_x` 或 `trackpad_y`，则使用对应的摇杆值作为回退。`finger_bends` 是可选的；存在时，每个值会被钳制到 `0.0` 至 `1.0`，并驱动控制器骨骼。
