# TotalReaper

> REAPER ↔ RME TotalMix FX Global OSC bridge. *In Erinnerung an ASIO Direct Monitoring in Samplitude.*

A REAPER extension that turns RME audio interfaces (UFX+ family) into a
fully DAW-controlled hardware console. Mic gain, 48V phantom, pad, phase,
EQ, compressor, submix sends — all driven from REAPER tracks via TotalMix
FX 2.1's new Global OSC protocol.

**Status:** Phase 0 — research and protocol discovery. Not ready for production use.

**See also:** [Notion project page](https://www.notion.so/356e5107cf8e81dd9dd1ce449d2f315e)
for full scope, roadmap, and design decisions.

---

## What it does today (v0.1.0 — MVP skeleton)

Two REAPER actions:

- **TotalReaper: Toggle OSC Dump** — listens on UDP 7002 and prints every
  incoming OSC message from TotalMix to the REAPER console. The primary
  Phase 0 protocol-exploration tool: enable it, click around in TotalMix,
  watch the paths fly by.
- **TotalReaper: Send Test Mute Input 1** — sends `/input/1/mute` to
  TotalMix on UDP 7001. If the connection works, Input 1 will mute. Run
  again to unmute. Sanity test.

Find both actions in REAPER's Action List by typing "TotalReaper".

---

## Prerequisites

1. **TotalMix FX 2.1 Alpha 4** (or later) with **Global OSC** enabled.
   - Download: https://www.rme-audio.de/downloads/tmfx_mac_globalosc_21alpha4.zip (Mac)
   - Or: https://www.rme-audio.de/downloads/tmfx_win_globalosc_21alpha4.zip (Win)
   - In TotalMix: **Settings → OSC → Compatibility Mode → Global OSC**, then
     **Options → Enable OSC Control**.
   - Default ports: TotalMix RX 7001, TX 7002 (TotalReaper assumes these).

2. **REAPER 6 or later**.

3. **Build toolchain**:
   - macOS: Xcode Command Line Tools, CMake 3.20+
   - Windows: Visual Studio 2022 (or Build Tools), CMake 3.20+

---

## Build

```bash
git clone --recursive https://github.com/acklin83/totalreaper.git
cd totalreaper
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

If you forgot `--recursive`:

```bash
git submodule update --init --recursive
```

---

## Deploy locally

After building, copy the resulting library to REAPER's UserPlugins folder.
There's a CMake target that does this for you:

```bash
cmake --build build --target install_local
```

Or do it manually:

| Platform | Source | Destination |
|---|---|---|
| macOS | `build/reaper_totalreaper.dylib` | `~/Library/Application Support/REAPER/UserPlugins/` |
| Windows | `build/Release/reaper_totalreaper.dll` | `%APPDATA%\REAPER\UserPlugins\` |

Restart REAPER. You should see in the console (View → Show Console):

```
[TotalReaper] v0.1.0 loaded — find actions in Action List by typing 'TotalReaper'
```

---

## First run

1. Start TotalMix with Global OSC enabled (see Prerequisites).
2. In REAPER: **Actions → Show action list...**, type `TotalReaper`.
3. Run **TotalReaper: Toggle OSC Dump**. Console should show:
   ```
   [OSC] server listening on UDP 7002
   [TotalReaper] OSC dump started — interact with TotalMix to see paths
   ```
4. In TotalMix, move a fader. Console should show messages like:
   ```
   [RX] /mix/in/1/0/fader ,f -12.5
   ```
5. Run **TotalReaper: Send Test Mute Input 1**. TotalMix Input 1 should mute.
   Run again to unmute.

If step 4 shows nothing, check that TotalMix is actually sending OSC and
that no firewall is blocking UDP 7002 inbound on localhost.

---

## Project structure

```
totalreaper/
├── src/
│   ├── main.cpp                # Plugin entry, action registration
│   ├── osc/                    # Minimal OSC 1.0 (no dependencies)
│   │   ├── OscMessage.{h,cpp}
│   │   ├── OscClient.{h,cpp}
│   │   └── OscServer.{h,cpp}
│   ├── reaper/                 # SDK glue
│   │   ├── ReaperAPI.{h,cpp}
│   │   └── Console.h
│   └── actions/                # User-facing actions
│       ├── Actions.h
│       ├── DumpOscAction.cpp
│       └── TestSendAction.cpp
├── external/
│   └── reaper-sdk/             # git submodule
├── docs/
│   └── osc-paths-discovered.md
├── .github/workflows/build.yml # macOS + Windows CI
├── CMakeLists.txt
├── LICENSE                     # MIT
└── README.md
```

---

## Roadmap

See [Notion](https://www.notion.so/356e5107cf8e81dd9dd1ce449d2f315e) for the
full plan. Short version:

- **Phase 0 (now):** Protocol discovery — enumerate Global OSC paths, value ranges, latency
- **Phase 1:** MVP — track-arm-triggered routing (ADM-style), pre-gain / 48V / pad / phase per track
- **Phase 2:** Cue mixes via REAPER routing graph, integration with phones.stoersender.ch
- **Phase 3:** Channel EQ, Comp, FX bus control
- **Phase 4:** SSL UF8 / UC1 control surface bridge

---

## License

MIT — see [LICENSE](LICENSE).

Sister project: [acklin83/reaper-uf8](https://github.com/acklin83/reaper-uf8) (SSL UF8 → REAPER, also MIT).
