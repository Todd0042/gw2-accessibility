#!/usr/bin/env bash
set -euo pipefail

NAME="GW2Accessibility"
BUILD_DIR="build"

echo "==> Checking prerequisites..."
for cmd in cmake x86_64-w64-mingw32-g++; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "ERROR: $cmd not found. Install mingw-w64-gcc cmake."
        exit 1
    fi
done

if [ "${1:-}" == "--clean" ]; then
    echo "==> Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

echo "==> Configuring..."
cmake -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake \
    -DCMAKE_BUILD_TYPE=Release

echo "==> Building..."
cmake --build "$BUILD_DIR" --config Release -- -j"$(nproc)"

echo "==> Verifying exports..."
x86_64-w64-mingw32-objdump -x "$BUILD_DIR/${NAME}.dll" | grep "base\[" || echo "    (no exports found)"

echo "==> Verifying no stray MinGW DLL dependencies..."
x86_64-w64-mingw32-objdump -p "$BUILD_DIR/${NAME}.dll" | grep "DLL Name" || true

echo ""
echo "==> DONE: $BUILD_DIR/${NAME}.dll"
