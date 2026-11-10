# Known issues - v0.5.42

## ALBUM / ONCE

**Status: fixed and hardware tested.**
ALBUM ONCE stops after the final song on the developer's FM-1.
The FMHub build changes the firmware identity only.

## LIVE PLAY display range

Only patterns **1–176** are mapped onto the 11 black-key banks × 16 white keys. Patterns 177–512 are not deleted or disabled; their active mapping is simply not shown by LIVE PLAY yet.

## Persistence and samples

The arranger uses **240 KiB from three user sample slots** as its music disk. Existing user samples in that area are not compatible with this storage use. Review source before clearing or formatting. A destructive erase must only happen after explicit user confirmation; do **not** assume a firmware upgrade preserves samples.

## General beta warning

This is a hobby beta. Not all USB Host, BLE pairing, TRS routing, recording, pattern save/load and cross-version update scenarios were independently verified for this distribution.
