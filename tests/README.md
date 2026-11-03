The display tests compile the window implementation and Unity JNI int dispatcher with host stubs for libnx. They also compile the actual JNI field reader and EGL size fallback extracted from their source files.

Run with a host GCC and the Mesa EGL headers:

```sh
CC=gcc MESA_INCLUDE=/path/to/mesa/include python3 tests/run_display_tests.py
```

The tests cover handheld and docked startup, saved handheld buffer sizes during docked startup, JNI/window/EGL size agreement, config overrides, rotation, failed resizes, invalid geometry, repeated window acquisition, docking with registered buffers, and selecting a new size after EGL releases its buffers. A Switch is required to reproduce issue #3 and verify actual presentation.
