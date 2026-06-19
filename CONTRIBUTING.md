# Contributing

This project is Windows-only and uses CMake plus CTest.

Before submitting changes:

```powershell
.\scripts\build_driver.ps1
ctest --test-dir build -C Release --output-on-failure
```

Guidelines:

- write commit messages and code comments in English
- keep the UDP protocol backward compatible unless the protocol version changes
- keep OpenVR-required names such as `HmdDriverFactory`, `driver.vrdrivermanifest`, `knuckles`, and OpenVR input paths intact
- do not commit generated binaries, PDB files, CMake build directories, SteamVR backups, or local settings
- update docs when behavior changes
- add focused tests for protocol, safety, stale-device, T-pose, input, manipulation, and log behavior

By participating in this project you agree to abide by our [Code of Conduct](CODE_OF_CONDUCT.md).