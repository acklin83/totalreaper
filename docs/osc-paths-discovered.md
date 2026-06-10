# Global OSC paths — discovered

Living document. Add entries as we find them via the OSC dump action.

Source: TotalMix FX 2.1 Alpha 4, Global OSC mode, UFX+ in primary slot.

## Architecture — three sections, two namespaces each

TotalMix's OSC vocabulary follows a consistent shape across the three
input/playback/output sections:

| Section | Strip namespace | Matrix namespace |
|---|---|---|
| Hardware inputs | `/input/<n>/...` | `/mix/in/<n>/<bus>/...` |
| Software playback | `/playback/<n>/...` | `/mix/pb/<n>/<bus>/...` |
| Hardware outputs | `/output/<n>/...` | — (outputs ARE the bus) |

Plus a flat **Control Room** namespace `/controlroom/...` for global
flags (mono, dim, talkback, etc.).

### Two namespaces, same channel index (provisional, for inputs)

Both appear to use the **same 0-based absolute hardware channel index**.
Verified data points on UFX+:

- Analog 1 mute → `/input/0/mute`
- MADI 9 strip controls → `/input/38/...` and `/mix/in/38/<bus>/...`

UFX+ orders inputs as Analog (12) + AES (2) + SPDIF (2) + ADAT (?) + MADI,
so MADI 9 landing at index 38 implies ~30 channels precede MADI in the
matrix. Need to dump-click the full strip range to confirm the exact
boundary positions.

### Preamp controls on digital channels — AUX device remoting

The dump for MADI 9 (`n=38`) showed `/input/38/48v`, `/input/38/pad`,
`/input/38/gain` fire. MADI itself is digital, but **TotalMix exposes
preamp controls on MADI strips when an upstream RME device (Octamic XTC,
M-32 Pro AD, etc.) is connected over MADI**. TotalMix tunnels the
preamp commands to the upstream device via MIDI-over-MADI.

Confirmed by user: 48v / pad / gain are visible in the TotalMix UI on
the MADI strip in question, because there is an upstream RME preamp
device feeding it.

**Consequence for the Phase 1 model**: we can't assume "input section =
digital → no preamp controls". The model has to allow preamp state on
*any* input strip the device exposes it on, and the UI mapping has to
come from the device profile (UFX+ + Octamic XTC over MADI is a
different shape than UFX+ alone).

## Confirmed paths

### Channel-strip controls — `/input/<n>/...`

Verified for MADI 9 on UFX+ (n=38). Note: 48v / pad / gain were emitted
even though MADI is digital — see hypotheses above.

| Path | Type | Range / Values | Notes |
|---|---|---|---|
| `/input/<n>/mute` | float | 0.0 / 1.0 | Per-strip mute |
| `/input/<n>/48v` | float | 0.0 / 1.0 | Phantom power (mic inputs) |
| `/input/<n>/pad` | float | 0.0 / 1.0 | Pad attenuator |
| `/input/<n>/phase` | float | 0.0 / 1.0 | Phase invert |
| `/input/<n>/gain` | float | integer dB, observed 13–33 | Preamp gain. Stepwise in 1 dB increments — float type but integer values |
| `/input/<n>/stereo` | float | 0.0 / 1.0 | Stereo link toggle. When set to 1, partner channel `<n+1>/stereo` fires too on un-linking |
| `/input/<n>/width` | float | 0.0 … 1.0 | Stereo width of a linked pair (1.0 = full stereo, 0.0 = mono). Confirmed by dump 2026-06-10, addressed on the pair's left channel `<n>`. Maps to REAPER `D_WIDTH` (clamped 0…1; REAPER's negative/swap range has no TotalMix equivalent) |

**Indexing for `/input/`**: 0-based absolute hardware channel index. Same
`<n>` as the matrix namespace below.

### Mix matrix — `/mix/in/<n>/<bus>/...`

Verified for MADI 9 on UFX+ (n=38):

| Path | Type | Range / Values | Notes |
|---|---|---|---|
| `/mix/in/<n>/<bus>/fader` | float | dB, observed -300 to +6 | Send level from input n to output bus. `-300` = -∞ (fully muted at fader) |
| `/mix/in/<n>/<bus>/balpan` | float | -1.0 to +1.0 | Pan/balance into bus. -1 = hard left, 0 = center, +1 = hard right |
| `/mix/in/<n>/<bus>/solo` | float | 0.0 / 1.0 | Per-routing solo |
| `/mix/in/<n>/<bus>/groupflags` | float | observed 0 | Group/link flag — semantics TBD |

**Indexing for `/mix/in/`**: 0-based absolute hardware channel position.
On UFX+, MADI 9 = `n=38`. Need to map `n` → UI label across all input
sections per device.

**Bus indexing**: observed `<bus>` values 0, 2, 4, 6, 8, 10 — even-numbered
only. Buses appear to be indexed by the **left channel of each stereo pair**
(bus pair 0/1 → bus index 0, bus pair 2/3 → bus index 2, etc.).

### Output bus controls — `/output/<n>/...`

Verified for AN 1/2 (n=0) and PH 9/10 (n=8) on UFX+:

| Path | Type | Range / Values | Notes |
|---|---|---|---|
| `/output/<n>/volume` | float | dB, observed -300 to 0 | Output bus master level. -300 = -∞. No positive gain seen (likely capped at 0). |
| `/output/<n>/mute` | float | 0.0 / 1.0 | Output bus mute |
| `/output/<n>/balpan` | float | -1.0 to +1.0 | Output bus pan/balance |

Output index `<n>` follows the same stereo-pair-by-left-channel scheme:
AN 1/2 = 0, AN 3/4 = 2, …, AN 7/8 = 6, AN 9/10 = 8, AN 11/12 = 10.

### Control Room — `/controlroom/...`

The Control Room namespace contains **status flags only** — toggles for
features that affect the main output. The actual main-bus volume is
`/output/0/volume` (or whichever bus is configured as Main); there is
no separate `/controlroom/mainvolume` path.

| Path | Type | Range / Values | Notes |
|---|---|---|---|
| `/controlroom/mainmono` | float | 0.0 / 1.0 | Mono sum on main |
| `/controlroom/dim` | float | 0.0 / 1.0 | Dim main output by configured amount |
| `/controlroom/talkback` | float | 0.0 / 1.0 | Talkback active |
| `/controlroom/speakerb` | float | 0.0 / 1.0 | Speaker B switch (alternate monitors) |
| `/controlroom/cuechan` | float | -1, or output bus index | -1 = no cue; otherwise the bus currently in cue (e.g. 8 = cue PH 9/10) |

Not yet seen but expected: listenback, recall, talkback-dim level,
external input. Map when relevant.

### Playback strips — `/playback/<n>/...` and `/mix/pb/<n>/<bus>/...`

Playback channels (software returns from REAPER) use the same shape as
inputs but a different namespace prefix:

| Path | Type | Range / Values | Notes |
|---|---|---|---|
| `/playback/<n>/mute` | float | 0.0 / 1.0 | Strip mute |
| `/mix/pb/<n>/<bus>/fader` | float | dB, observed -300 to 0 | Send level from playback to bus |
| `/mix/pb/<n>/<bus>/solo` | float | 0.0 / 1.0 | Per-routing solo |
| `/mix/pb/<n>/<bus>/groupflags` | float | observed 0 | Group/link flag |

Playback strips appear to lack channel-strip-level controls beyond mute
(no preamp obviously, but also no `/playback/<n>/balpan` seen — pan is
matrix-level only via `/mix/pb/<n>/<bus>/balpan`).

The `<n>` index for playback is independent of the input index space.

## Behavioral observations

- **Submix mode broadcasts**: when TotalMix is in Submix mode, moving a
  fader on an input emits a `fader` message **for every routed bus
  simultaneously**. We saw `/mix/in/38/0/fader` and `/mix/in/38/6/fader`
  fire as a pair on every fader move — same value, different bus index.
  This is how TotalMix's "Submix mode" preserves relative levels across
  destinations.

- **Stereo link side-effects**: toggling `/input/<n>/stereo` 0→1 emits
  fader/balpan messages for additional buses (we saw buses 2, 4, 8, 10
  appear when linking). Toggling 1→0 emits `stereo` for both `<n>` and
  `<n+1>`, and resets balpan for both halves to ±1 (hard L / hard R) on
  every routed bus.

- **Output linking / Control Room groups**: when two output buses are
  linked, level changes on one emit `/output/<a>/volume` and
  `/output/<b>/volume` simultaneously, with a **fixed offset preserved
  across the move**. Observed AN 1/2 (n=0) and AN 7/8 (n=6) firing with
  constant +1.2 dB offset on every fader move. Phase 1 needs to surface
  these groups as first-class objects, not just observe the synchronized
  emissions.

- **All packets bundled**: every TotalMix → host packet is wrapped in an
  OSC `#bundle`. OscServer parses them recursively in `dispatchPacket()`.

## To verify in Phase 0

- ~~Width control on stereo channels~~ — CONFIRMED `/input/<n>/width` (strip-level, not matrix). See table above.
- Submix-mode toggle — global flag or per-bus?
- DURec status and time messages (Alpha 4)
- Snapshot recall / save
- Control Room: listenback, recall, dim-level, talkback-dim level
- Heartbeat / `/status/*` — now that bundles parse, these should appear

## Open questions

- **Index mapping for UFX+**: need to enumerate `<n>` → UI label mapping
  for the full UFX+ input matrix (analog + AES + SPDIF + ADAT/MADI).
  Strategy: dump-click each input strip's mute in order, record the `n`.
- **`groupflags` semantics**: only seen value 0. What does non-zero mean?
  EQ-link? Routing-link? Group membership?
- **Output positive gain**: outputs only seen between -300 and 0 dB.
  Confirm whether outputs cap at 0 (TotalMix behaviour) or can go higher.
- **Output stereo-pair index `n=6`** in Frank's setup: AN 7/8, linked to
  Main with +1.2 dB offset. Likely a second monitor output (Phones 2 or
  studio monitors B). Document as part of the device profile.

## Out of scope

TotalMix-internal effects (channel EQ, dynamics, room EQ, FX send/return)
are explicitly out of scope (commit 0083366) and are not catalogued here.
