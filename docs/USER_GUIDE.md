# keySTEP ONE — Kurzhandbuch / User Guide (v0.5.35-beta)

## LIVE PLAY (LFO-Taste)

1. **LFO** drücken → LIVE PLAY.
2. Mit **ALGO** einen der **vier Tracks** auswählen.
3. Eine der **11 schwarzen Tasten** drücken → Bank 1 bis 11.
4. Eine der **16 weißen Tasten** drücken → Pattern innerhalb der Bank vormerken.
5. Der Patternwechsel erfolgt **am nächsten Loop-Ende des betreffenden Tracks**, nicht sofort mitten im Pattern.
6. ALGO auf anderen Track drehen: Anzeige springt auf dessen **aktuell laufendes** Pattern.

**Berechnung:** `Pattern = (Bank − 1) × 16 + weiße Taste (1…16)`.

Die schwarze Banktaste allein startet nichts. Ab Pattern 177 gibt es derzeit keine Tastenanzeige, die Patterns bleiben aber vorhanden.

## LIVE PLAY mixer

KNOB 1–4 steuern die vier Track-Lautstärken (0–100%). Intern: GM-Kanal-Lautstärke über MIDI CC7. Externe MIDI-Ausgabe: Velocity-Skalierung neuer Note-On-Nachrichten; bereits klingende Noten außerhalb des Geräts werden dadurch nicht nachträglich leiser.

## MIDI

Unter SETUP kann zwischen **USB DEVICE** (PC/Mac/iPad) und **USB HOST** (externe USB-MIDI-Geräte) gewechselt werden. BLE MIDI ist grundsätzlich in beiden Modi vorgesehen. Details zu Routing und USB-Stromversorgung hängen von Gerät und Aufbau ab; Beta-Tests sind ratsam.

## PLAYER / SONG / ALBUM

Der PLAYER ist über **SEQ kurz** erreichbar; die Editor-Ansichten über **SEQ lang**. Der SONG-/ALBUM-Editor hat eine eigene Wiedergabelogik. **ALBUM ONCE** ist aktuell ein offener Fehler: am Ende eines Albums kann erneut Song 1 starten.

## Daten / MUSIC DISK

Die Firmware verwendet die drei USER SAMPLE Slots für ca. **240 KiB MUSIC DISK**. Nicht ohne Datensicherung löschen/formatieren: vorhandene Samples können verloren gehen. Die interne Sonivox-GM-Bank ist davon getrennt.

## Kurzstatus

- LIVE PLAY: Hardwaretest erfolgreich laut Projektinhaber.
- ALBUM ONCE: offen.
- Weitere Funktionen: Beta, abhängig von Hardware und MIDI-Setup.
