#!/usr/bin/env bash
# Build helper for angrybird2_nx. Runs in an msys2 LOGIN shell so devkitPro's
# /opt/devkitpro mount + TMP are set up correctly (native Git Bash fails on TMP).
# Usage: /c/msys64/usr/bin/bash -lc '/path/to/build.sh [clean]'
set -e
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=/opt/devkitpro/devkitA64
export DEVKITARM=/opt/devkitpro/devkitARM
export DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH=$DEVKITA64/bin:$DEVKITPRO/tools/bin:$DEVKITPRO/portlibs/switch/bin:$PATH
cd "$(dirname "$0")" || exit 1
if [ "$1" = "clean" ]; then make clean; shift; fi
make "$@"
