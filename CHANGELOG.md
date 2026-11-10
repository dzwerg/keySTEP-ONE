# Changelog
## v0.5.42 - 2026-11-10

- ALBUM ONCE confirmed working on physical FM-1 hardware.
- Removed the temporary red PLAYER debug line.
- Preset selection and Program Change routing fixes retained.
- LIVE PLAY, MUSIC DISK and track routing preserved.
- FMHub metadata added.
- Firmware identity changed to FM-1_74000542.
- Release package rebuilt with the new identity; playback logic unchanged.
- SHA-256: 9de60f29818022dd72cd2c098b91c9d27427bd8898682cf0c30863fc5754569b


## v0.5.35-beta — 2026-11-01

First public **beta snapshot** prepared from the owner's `keySTEP_ONE_ARBEITSKOPIE.zip` (pre-built firmware identity `FM-1_731000535`).

### Included
- Four-track arranger and native pattern/song/album workflows.
- LFO **LIVE PLAY**: 11 banks × 16 patterns = 176 keyboard-selectable IDs; four independent track selections; per-track next-loop queuing and live-pattern indication.
- Four live-play track volume knobs. For the internal GM engine, channel-local CC7 can affect active notes; outbound MIDI Note-On velocities are scaled for new notes (no global CC7 message sent to external gear).
- USB MIDI Device/Host capability, BLE MIDI and TRS MIDI code paths.
- Sonivox EAS General MIDI engine and M2-related functionality.

### Verification
- LIVE PLAY confirmed working on real hardware by project owner.
- Prebuilt `.fwsc` read from the supplied archive and its identity and SHA-256 verified during packaging.
- **No fresh hardware flash or full build was performed as part of this release preparation.**

### Known problem
- **ALBUM / ONCE** has been reported to restart after the final song. The source already includes an ONCE stop check, but the issue is not resolved/reproduced conclusively. Treat this as a beta limitation.
- Do not expect a displayed LIVE PLAY mapping for pattern IDs 177–512.
