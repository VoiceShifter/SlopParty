<div align="center">
  <img src="res/logo.png" alt="Logo" width="800">

  <p align="center">
    <a href="https://discord.gg/T4faGveujK">Discord</a>
  </p>
</div>

# Overview

[Build Status]: https://github.com/mariopartyrd/partyboard/actions/workflows/build.yml/badge.svg
[actions]: https://github.com/mariopartyrd/partyboard/actions/workflows/build.yml
[Discord Badge]: https://img.shields.io/discord/994839212618690590?color=%237289DA&logo=discord&logoColor=%23FFFFFF
[discord]: https://discord.gg/T4faGveujK

A work-in-progress Windows/Linux/macOS/Android/iOS port of Mario Party 4.

This repository does **not** contain any game assets or assembly whatsoever. An existing copy of the game is required.

Version Completion:

- `GMPE01_00`: Rev 0 (USA)
- `GMPE01_01`: Rev 1 (USA)

### 1. Download [Party Board](https://github.com/mariopartyrd/partyboard/releases)

### 2. Setup the game

- Extract the .zip file
- Launch partyboard or partyboard.exe depending on your platform.

## Experimental direct-IP multiplayer

Direct-IP multiplayer currently supports one host and one client. Both players must run the same Party Board revision, use matching gameplay settings and compatible save data, and provide their own copy of the same game revision.

Select **Network Play** on the startup screen. The first player selects **Host**, chooses a TCP port and input delay, and starts the host. The second player selects **Join** and enters the host's IP address and port. Use `127.0.0.1` when testing two instances on one computer. The game starts automatically after the connection succeeds.

The command-line options remain available as an alternative:

```sh
partyboard --netplay-host=46040 --netplay-delay=4
partyboard --netplay-connect=127.0.0.1:46040
```

The host controls player 1 and the client controls player 2. The default input delay is four frames. Internet hosts may need to allow and forward the selected TCP port. This initial implementation is intended for trusted peers and does not provide matchmaking, relay service, or transport encryption.

### Connection diagnostics

The console reports each peer's local Git revision, build/settings hashes, and the gameplay settings included in the hash. A **build mismatch** means different Git commits, not different disc-image checksums; rebuild both peers from the same commit. A **gameplay settings mismatch** requires matching the values on the `Hashed settings` lines, including `backend.skipBootSequence`.

The host sends build/settings rejection reasons to the client. If the host closes before sending a welcome or rejection, check the host console: that alone does not prove a hash mismatch. To save console output while using the startup GUI, run `./partyboard > netplay.log 2>&1` on Linux, or `.\partyboard.exe *> netplay.log` in Windows PowerShell.

# Building

If you'd like to build Party Board from source, please read the [build instructions](building.md).

## Common problems

### RenderDoc not working

RenderDoc has some conflict with asan. To turn off asan, you should delete the two lines in `CMakeLists.txt` that enable ASAN for the DOL and the RELs: `set_source_files_properties(..., -fsanitize=address)`

# Credits

Special thanks to the GC/Wii decompilation community, the [Aurora](https://github.com/encounter/aurora) developers, the Dusk developers, all [contributors](https://github.com/mariopartyrd/partyboard/graphs/contributors), [ImWhoreHay](https://x.com/ImWhoreHay) for the font, and justcamtro for designing the assets.

<br/>
<div align="center">
    <a href="https://github.com/encounter/aurora">
        <img src="assets/aurora-powered.png" alt="Powered by Aurora" width="800">
    </a>
</div>
