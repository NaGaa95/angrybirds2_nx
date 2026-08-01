<div align="center">

<img src="extras/banner.png" alt="Angry Birds 2 banner" width="25%">

</div>
<h1 align="center">Angry Birds 2 - Nintendo Switch port</h1>

This is a wrapper/port of the Android ARM64 version of Angry Birds 2 (`26.4.3`, version code `50297008`). <br/>
It loads the original game's ARM64 libraries and runs them as-is inside a minimal Android compatibility environment.

### How to install

You will need the following files from a legally obtained copy of Angry Birds 2:

* the base APK for package `com.rovio.baba`
* `UnityDataAssetPack.apk`
* the `arm64-v8a` configuration APK

Only version `26.4.3` (Unity `6000.3.6f1`) is supported. Other versions are not compatible with the fixed Unity and IL2CPP offsets used by the wrapper.

The included staging utility verifies the exact `26.4.3` package fingerprints, extracts the APKs, overlays their assets, merges the UnityFS archives, and validates the required boot files. It rejects armv7, older releases, and mixed split packages before writing the output directory:

```powershell
python tools/stage_sd.py `
  --base C:\path\to\com.rovio.baba.apk `
  --assetpack C:\path\to\UnityDataAssetPack.apk `
  --arch C:\path\to\config.arm64_v8a.apk `
  --nro .\angrybird2_nx.nro `
  --out C:\path\to\SD_angrybird2_nx
```

The Python `lz4` package is required by the UnityFS merge utility.

Copy the contents of the generated output directory to your SD card with this layout:

```text
/switch/angrybird2_nx/
  angrybird2_nx.nro
  libmain.so
  libunity.so
  libil2cpp.so
  assets/
```

The three libraries must come from the `arm64-v8a` APK.

### Notes

This will not work in applet/album mode. Use a game override (hold R while launching a title) or a forwarder so the game receives the full application memory pool.

The port supports:

* touchscreen input in handheld mode
* left stick to move the cursor, A to select, and B to go back
* FMOD audio output
* persistent local saves and PlayerPrefs
* Switch system-language detection
* temporary 1785 MHz CPU boost during the initial loading screen, restored to normal before gameplay
* offline game progression and local gem purchases without real-money transactions

Online services are unavailable, including cloud saves, advertisements, social features, live events, notifications, and Google Play services. Android video and camera APIs are also unsupported.

A `config.txt` file is created next to the NRO on first run:

* `screen_width` / `screen_height` - output resolution; `-1` uses the default
* `language` - language index; `0` follows the Switch system language
* `portrait` - display rotation; keep the default value of `0` for landscape

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
