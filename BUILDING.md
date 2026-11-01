# Build keySTEP ONE from source

This repository contains the modified CTL-1 application sources and uses **fm1-base** and **felucca-bsp** as recursively checked-out Git submodules.

```bash
git clone --recurse-submodules https://github.com/dzwerg/keySTEP-ONE.git
cd keySTEP-ONE
# Or, for an existing checkout:
git submodule update --init --recursive
```

Requirements: Python 3 and Pillow; a C compiler for the simulator; for device builds, the **JieLi AC79 SDK** and **pi32v2 toolchain** (not bundled). Use paths relevant to your WSL/Linux machine:

```bash
export JIELI_TOOLCHAIN="$HOME/.jieli/toolchain"
export AC79_SDK="$HOME/fw-AC79_AIoT_SDK"
python3 -m pip install Pillow
./build.sh --host
./build.sh
```

The final firmware package is generated under `app/build/`. The build framework is described in the upstream [fm1-base repository](https://github.com/charlesvestal/fm1-base).

**Version reproducibility:** The v0.5.35-beta release binary comes from the supplied working-copy ZIP, not a fresh build. The submodule is pinned by the first publication commit, which may not be the exact original checkout state because the ZIP did not include the Git object database. A bit-for-bit reproducible rebuild is not claimed for this beta.

Before contributing an edited file, keep its MPL-2.0 / Apache-2.0 notices and third-party attribution. Do not commit `app/build/`, SDK extracts, toolchain copies or firmware binaries into the Git source tree; attach firmware binaries to Releases instead.
