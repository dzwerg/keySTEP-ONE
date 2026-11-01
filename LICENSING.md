# Licenses and origins

The modified **CTL-1-derived application source** uses **Mozilla Public License 2.0** (MPL-2.0; see `LICENSE`). Keep upstream copyright notices and make source available when distributing modified files, as required by MPL-2.0.

- Main upstream application: [charlesvestal/fm1-ctl](https://github.com/charlesvestal/fm1-ctl), MPL-2.0.
- Platform (Git submodule): [charlesvestal/fm1-base](https://github.com/charlesvestal/fm1-base), MPL-2.0.
- Low-level platform (nested Git submodule): [hugelton/felucca-bsp](https://github.com/hugelton/felucca-bsp), MPL-2.0 according to the upstream relicensing statement.
- `app/dsp/eas/` — Sonivox EAS: Apache-2.0; keep the included LICENSE and NOTICE files.
- JieLi AC79 SDK: relevant upstream code and binary library license information is described in fm1-ctl's LICENSING.md and its SDK source. SDK caches and toolchains are **not** distributed as repository content.
- Fonts in the upstream platform are obtained via the upstream submodules, not repackaged in the release starter ZIP.

The precompiled `.fwsc` release asset was taken from the original working copy. Source for modified MPL files is included in this repository; any binary-linked third-party components remain subject to their respective licenses. **No claim of authorship over upstream components is made.**

"M-VAVE" and "FM-1" are third-party marks used for compatible hardware identification only.
