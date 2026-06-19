# Installation

## Register The Driver

Build first, then register the staged driver folder:

```powershell
.\scripts\build_driver.ps1
.\scripts\register_driver.ps1
.\scripts\restart_steamvr.ps1
```

The registered folder is normally:

```text
build\out\anyadance
```

## Fully Virtual Settings

`register_driver.ps1` also applies the fully virtual settings, so the virtual HMD is the active HMD:

```text
virtual HMD enabled
controllers enabled
trackers enabled
activateMultipleDrivers enabled
forcedDriver = anyadance
requireHmd = true
```

## Backups

`register_driver.ps1` backs up the current SteamVR settings file before writing these settings. The default backup path is:

```text
%LOCALAPPDATA%\AnyaDance\steamvr.vrsettings.backup
```

The first registration writes the backup, and repeated registration keeps that original pristine settings file. `unregister_driver.ps1` restores that backup and then clears it, so the next registration captures a fresh baseline.

Changing registration or startup settings requires restarting SteamVR.

## Unregister

```powershell
.\scripts\unregister_driver.ps1
.\scripts\restart_steamvr.ps1
```
