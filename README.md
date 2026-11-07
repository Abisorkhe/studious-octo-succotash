# Direct bridge: Android ↔ Windows Stronghold multiplayer (no Steam)

Prototype that lets the **Android port** (`pc_decomp_strhld`, custom SHL7
LAN protocol in `android/host/dplay.c`) play together with the **original
Windows games** (real DirectPlay), without Steam.

## What was read

- **`RedirectPlay`** (`../RedirectPlay`): adds a *new DirectPlay Service
  Provider* ("Steamworks Connection") to a drop-in `dplay.dll`. It proxies
  `IDirectPlay4` to Microsoft's real DirectPlay by default, and only when
  the user picks the new provider does it switch transport to
  SteamNetworkingSockets (P2P relay, Steam lobbies for discovery, a Steam
  game server as host, DPID allocation + `DPSYS_*` system-message synthesis
  in `SteamPlayClient`/`SteamPlayServer`). Transport-agnostic architecture:
  the provider, not the wire protocol, is the extension point.
- **`Olde-Skuul/directplay`** (`../directplay`): the DirectX SDK (Aug 2007)
  headers/libs/samples — *not* a bridge. Used here for the authoritative
  `Include/dplay.h` (`DPSESSIONDESC2`, `DPMSG_CREATEPLAYERORGROUP`, …,
  `DPSYS_*` constants) that both RedirectPlay and this prototype build
  against. Note its readme: DirectPlay is 32-bit only — build **x86**.

## Reference repositories

This repo is only the `direct-bridge` prototype itself — the codebases
above are **not** vendored or included here. Clone them separately as
siblings of this directory (as shown) if you want to build/test against
them:

| Local path | Upstream repository | Role in this project |
|---|---|---|
| `../RedirectPlay` | [Sh0wdown/RedirectPlay](https://github.com/Sh0wdown/RedirectPlay) | Base for the Windows `dplay.dll` provider this prototype adds to |
| `../directplay` | [Olde-Skuul/directplay](https://github.com/Olde-Skuul/directplay) | DirectX SDK headers/libs/samples (`Include/dplay.h`, …) |
| `../pc_decomp_strhld` | [phys-winner/pc_decomp_strhld](https://github.com/phys-winner/pc_decomp_strhld) | Android Stronghold port; source of the SHL7 protocol (`android/host/dplay.c`) |

```bash
git clone https://github.com/Sh0wdown/RedirectPlay ../RedirectPlay
git clone https://github.com/Olde-Skuul/directplay ../directplay
git clone https://github.com/phys-winner/pc_decomp_strhld ../pc_decomp_strhld
```

## Design

Add a second provider to a RedirectPlay fork, `CShl7PlayProvider`
(`windows/`), that speaks the Android port's SHL7 protocol over Winsock:

| Aspect | Android (`dplay.c`) | Windows provider (this prototype) |
|---|---|---|
| Transport | BSD sockets, TCP + UDP | Winsock TCP + UDP (same ports) |
| Magic / header | `0x53484C37`, 5×BE u32 | identical (`Shl7Protocol.h`) |
| Ports | 47624 Classic / 47625 Extreme / 47628 HD, discovery `port+2` | same; host binds `SHL7_PORT` (default Extreme), join/enumerate query all three |
| Handshake | HELLO (32 B session GUIDs) → WELCOME (id + session + players[8]) | identical |
| Data | `NET_DATA` host-relayed, ≤1 MB | identical |
| Players | `NET_PLAYER` / `NET_PLAYER_DATA` (4364 B `lan_player`) | identical |
| Guest events | kinds 3/5/0x102/0x104/0x31 = `DPSYS_*` | synthesized as real `DPMSG_*` structs from `dplay.h` |

Payload structs are native little-endian on both sides (ARM64 Android and
x86 Windows), exactly as `dplay.c` `memcpy`s them; only the 20-byte header
and discovery magic are big-endian.

## Layout

```
direct-bridge/
  README.md                  this file
  windows/
    Shl7Protocol.h           wire spec (constants, Session/Player structs)
    Shl7PlayProvider.h/.cpp  CShl7PlayProvider (IDirectPlay4 subset) + factory
  tests/
    shl7.py                  Python implementation of the same protocol
                             (host + client, guest-message queue)
    test_interop.py          socket-level interop + spec-vs-dplay.c checks
    android_endpoint.c       real Android dplay.c as a Linux host/client
    test_android_interop.py  Python <-> Android-C interop
    linux_shim/              Winsock/Win32 shims for native Linux builds
    provider_linux_test.cpp  drives the real provider's COM methods
    test_provider_linux.py   provider (C++) <-> Android-C / Python interop
```

## Build & run (Windows)

1. Fork/clone RedirectPlay; copy `windows/Shl7PlayProvider.{h,cpp}` and
   `Shl7Protocol.h` into `RedirectPlay/ServiceProviders/Shl7/`, add them to
   `DirectPlay.vcxproj` (x86, link `ws2_32.lib`).
2. Headers: copy `dplay.h`/`dplobby.h` from `../directplay/Include/` into
   `RedirectPlay/External/DirectX/` (apply RedirectPlay's documented
   `IDirectPlayLobby3` inheritance fix in `dplobby.h`).
3. Build the DLL with MSVC (VS2022, Win32). The Steam provider can be
   dropped from the build if you don't want it.
4. Put the built `dplay.dll` next to the Stronghold/Crusader exe. In the
   game's network menu choose **"Stronghold Direct LAN"**. Set
   `SHL7_PORT` to 47624/47625/47628 if hosting a different edition than
   Extreme.
5. On Android, start the APK and host/join via its "TCP/IP LAN" provider
   (same session name / same LAN). No Steam account, no internet needed.

> The C++ provider compiles with MinGW-w64 (see Tests); a full MSVC DLL
> build + in-game test still needs a Windows machine. The protocol logic
> itself is tested below against the real Android C implementation.

## Tests

Python prototype over real sockets:

```
$ python3 tests/test_interop.py
PASS spec constants match android/host/dplay.c
PASS host<->client (discovery/join/player/data/pdata/session/big/disconnect)
PASS malformed packets rejected, host survives
ALL TESTS PASS
```

Cross-implementation against the **real Android C code** (`android/host/dplay.c`
compiled natively as `tests/android_endpoint.c`, same guest-VM stubs as
`pc_decomp_strhld/tests/dplay_selfcheck.c`):

```
$ gcc -std=gnu11 -I../../pc_decomp_strhld/android/runtime \
      -I../../pc_decomp_strhld/android/host android_endpoint.c \
      -o android_endpoint -lm -lpthread
$ python3 tests/test_android_interop.py ./android_endpoint
PASS python client <-> android C host (discovery/join/player/data/pong/disconnect)
PASS android C client <-> python host (discovery/join/player/data/pong)
ALL ANDROID INTEROP TESTS PASS
```

**Native emulation of the real provider** — the provider C++ is also
compiled for Linux via `tests/linux_shim/` (Winsock→POSIX, minimal
`windows.h`/`DirectX/dplay.h`/`COM` shims with layouts copied from
`Olde-Skuul/directplay/Include/dplay.h`), and driven through its COM
methods by `tests/provider_linux_test.cpp`:

```
$ g++ -std=c++17 -I tests/linux_shim -I windows windows/Shl7PlayProvider.cpp \
      tests/provider_linux_test.cpp -o provider_linux -pthread
$ python3 tests/test_provider_linux.py ./provider_linux ./android_endpoint
PASS windows provider (C++) host <-> python client
PASS windows provider (C++) host <-> android C client
PASS android C host <-> windows provider (C++) client
ALL PROVIDER LINUX TESTS PASS
```

This caught real provider bugs that the Python twin couldn't: the
discovery socket bound to the TCP port instead of `port+2`, the discovery
answer split into two datagrams instead of one, and `select(0, …)` which
is a no-op for nfds on POSIX (fine on Windows) — all fixed in the shared
Windows source.

Windows provider compile check (MinGW-w64 i686, headers from
`Olde-Skuul/directplay/Include` + RedirectPlay's `COM/`/`Registration.h`):

```
i686-w64-mingw32-g++ -std=c++17 -c ... Shl7PlayProvider.cpp -o provider.o
# OK (906 KB object). This check caught real bugs before: kPlayerData
# constant shadowing (same as in the Python prototype), a void/bool
# ProcessPacket mismatch, and DPCAPS field names for this dplay.h.
```

Full DLL linking is done on Windows/MSVC inside the RedirectPlay fork
(RedirectPlay's own support files use MSVC-isms that MinGW rejects);
the provider translation unit itself compiles clean on both toolchains.

- `test_spec_matches_android`: magic, ports, packet kinds, struct sizes
  (336/4364 B) and session-desc offsets (size@0, flags@4, guidInstance@8,
  guidApplication@24, maxPlayers@40, currentPlayers@44) are checked
  against `android/host/dplay.c` so the Windows build stays wire-compatible.
- `test_host_client`: UDP discovery → TCP join → player create → data
  both ways → player-data update → session-desc update → 200 KB message
  (cf. `tests/dplay_selfcheck.c`'s 60/120/180 KB cases) → disconnect
  (`DPSYS_DESTROYPLAYERORGROUP` on host, `DPSYS_SESSIONLOST` on client).
- `test_malformed`: bad magic / wrong session GUID are rejected without
  killing the host.

Not covered here (needs a Windows machine + devices): compiling the DLL,
in-game session enumeration with the real Stronghold UI, and end-to-end
Android↔Windows play. Recommended next step: build the DLL, run the
Python host (`shl7.Endpoint.host(...)`) as a stand-in Android peer, and
join it from Windows Stronghold — that isolates Windows-side bugs from
Android-side ones.

## Limitations (inherited from both codebases)

- LAN only: UDP broadcast discovery doesn't cross routers (use `DPAID_INet`
  direct-IP join, already parsed in `InitializeConnection`).
- ≤8 players, ≤1 MB per message, 4096 B player data (Android caps).
- No passwords (Android rejects them), no host migration, no groups/chat —
  same subset as RedirectPlay's Steam provider and the Android port.
- Wide-char (Unicode) DirectPlay interface only, matching Stronghold.
