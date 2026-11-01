# Installing the v0.5.35 beta on an FM-1

> **Risk:** flashing community firmware may fail or erase settings. Back up your patterns and user samples; ensure stable USB and power. This firmware reuses the three USER SAMPLE slots as a MUSIC DISK. Do not flash if you need the old samples without a backup.

1. Download `keySTEP_ONE_v0.5.35_BETA.fwsc` from this project's GitHub Release assets (not GitHub's auto-generated "Source code" archive).
2. Obtain the MIDI firmware installer from the `base/tools/fm1_install.py` source in this repository (clone recursively; see BUILDING.md), or use another **known compatible** FM-1 installer.
3. Install Python 3 dependencies `mido` and `python-rtmidi` for the script if needed.
4. Connect the FM-1 through USB MIDI and verify the device/port.
5. From the repository root, run:

```bash
python3 base/tools/fm1_install.py --info --port "keySTEP ONE"
python3 base/tools/fm1_install.py /path/to/keySTEP_ONE_v0.5.35_BETA.fwsc --port "keySTEP ONE"
```

The port name is an example; substitute your actual device name if needed. Follow the installer's confirmation and status messages. Do not disconnect during flashing. Verify the installed identity is `FM-1_731000535` afterward.

**No official support** is provided by the FM-1 manufacturer or upstream projects. Recovery behavior varies with the firmware currently on the device; ensure you understand the rescue process *before* installing.

To verify the file before flashing:

```bash
sha256sum keySTEP_ONE_v0.5.35_BETA.fwsc
```

Expected SHA-256: `0cdf16c8ceef2be151f4ce641e629ba2b96e85d9910e1f4bc083ac95c6808884`.
