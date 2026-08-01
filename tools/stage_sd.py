#!/usr/bin/env python3
"""stage_sd.py -- assemble the Switch SD-card layout for angrybird2_nx.

ANGRY BIRDS 2 (com.rovio.baba 26.4.3, Unity 6000.3.6f1, IL2CPP) ships as a split
APK bundle:

  base APK  (com.rovio.baba.apk)      arm-independent Unity data:
      assets/bin/Data/data.unity3d              (main serialized bundle, ~12MB)
      assets/bin/Data/Managed/Metadata/global-metadata.dat   (il2cpp metadata)
      assets/bin/Data/Resources/, boot.config, *.json, unity_app_guid
  UnityDataAssetPack.apk  (Play Asset Delivery)  more Unity data:
      assets/bin/Data/datapack.unity3d          (~218MB)
      assets/bin/Data/resources.resource        (~39MB)
      assets/aa/ (Addressables), assets/StreamingAssets, propbundle_*.unity3d, ...
  config.arm64_v8a.apk    arm64 native code:
      lib/arm64-v8a/{libmain,libunity,libil2cpp}.so

On Android the base + asset-pack asset trees are merged into one asset path.  The
native Play Asset Delivery mount is not reached by this port, so merely overlaying
the files is insufficient: this script also merges datapack.unity3d into the base
data.unity3d UnityFS archive.  There is NO OBB and NO on-device extraction.

Result layout (copy to sdmc:/switch/angrybird2_nx/ on the card):

  <out>/
    angrybird2_nx.nro
    libmain.so  libunity.so  libil2cpp.so          (arm64-v8a)
    assets/bin/Data/data.unity3d (base + datapack merged), resources.resource,
                   Managed/Metadata/global-metadata.dat, boot.config, ...
    assets/aa/..., assets/StreamingAssets, assets/propbundle_*.unity3d, ...

Usage:
  python stage_sd.py --base com.rovio.baba.apk \
                     --assetpack UnityDataAssetPack.apk \
                     --arch config.arm64_v8a.apk \
                     --nro ../angrybird2_nx.nro \
                     --out <dir>

  --arch is the arm64-v8a split (NOT config.armeabi_v7a.apk -- that is 32-bit and
  cannot run on Switch; the loader is arm64-only). If you omit --arch the assets are
  staged and the missing libs are reported so you can drop them in later.

Accepts either .apk files (extracted with the stdlib zipfile) or already-extracted
directories (pass the dir; it must contain assets/ resp. lib/arm64-v8a/).
"""
import argparse, hashlib, os, sys, zipfile, shutil

LIBS = ["libmain.so", "libunity.so", "libil2cpp.so"]
PKG, VERSION_CODE, VERSION_NAME = "com.rovio.baba", 50297008, "26.4.3"

# These files define the ABI, metadata, and merged Unity data layout assumed by
# the native patches. Refuse mixed or unsupported package revisions up front.
EXPECTED_FINGERPRINTS = {
    "base": {
        "assets/bin/Data/Managed/Metadata/global-metadata.dat":
            "446e9a0d17504c7b011f9003c232fed6e9947c8f3588cb418a0487847dc9cb88",
        "assets/bin/Data/data.unity3d":
            "772834f07153bfa2af7d82434491a8550590eda8ffed2055bc90a79fb8b16d01",
    },
    "asset pack": {
        "assets/bin/Data/datapack.unity3d":
            "23a6d5c14e64dd89b9a7d1f8c45ff42479b9e2b4be6f30f65ceba4c19229fd55",
        "assets/bin/Data/resources.resource":
            "5604d8a85bedcd7604f6285630087e27eb64a3cd6b0d4eb85d9d1906ae5076f8",
    },
    "arm64 split": {
        "lib/arm64-v8a/libmain.so":
            "d1472b24e12cf04d86af157637ee84e93d17a267561e8005a4626be35b2a0c8e",
        "lib/arm64-v8a/libunity.so":
            "4d6397b08d63405f60cd13793c27d617dd4281001469522367282b95fbf29efc",
        "lib/arm64-v8a/libil2cpp.so":
            "0d626301991fdebbd6ffa2c5379eb96c12ef2925f2a68cf1c840ef55bbc90494",
    },
}


def _sha256_stream(src):
    digest = hashlib.sha256()
    for chunk in iter(lambda: src.read(1024 * 1024), b""):
        digest.update(chunk)
    return digest.hexdigest()


def _source_member_sha256(src, member):
    if os.path.isdir(src):
        path = os.path.join(src, *member.split("/"))
        with open(path, "rb") as stream:
            return _sha256_stream(stream)
    with zipfile.ZipFile(src) as archive:
        with archive.open(member) as stream:
            return _sha256_stream(stream)


def validate_source(label, src):
    if not src:
        return
    if not os.path.exists(src):
        raise RuntimeError(f"{label} does not exist: {src}")
    for member, expected in EXPECTED_FINGERPRINTS[label].items():
        try:
            actual = _source_member_sha256(src, member)
        except (FileNotFoundError, KeyError, zipfile.BadZipFile) as exc:
            raise RuntimeError(f"{label} is missing {member}: {src}") from exc
        if actual != expected:
            raise RuntimeError(
                f"unsupported or mixed {label}: {member}\n"
                f"  expected 26.4.3: {expected}\n"
                f"  actual:          {actual}\n"
                f"  source:          {src}"
            )


def validate_inputs(base, assetpack, arch):
    validate_source("base", base)
    validate_source("asset pack", assetpack)
    validate_source("arm64 split", arch)


def _extract_prefix(apk, prefix, out_root):
    """Extract every zip member under `prefix` into out_root. Returns bytes written."""
    total = 0
    with zipfile.ZipFile(apk) as z:
        for info in z.infolist():
            if info.is_dir() or not info.filename.startswith(prefix):
                continue
            dst = os.path.join(out_root, info.filename.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with z.open(info) as src, open(dst, "wb") as f:
                shutil.copyfileobj(src, f)
            total += info.file_size
    return total


def _copy_tree_prefix(srcdir, prefix, out_root):
    total = 0
    base = os.path.join(srcdir, prefix.replace("/", os.sep))
    for dp, _, fs in os.walk(base):
        for fn in fs:
            sp = os.path.join(dp, fn)
            rel = os.path.relpath(sp, srcdir)
            dst = os.path.join(out_root, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(sp, dst)
            total += os.path.getsize(sp)
    return total


def stage_assets(src, out):
    """Overlay the assets/ tree from an APK file or extracted dir into <out>."""
    if os.path.isdir(src):
        return _copy_tree_prefix(src, "assets/", out)
    return _extract_prefix(src, "assets/", out)


def stage_libs(arch_src, out):
    if not arch_src:
        return list(LIBS)
    missing = []
    if os.path.isdir(arch_src):
        libdir = os.path.join(arch_src, "lib", "arm64-v8a")
        for lib in LIBS:
            sp = os.path.join(libdir, lib)
            if os.path.isfile(sp):
                shutil.copy2(sp, os.path.join(out, lib))
                print(f"  lib   {lib}  ({os.path.getsize(sp):,} B)")
            else:
                missing.append(lib)
    else:
        with zipfile.ZipFile(arch_src) as z:
            names = set(z.namelist())
            for lib in LIBS:
                member = f"lib/arm64-v8a/{lib}"
                if member in names:
                    with z.open(member) as s, open(os.path.join(out, lib), "wb") as f:
                        shutil.copyfileobj(s, f)
                    print(f"  lib   {lib}  ({z.getinfo(member).file_size:,} B)")
                else:
                    missing.append(lib)
    return missing


def merge_data_archives(out):
    """Atomically replace data.unity3d with base+datapack UnityFS content."""
    data_dir = os.path.join(out, "assets", "bin", "Data")
    base_path = os.path.join(data_dir, "data.unity3d")
    pack_path = os.path.join(data_dir, "datapack.unity3d")
    tmp_path = base_path + ".merge-tmp"

    if not os.path.isfile(base_path):
        raise RuntimeError(f"base UnityFS archive is missing: {base_path}")
    if not os.path.isfile(pack_path):
        raise RuntimeError(
            f"asset-pack UnityFS archive is missing: {pack_path}\n"
            "The asset-pack archive must be merged before this port can load all Resources."
        )

    # MSYS Python treats a PowerShell-supplied backslash script path as a literal
    # POSIX filename, so explicitly make this tool's directory importable.
    tool_dir = os.path.dirname(os.path.abspath(__file__.replace("\\", os.sep)))
    if tool_dir not in sys.path:
        sys.path.insert(0, tool_dir)
    try:
        from merge_unity3d import parse, write_merged
    except ModuleNotFoundError as exc:
        if exc.name == "lz4":
            raise RuntimeError(
                "Python package 'lz4' is required for the UnityFS merge. "
                "Install it with: python -m pip install lz4"
            ) from exc
        raise

    try:
        base = parse(base_path)
        pack = parse(pack_path)
        expected_nodes = len(base["nodes"]) + len(pack["nodes"])
        expected_blocks = len(base["blocks"]) + len(pack["blocks"])
        expected_uncompressed = base["total_u"] + pack["total_u"]

        info = write_merged([base, pack], tmp_path, compress_bi=True)
        check = parse(tmp_path)
        if (len(check["nodes"]) != expected_nodes or
                len(check["blocks"]) != expected_blocks or
                check["total_u"] != expected_uncompressed):
            raise RuntimeError(
                "merged UnityFS validation failed: "
                f"nodes={len(check['nodes'])}/{expected_nodes}, "
                f"blocks={len(check['blocks'])}/{expected_blocks}, "
                f"uncompressed={check['total_u']}/{expected_uncompressed}"
            )

        os.replace(tmp_path, base_path)
        os.remove(pack_path)
        return info
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as src:
        for chunk in iter(lambda: src.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="com.rovio.baba.apk (or extracted dir)")
    ap.add_argument("--assetpack", required=True, help="UnityDataAssetPack.apk (or extracted dir)")
    ap.add_argument("--arch", help="config.arm64_v8a.apk (or extracted dir). arm64-v8a ONLY.")
    ap.add_argument("--nro", help="NRO to copy into the staged game folder (defaults to ../angrybird2_nx.nro when present)")
    ap.add_argument("--out", required=True, help="staging output dir")
    a = ap.parse_args()

    print("[0/5] validate Angry Birds 2 26.4.3 package fingerprints")
    try:
        validate_inputs(a.base, a.assetpack, a.arch)
    except Exception as exc:
        sys.exit(f"\nStaging refused before writing output: {exc}")
    print("  supported package revision confirmed")

    os.makedirs(a.out, exist_ok=True)

    print("[1/5] arm64 native libs")
    missing = stage_libs(a.arch, a.out)
    if not a.arch:
        print("  (no --arch given: skipping libs)")

    print("[2/5] base APK assets")
    b = stage_assets(a.base, a.out)
    print(f"  merged base assets ({b:,} B)")
    print("[3/5] UnityDataAssetPack assets")
    p = stage_assets(a.assetpack, a.out)
    print(f"  merged asset-pack assets ({p:,} B)")

    print("[4/5] merge base + asset-pack UnityFS archives")
    try:
        merged = merge_data_archives(a.out)
    except Exception as exc:
        sys.exit(f"\nStaging failed during UnityFS merge: {exc}")
    print(f"  data.unity3d ({merged['size']:,} B, {merged['nnodes']:,} nodes, "
          f"{merged['nblocks']:,} blocks); removed standalone datapack.unity3d")

    print("[5/5] wrapper NRO")
    default_nro = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "angrybird2_nx.nro"))
    nro_src = a.nro or (default_nro if os.path.isfile(default_nro) else None)
    if nro_src:
        if not os.path.isfile(nro_src):
            sys.exit(f"\nStaging failed: NRO does not exist: {nro_src}")
        nro_dst = os.path.join(a.out, "angrybird2_nx.nro")
        shutil.copy2(nro_src, nro_dst)
        print(f"  NRO   {os.path.getsize(nro_dst):,} B  sha256={sha256_file(nro_dst)}")
    else:
        print("  MISSING angrybird2_nx.nro (build first or pass --nro)")

    # boot-critical sanity
    data = os.path.join(a.out, "assets", "bin", "Data")
    checks = {
        "data.unity3d":        os.path.join(data, "data.unity3d"),
        "resources.resource":  os.path.join(data, "resources.resource"),
        "boot.config":         os.path.join(data, "boot.config"),
        "global-metadata.dat": os.path.join(data, "Managed", "Metadata", "global-metadata.dat"),
    }
    print("\nboot-critical data files:")
    data_ok = True
    for name, pth in checks.items():
        good = os.path.isfile(pth)
        data_ok = data_ok and good
        print(f"  {'OK ' if good else 'MISSING'} {name}")
    for lib in LIBS:
        good = os.path.isfile(os.path.join(a.out, lib))
        print(f"  {'OK ' if good else 'MISSING'} {lib}")
    nro_ok = os.path.isfile(os.path.join(a.out, "angrybird2_nx.nro"))
    print(f"  {'OK ' if nro_ok else 'MISSING'} angrybird2_nx.nro")

    if missing:
        print("\n*** arm64 native libs are MISSING. ***")
        print("    Provide the arm64-v8a split (config.arm64_v8a.apk) via --arch.")
        print("    NOTE: config.armeabi_v7a.apk is 32-bit ARM and will NOT work on Switch.")
    if not data_ok:
        sys.exit("\nStaging incomplete: a boot-critical data file is missing.")

    if not nro_ok:
        print("\n*** wrapper NRO is MISSING; do not reuse an older staged NRO. ***")
        print("    Build angrybird2_nx.nro, rerun with --nro, and verify its SHA-256.")

    print(f"\nDone. Copy the CONTENTS of:\n  {a.out}\nto your SD card at:\n  sdmc:/switch/angrybird2_nx/")
    print("Launch angrybird2_nx.nro from that folder via title-override (hold R over a game).")
    print(f"(pkg {PKG}, versionCode {VERSION_CODE}, versionName {VERSION_NAME})")


if __name__ == "__main__":
    main()
