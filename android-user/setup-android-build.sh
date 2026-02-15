#!/bin/bash
# Setup script for building QEMU TCG as an Android ARM64 shared library
# Run this from the qemu source root directory
#
# Prerequisites:
#   - Linux or WSL environment
#   - Android NDK r28b+ installed
#   - meson, ninja, python3-venv, pkg-config installed
#
# Usage:
#   export NDK_PATH=$HOME/android-ndk-r28b
#   ./android-user/setup-android-build.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$QEMU_ROOT/build-android"
DEPS_DIR="$QEMU_ROOT/build-android-deps"
API_LEVEL=24

# --- Validate NDK ---
if [ -z "$NDK_PATH" ]; then
    # Try common locations
    for d in "$HOME/android-ndk-r28b" "$HOME/android-ndk-r28" \
             "/opt/android-ndk-r28b" "$ANDROID_NDK_HOME"; do
        if [ -d "$d/toolchains/llvm" ]; then
            NDK_PATH="$d"
            break
        fi
    done
fi

if [ -z "$NDK_PATH" ] || [ ! -d "$NDK_PATH/toolchains/llvm" ]; then
    echo "ERROR: Android NDK not found."
    echo "Set NDK_PATH to your NDK installation directory."
    echo "Example: export NDK_PATH=\$HOME/android-ndk-r28b"
    exit 1
fi

# Detect host OS for prebuilt path
case "$(uname -s)" in
    Linux*)  HOST_TAG="linux-x86_64";;
    Darwin*) HOST_TAG="darwin-x86_64";;
    *)       echo "ERROR: Unsupported host OS"; exit 1;;
esac

TOOLCHAIN="$NDK_PATH/toolchains/llvm/prebuilt/$HOST_TAG"
CC="$TOOLCHAIN/bin/aarch64-linux-android${API_LEVEL}-clang"
CXX="$TOOLCHAIN/bin/aarch64-linux-android${API_LEVEL}-clang++"
AR="$TOOLCHAIN/bin/llvm-ar"
STRIP="$TOOLCHAIN/bin/llvm-strip"
RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
NM="$TOOLCHAIN/bin/llvm-nm"
SYSROOT="$TOOLCHAIN/sysroot"

if [ ! -f "$CC" ]; then
    echo "ERROR: Compiler not found at $CC"
    exit 1
fi

echo "=== QEMU Android ARM64 Build Setup ==="
echo "NDK:       $NDK_PATH"
echo "Toolchain: $TOOLCHAIN"
echo "CC:        $CC"
echo "API Level: $API_LEVEL"
echo ""

# --- Step 1: Cross-compile glib-2.0 for Android ---
echo "=== Step 1: Building glib-2.0 for Android ARM64 ==="

mkdir -p "$DEPS_DIR"

# Check if glib is already built
if [ -f "$DEPS_DIR/lib/libglib-2.0.a" ]; then
    echo "glib-2.0 already built, skipping."
else
    echo "Downloading glib..."
    GLIB_VERSION="2.80.0"
    GLIB_TARBALL="glib-${GLIB_VERSION}.tar.xz"
    GLIB_URL="https://download.gnome.org/sources/glib/2.80/${GLIB_TARBALL}"

    cd "$DEPS_DIR"
    if [ ! -f "$GLIB_TARBALL" ]; then
        wget -q "$GLIB_URL" -O "$GLIB_TARBALL"
    fi
    if [ ! -d "glib-${GLIB_VERSION}" ]; then
        tar xf "$GLIB_TARBALL"
    fi

    # Create meson cross file for glib
    cat > "$DEPS_DIR/glib-cross.txt" << CROSSEOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
ranlib = '$RANLIB'
nm = '$NM'
pkgconfig = '/usr/bin/pkg-config'

[built-in options]
c_args = ['-fPIC', '-DANDROID', '-D__ANDROID_API__=${API_LEVEL}']
c_link_args = ['-llog']
default_library = 'static'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
sys_root = '$SYSROOT'
pkg_config_libdir = '$DEPS_DIR/lib/pkgconfig'
needs_exe_wrapper = true
CROSSEOF

    cd "glib-${GLIB_VERSION}"
    meson setup builddir \
        --cross-file="$DEPS_DIR/glib-cross.txt" \
        --prefix="$DEPS_DIR" \
        --default-library=static \
        -Dtests=false \
        -Dglib_checks=false \
        -Dglib_assert=false \
        -Dlibmount=disabled \
        -Dselinux=disabled \
        -Dxattr=false \
        -Dlibelf=disabled \
        -Dintrospection=disabled \
        -Dnls=disabled \
        -Doss_fuzz=disabled \
        -Ddtrace=false \
        -Dsystemtap=false \
        -Db_lto=false \
        2>&1

    cd builddir
    ninja -j$(nproc)
    ninja install
    echo "glib-2.0 built successfully."
fi

# --- Step 2: Generate Meson cross file for QEMU ---
echo ""
echo "=== Step 2: Generating QEMU cross-compilation file ==="

CROSS_FILE="$BUILD_DIR/android-cross.txt"
mkdir -p "$BUILD_DIR"

cat > "$CROSS_FILE" << CROSSEOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
ranlib = '$RANLIB'
nm = '$NM'
objcopy = '$TOOLCHAIN/bin/llvm-objcopy'
objdump = '$TOOLCHAIN/bin/llvm-objdump'
readelf = '$TOOLCHAIN/bin/llvm-readelf'
pkgconfig = '/usr/bin/pkg-config'

[built-in options]
c_args = ['-fPIC', '-DANDROID', '-D__ANDROID_API__=${API_LEVEL}', '-Wno-unused-function']
c_link_args = ['-llog', '-landroid']

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
sys_root = '$SYSROOT'
pkg_config_libdir = '$DEPS_DIR/lib/pkgconfig'
needs_exe_wrapper = true
CROSSEOF

echo "Cross file written to: $CROSS_FILE"

# --- Step 3: Configure QEMU ---
echo ""
echo "=== Step 3: Configuring QEMU ==="

cd "$QEMU_ROOT"
cd "$BUILD_DIR"

PKG_CONFIG_LIBDIR="$DEPS_DIR/lib/pkgconfig" \
PKG_CONFIG_PATH="$DEPS_DIR/lib/pkgconfig" \
"$QEMU_ROOT/configure" \
    --cross-file="$CROSS_FILE" \
    --target-list=arm-linux-user \
    --disable-system \
    --disable-bsd-user \
    --disable-tools \
    --disable-guest-agent \
    --disable-docs \
    --disable-plugins \
    --disable-capstone \
    --static \
    2>&1

echo ""
echo "=== Step 4: Building ==="
ninja -j$(nproc) qemu-arm 2>&1

echo ""
echo "=== Build Complete ==="
file "$BUILD_DIR/qemu-arm"
echo ""
echo "Binary: $BUILD_DIR/qemu-arm"
echo "To verify: file $BUILD_DIR/qemu-arm (should show 'aarch64')"
