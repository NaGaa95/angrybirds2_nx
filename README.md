<div align="center">

<img src="extras/banner.png" alt="Angry Birds 2 banner" width="35%">

</div>
<h1 align="center">Angry Birds 2 - Nintendo Switch port</h1>

This is a wrapper/port of the Android ARM64 version of Angry Birds 2 (`26.4.3`). <br/>
It loads the original game's ARM64 libraries and runs them as-is inside a minimal Android compatibility environment.

### How to install

You will need the a legally obtained copy of Angry Birds 2

Copy the contents of the generated output directory to your SD card with this layout:

```text
/switch/angrybird2_nx/
  angrybird2_nx.nro
  libmain.so
  libunity.so
  libil2cpp.so
  assets/
```

The three libraries must come from the `arm64-v8a` lib.

### Notes

This will not work in applet/album mode. Use a game override (hold R while launching a title) or a forwarder so the game receives the full application memory pool.

### How to build

You need devkitA64 plus the following devkitPro packages:

* `switch-mesa`
* `switch-libdrm_nouveau`
* `switch-sdl2`
* `switch-zlib`

From an MSYS2 environment with devkitPro mounted at `/opt/devkitpro`:

```bash
./build.sh clean
```

The resulting release build is `angrybird2_nx.nro`.

### Credits

* TheOfficialFloW, Andy Nguyen, and fgsfds for the Android shared-object loader lineage
* devkitPro and libnx contributors for the Nintendo Switch homebrew toolchain

### Support

If you enjoy my work and want to support me:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project is not affiliated with Rovio Entertainment. Angry Birds, Angry Birds 2, and all related assets are trademarks or property of their respective owners. All rights reserved.
No assets or program code from Angry Birds 2 are included in this repository. We do not condone piracy and encourage users to legally obtain the original game.
Unless specified otherwise, the source code in this repository is licensed under the MIT License. See the accompanying [LICENSE](LICENSE) file.
