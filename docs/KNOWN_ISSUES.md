# Known issues — v0.5.35-beta

## ALBUM / ONCE: repeats instead of stopping

**Status: open.** The real FM-1 has reportedly repeated an album after the last S/M song with **ONCE** selected, though a targeted earlier local test did not reproduce it. The PLAYER (SEQ short press) and ALBUM editor have distinct loop behavior, which needs to be distinguished in future debugging.

Reproduction checklist:
1. From the PLAYER (**SEQ short**), choose **ALBUM**.
2. Select an album with more than one S or M song.
3. Set **KNOB 4 = ONCE**.
4. Start playback, let the final item finish.
5. Record whether playback stops or restarts at the first item.

In any report include firmware identity (`FM-1_731000535`), album/song IDs, whether tracks have different pattern lengths, and whether playback was started in the PLAYER or editor.

## LIVE PLAY display range

Only patterns **1–176** are mapped onto the 11 black-key banks × 16 white keys. Patterns 177–512 are not deleted or disabled; their active mapping is simply not shown by LIVE PLAY yet.

## Persistence and samples

The arranger uses **240 KiB from three user sample slots** as its music disk. Existing user samples in that area are not compatible with this storage use. Review source before clearing or formatting. A destructive erase must only happen after explicit user confirmation; do **not** assume a firmware upgrade preserves samples.

## General beta warning

This is a hobby beta. Not all USB Host, BLE pairing, TRS routing, recording, pattern save/load and cross-version update scenarios were independently verified for this distribution.
