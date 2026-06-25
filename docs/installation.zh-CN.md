# 安装

[English](installation.md) | **简体中文**

## 注册驱动

先构建，然后注册暂存的驱动文件夹：

```powershell
.\scripts\build_driver.ps1
.\scripts\register_driver.ps1
.\scripts\restart_steamvr.ps1
```

被注册的文件夹通常是：

```text
build\out\anyadance
```

## 全虚拟设置

`register_driver.ps1` 还会应用全虚拟设置，使虚拟头显成为活动头显：

```text
启用虚拟头显
启用控制器
启用追踪器
启用 activateMultipleDrivers
forcedDriver = anyadance
requireHmd = true
```

## 备份

`register_driver.ps1` 在写入这些设置之前，会先备份当前的 SteamVR 设置文件。默认备份路径为：

```text
%LOCALAPPDATA%\AnyaDance\steamvr.vrsettings.backup
```

首次注册会写入备份，重复注册会保留那份原始、未被修改的设置文件。`unregister_driver.ps1` 会还原该备份并随后清除它，以便下次注册重新捕获一份全新的基线。

更改注册或启动设置后需要重启 SteamVR。

## 取消注册

```powershell
.\scripts\unregister_driver.ps1
.\scripts\restart_steamvr.ps1
```
