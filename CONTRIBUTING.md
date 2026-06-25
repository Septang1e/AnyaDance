# Contributing

This project is Windows-only and uses CMake plus CTest.

## Using a coding agent

Contributing with an AI coding agent (Claude Code, Codex, or similar) is
recommended. The repository ships an [AGENTS.md](AGENTS.md) that gives an agent
the project layout, build/test commands, conventions, and the squash-merge PR
workflow up front, so an agent can build, test, and open a focused PR with
little hand-holding. Point your agent at it and let it follow the same gate
described below. You are still responsible for reviewing the diff and the PR
description before submitting.

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

## Licensing of contributions

Unless you state otherwise, contributions you submit are accepted under the
project's [Apache License 2.0](LICENSE) (inbound = outbound). You retain the
copyright to your contribution; no separate contributor license agreement is
required.

By participating in this project you agree to abide by our [Code of Conduct](CODE_OF_CONDUCT.md).