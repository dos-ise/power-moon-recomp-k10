#!/bin/bash
# tools/linux/gen_helper.sh <x86_64|aarch64> (inside the build container of that arch, tools/linux/appimage.sh): the
# game code generator, the native tools/nativegen binary built with clang++ (as `make gen-helper`), into
# build/linux/<arch>/helper/odysseygen.
set -euo pipefail
cd "$(dirname "$0")/../.."
L=$PWD/build/linux/$1
mkdir -p "$L/helper/odysseygen"
clang++ -std=c++17 -O2 -o "$L/helper/odysseygen/gen" $(ls tools/nativegen/*.cpp | grep -v '/test_')
du -sh "$L/helper/odysseygen"
