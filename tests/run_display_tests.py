"""Run display regression tests against the port's C code using a host compiler."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == "{": depth += 1
        elif source[end] == "}": depth -= 1
        end += 1
    return source[start:end] + "\n"


with tempfile.TemporaryDirectory() as temporary:
    out = Path(temporary)
    (out / "field_int.inc").write_text(function(
        (ROOT / "source/jni_fake.c").read_text(encoding="utf-8"),
        "static juint field_int("), encoding="utf-8")
    (out / "egl_query.inc").write_text(function(
        (ROOT / "source/imports.c").read_text(encoding="utf-8"),
        "static EGLBoolean egl_QuerySurface_log("), encoding="utf-8")
    executable = out / ("display_test.exe" if os.name == "nt" else "display_test")
    host_options = ["-Dfsync=_commit", "-include", "io.h"] if os.name == "nt" else []
    subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-DEGLAPI=",
        *host_options,
        "-I"+str(ROOT / "tests/stubs"), "-I"+str(ROOT / "source"), "-I"+str(out),
        "-I"+os.environ["MESA_INCLUDE"], str(ROOT / "tests/display_test.c"),
        str(ROOT / "source/android_window.c"), str(ROOT / "source/unity_jni.c"),
        "-o", str(executable)], check=True)
    for mode in ("handheld", "docked", "custom", "rotation"):
        subprocess.run([str(executable), mode], check=True)
