# keySTEP ONE 🎹

**Alternative controller, MIDI router and four-track pattern/song arranger firmware for the M-VAVE FM-1.**

> **Beta v0.5.35** — community project, **not** an official M-VAVE release. Flash at your own risk. Back up your music and settings first.
>
> **Known issue:** ALBUM / ONCE may repeat instead of stopping after the final song. See [Known issues](docs/KNOWN_ISSUES.md).

Built on [CTL-1](https://github.com/charlesvestal/fm1-ctl) by Charles Vestal and [fm1-base](https://github.com/charlesvestal/fm1-base) / [felucca-bsp](https://github.com/hugelton/felucca-bsp). This repository includes the modified application sources under MPL-2.0; the platform is obtained through Git submodules. See [CREDITS](CREDITS.md) and [LICENSING](LICENSING.md).

## Highlights

| Area | Beta features |
|---|---|
| Arranger | Four independently controlled tracks, PATTERN / SONG / ALBUM playback |
| Patterns | Up to **512** IDs, stored in a dedicated music area |
| LIVE PLAY (LFO button) | Direct performance selection of **176** patterns, quantized to the next selected-track pattern boundary |
| LIVE PLAY mixer | KNOB 1–4 set track volumes; internal GM uses CC7, outbound MIDI scales new Note-On velocities |
| MIDI | USB MIDI Device / Host selection, BLE MIDI, TRS MIDI integration |
| Sound | On-device Sonivox General MIDI engine; velocity/pressure capable controller |
| Editing | Pattern, song and album editing; native M2 song support |

## LIVE PLAY: keyboard as a pattern selector

Press **LFO** to enter LIVE PLAY. **ALGO** chooses track 1–4. The **11 black keys** choose a bank (1–11); the **16 white keys** select one of its 16 patterns.

`Pattern ID = (bank - 1) × 16 + white key position`

Examples: black key 1 → white keys 1–16 = patterns 1–16; black key 2 → 17–32; black key 11 → 161–176.

The black key alone **never** changes the playing pattern. Press a white key to queue the pattern for the **next loop end of the selected track**. When ALGO changes tracks, the display follows that track's currently playing pattern. Patterns **177–512** remain usable elsewhere, but currently have no LIVE PLAY keyboard indication. LIVE PLAY operation was reported working on actual hardware by the developer.

## Downloads and installation

Download **`keySTEP_ONE_v0.5.35_BETA.fwsc`** from the [GitHub Releases](../../releases) page. Do not flash a source ZIP.

Read [Flashing instructions](docs/FLASHING.md) and the [Beta release notes](CHANGELOG.md) before installing.

## Developer documentation

- [User guide / Bedienung](docs/USER_GUIDE.md)
- [Building from source](BUILDING.md)
- [Known issues and testing](docs/KNOWN_ISSUES.md)
- [Credits](CREDITS.md) and [licenses](LICENSING.md)
- [Changes](CHANGELOG.md)

The upstream CTL-1 manual remains in [docs/UPSTREAM_CTL1_MANUAL.md](docs/UPSTREAM_CTL1_MANUAL.md) **for historical reference only**. Its controls are not a guide to keySTEP ONE v0.5.35.

## Deutsch

**keySTEP ONE** ist eine alternative FM-1-Firmware mit vier Arranger-Tracks, LIVE PLAY, USB-/BLE-MIDI und internem GM-Synthesizer. Die LFO-Taste öffnet LIVE PLAY: 11 schwarze Banktasten und 16 weiße Auswahltasten ergeben 176 direkt spielbare Patterns. Der Wechsel wird zum nächsten Loop-Ende vorgemerkt. Diese Beta ist nicht vollständig getestet. Besonders **ALBUM ONCE** kann derzeit unerwartet wiederholen. [Anleitung](docs/USER_GUIDE.md) · [Flashen](docs/FLASHING.md).

This is an independent hobby project; M-VAVE and FM-1 are trademarks of their respective owners.
