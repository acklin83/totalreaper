# TotalReaper

> REAPER ↔ RME TotalMix FX Global OSC bridge. *In Erinnerung an ASIO Direct Monitoring in Samplitude.*

A REAPER extension that turns RME audio interfaces (UFX+ family) into a
fully DAW-controlled hardware console. Mic gain, 48V phantom, pad, phase,
submix sends — all driven from REAPER tracks via TotalMix FX 2.1's new
Global OSC protocol.

Out of scope: TotalMix-internal effects (channel EQ, dynamics, room EQ,
FX send/return bus). Those stay in TotalMix.

**Status:** Phase 1 — MVP working. Track-to-input routing mirror, per-track
preamp controls, and track-to-track sends all flow from REAPER to TotalMix
in real time. Not yet ready for unattended production use.

---

## What it does today (v0.1.8)

### Routing mirror (the big one)

**TotalReaper: Toggle Routing Mirror** — when enabled, REAPER becomes the
source of truth for the TotalMix input matrix:

- Track volume / mute / pan / width → `/mix/in/<n>/<bus>/fader|balpan` on
  the bus the track records to. REAPER's own software monitor is muted
  while the mirror is engaged, so you only hear TotalMix's hardware path.
- Stereo input tracks emit fader/pan/width for both halves of the pair.
- Track-to-track sends → matrix routings to the destination bus, with
  per-send level/pan respected.
- Only tracks the user has actually engaged (armed, monitor-on, or with
  active sends) participate, so the mirror doesn't wipe TotalMix routings
  the user set up by hand on other channels.
- Reacts live to input reassignment, send add/remove, and stereo-link
  changes.
- Toggle state persists across REAPER restarts.

**TotalReaper: Toggle 2-Way Control** — on top of the routing mirror, this
toggle adds the reverse direction: moving a fader or balpan in TotalMix
also moves the matching REAPER track / send. First REAPER track whose
input maps to the hardware channel wins. Echo-suppression is value-based,
so the loop stays stable.

### Per-track preamp control

Five actions operate on selected tracks and address the device's input
strip via `/input/<n>/...`:

- **TotalReaper: Increase / Decrease preamp gain on selected tracks (±1 dB)**
- **TotalReaper: Toggle 48V phantom on selected tracks**
- **TotalReaper: Toggle pad on selected tracks**
- **TotalReaper: Toggle phase invert on selected tracks**
- **TotalReaper: Toggle AutoLevel on selected tracks** — TotalMix rides
  the input gain automatically. Useful for unattended gain-staging during
  sound-check: arm the channel, enable AutoLevel, let the source play,
  disable when settled.

Bind these to keyboard shortcuts (or a control surface) and you have
direct mic-pre control from inside REAPER.

### Studio actions

Global TotalMix controls and project-state recall:

- **TotalReaper: Toggle Talkback** — drives `/controlroom/talkback`. Bind
  to a footswitch via Stream Deck / SSL UF8 / keyboard shortcut.
- **TotalReaper: Toggle Auto-Talkback on Stop** — drives talkback from
  REAPER's transport state. Stop/pause opens talkback, play/record closes
  it. Toggling the action on while stopped opens talkback immediately;
  toggling off closes it.
- **TotalReaper: Save / Load TotalMix Snapshot Slot 1…8** — sixteen
  actions, one save and one load per snapshot slot. Bind each to the key
  you want for that slot.

### Diagnostics

- **TotalReaper: Toggle OSC Dump** — listens on UDP 7002 and prints every
  incoming OSC message from TotalMix to the REAPER console. The original
  Phase 0 protocol-exploration tool, kept around for debugging.

Find all actions in REAPER's Action List by typing "TotalReaper".

---

## Prerequisites

1. **TotalMix FX 2.1 Alpha 4** (or later) with **Global OSC** enabled.
   - Download: https://www.rme-audio.de/downloads/tmfx_mac_globalosc_21alpha4.zip (Mac)
   - Or: https://www.rme-audio.de/downloads/tmfx_win_globalosc_21alpha4.zip (Win)
   - In TotalMix: **Settings → OSC → Compatibility Mode → Global OSC**, then
     **Options → Enable OSC Control**.
   - Default ports: TotalMix RX 7001, TX 7002 (TotalReaper assumes these).

2. **REAPER 6 or later**.

3. **Build toolchain** (only if building from source — see Install below for pre-built binaries):
   - macOS: Xcode Command Line Tools, CMake 3.20+
   - Windows: Visual Studio 2022 (or Build Tools), CMake 3.20+

---

## Install (pre-built)

### Via ReaPack (recommended)

If you already use [ReaPack](https://reapack.com/), add the Frank Acklin
Scripts repository — TotalReaper updates then arrive automatically:

1. **Extensions → ReaPack → Import repositories…**
2. Paste: `https://github.com/acklin83/reaper-scripts/raw/main/index.xml`
3. **Extensions → ReaPack → Browse packages…** → search *TotalReaper* → install
4. Restart REAPER

### Manual download

Grab the binary for your platform from the [latest release](https://github.com/acklin83/totalreaper/releases/latest)
and drop it in REAPER's UserPlugins folder, then restart REAPER:

| Platform | Asset | Destination |
|---|---|---|
| macOS (Apple Silicon) | `reaper_totalreaper-arm64.dylib` | `~/Library/Application Support/REAPER/UserPlugins/` |
| macOS (Intel) | `reaper_totalreaper-x86_64.dylib` | `~/Library/Application Support/REAPER/UserPlugins/` |
| Windows (x64) | `reaper_totalreaper.dll` | `%APPDATA%\REAPER\UserPlugins\` |

The Windows DLL has the MSVC runtime statically linked, so you don't
need the Visual C++ Redistributable installed. The macOS dylib targets
macOS 11 (Big Sur) or later.

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
| macOS | `build/reaper_totalreaper-<arch>.dylib` | `~/Library/Application Support/REAPER/UserPlugins/` |
| Windows | `build/Release/reaper_totalreaper.dll` | `%APPDATA%\REAPER\UserPlugins\` |

Restart REAPER. You should see in the console (View → Show Console):

```
[TotalReaper] loaded — find actions in Action List by typing 'TotalReaper'
```

---

## First run

1. Start TotalMix with Global OSC enabled (see Prerequisites).
2. In REAPER: **Actions → Show action list...**, type `TotalReaper`.
3. Run **TotalReaper: Toggle Routing Mirror**. Move a track fader and the
   corresponding TotalMix input fader should follow.
4. Select a track recording from a mic input and run
   **TotalReaper: Toggle 48V phantom on selected tracks** — the device
   should click and the strip's 48V indicator should light.
5. If something looks wrong, run **TotalReaper: Toggle OSC Dump** and
   watch the console as you click around in TotalMix. Compare paths
   against `docs/osc-paths-discovered.md`.

If nothing happens, check that TotalMix is actually emitting OSC and that
no firewall is blocking UDP 7001/7002 on localhost.

---

## Project structure

```
totalreaper/
├── src/
│   ├── main.cpp                    # Plugin entry, action + csurf registration
│   ├── osc/                        # Minimal OSC 1.0, no dependencies
│   │   ├── OscMessage.{h,cpp}      # Wire format
│   │   ├── OscClient.{h,cpp}       # UDP TX
│   │   ├── OscServer.{h,cpp}       # UDP RX, bundle dispatch
│   │   └── TotalMixState.{h,cpp}   # Cache of last-seen TotalMix values
│   ├── reaper/
│   │   ├── ReaperAPI.{h,cpp}       # SDK function-pointer glue
│   │   ├── ChannelMap.{h,cpp}      # REAPER input slot → device hw index
│   │   └── Console.h
│   ├── csurf/
│   │   └── TotalReaperCSurf.{h,cpp}# Routing mirror (REAPER → TotalMix)
│   └── actions/
│       ├── Actions.h
│       ├── DumpOscAction.cpp
│       ├── RoutingMirrorAction.cpp
│       ├── TwoWayControlAction.cpp
│       ├── AutoTalkbackAction.cpp
│       ├── PreampActions.cpp
│       └── GlobalActions.cpp
├── external/
│   └── reaper-sdk/                 # git submodule
├── docs/
│   └── osc-paths-discovered.md     # Living protocol reference
├── .github/workflows/
│   ├── build.yml                   # macOS + Windows CI
│   └── release.yml                 # Publish on v* tag push
├── CMakeLists.txt
├── LICENSE                         # MIT
└── README.md
```

---

## Roadmap

- **Phase 0 — done:** Protocol discovery. Global OSC vocabulary mapped in
  `docs/osc-paths-discovered.md`.
- **Phase 1 — current:** MVP. Routing mirror via control surface; per-track
  preamp (gain / 48V / pad / phase); track-to-track sends mirrored to the
  TotalMix matrix.

---

## License

MIT — see [LICENSE](LICENSE).
