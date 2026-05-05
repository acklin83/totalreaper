# Global OSC paths — discovered

Living document. Add entries as we find them via the OSC dump action.

Source: TotalMix FX 2.1 Alpha 4, Global OSC mode, UFX+ in primary slot.

## Confirmed

### Inputs

| Path | Type | Range / Values | Notes |
|---|---|---|---|
| `/input/<n>/mute` | float | 0.0 / 1.0 | Confirmed by RME admin in forum thread 43075 |

### Mix matrix

| Path | Type | Range / Values | Notes |
|---|---|---|---|
| `/mix/in/<n>/<submix>/fader` | float | dB | `<submix>` 0 = AN1/2 output. Float in dB, e.g. -37.0 |

### Phantom power (legacy paths — verify under Global OSC)

| Path | Type | Range / Values | Notes |
|---|---|---|---|
| `/1/phantom/<row>/<channel>` | float | 0.0 / 1.0 | Babyface dump from forum 39099, may differ on Global OSC |

## To verify in Phase 0

- Mic gain — path format unknown. Probably `/input/<n>/gain` but TBD.
- Pad / Hi-Z / Phase invert per channel
- AUX device pathways (Octamic XTC over MADI / MIDI-over-MADI bridge)
- Channel EQ (3-band PEQ + LowCut)
- Channel compressor / expander
- FX bus (reverb / delay) sends and parameters
- DURec status and time messages (added in Alpha 4)

## Open questions

- **Heartbeat:** `/status/*` arrives in bundles — current OSC server
  silently skips bundles. Need to add bundle parsing to monitor connection
  health. See forum thread 43075, posts 15–18.
- **Reverb/FX preset recall** — feature request 30.04.2026 (forum post),
  not yet implemented in Alpha 4.
