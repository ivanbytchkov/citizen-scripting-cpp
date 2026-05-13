#!/bin/bash
set -e

SERVER_DIR="${FX_SERVER_DIR:?Set FX_SERVER_DIR to your cfx-server directory}"
RESOURCE_DIR="${FX_RESOURCE_DIR:?Set FX_RESOURCE_DIR to your resource directory}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

cd "$PROJECT_DIR"
python3 tools/code-gen/build.py "$@"
premake5 gmake2
make -C "$BUILD_DIR" config=release \
    CC="zig cc -target x86_64-linux-musl" \
    CXX="zig c++ -target x86_64-linux-musl" \
    -j"$(nproc)"

mkdir -p "$RESOURCE_DIR"
cp "$BUILD_DIR/bin/Release/libcitizen-scripting-cpp.so" "$SERVER_DIR/"
cp "$BUILD_DIR/bin/Release/server.so" "$RESOURCE_DIR/"
cp -a "$PROJECT_DIR/tools/example/." "$RESOURCE_DIR/"

echo "Deployed to $SERVER_DIR and $RESOURCE_DIR"
