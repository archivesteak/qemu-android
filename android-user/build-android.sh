#!/bin/bash
# Build QEMU arm-linux-user for Android AArch64
# Run from the QEMU source root: ./android-user/build-android.sh
#
# Prerequisites:
#   - Android NDK r28b at ~/android-ndk-r28b
#   - Cross-compiled deps (glib, libiconv, libffi) at ~/android-deps/install
#   - pkg-config wrapper at $TOOLCHAIN/bin/aarch64-linux-android26-pkg-config

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
NDK_PATH="${NDK_PATH:-$HOME/android-ndk-r28b}"
TOOLCHAIN="$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64"
DEPS_PREFIX="${DEPS_PREFIX:-$HOME/android-deps/install}"
BUILD_DIR="$QEMU_ROOT/build-android"
API=26

export CC="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang"
export CXX="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang++"
export OBJCC="$CC"
export AR="$TOOLCHAIN/bin/llvm-ar"
export NM="$TOOLCHAIN/bin/llvm-nm"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export STRIP="$TOOLCHAIN/bin/llvm-strip"
export READELF="$TOOLCHAIN/bin/llvm-readelf"
export OBJCOPY="$TOOLCHAIN/bin/llvm-objcopy"
export PKG_CONFIG="$TOOLCHAIN/bin/aarch64-linux-android${API}-pkg-config"
export PATH="$TOOLCHAIN/bin:$PATH"

# Ensure pkg-config wrapper exists
if [ ! -x "$PKG_CONFIG" ]; then
    cat > "$PKG_CONFIG" << 'EOF'
#!/bin/bash
export PKG_CONFIG_LIBDIR="$HOME/android-deps/install/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""
exec pkg-config "$@"
EOF
    chmod +x "$PKG_CONFIG"
    # Fix the HOME variable in the script
    sed -i "s|\$HOME|$HOME|g" "$PKG_CONFIG"
fi

# Ensure tool symlinks exist
for tool in ar nm ranlib strip objcopy objdump readelf; do
    link="$TOOLCHAIN/bin/aarch64-linux-android${API}-$tool"
    [ -e "$link" ] || ln -sf "$TOOLCHAIN/bin/llvm-$tool" "$link"
done

case "${1:-build}" in
    configure)
        rm -rf "$BUILD_DIR"
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        "$QEMU_ROOT/configure" \
            --target-list=arm-linux-user \
            --cross-prefix="aarch64-linux-android${API}-" \
            --cc="$CC" --cxx="$CXX" \
            --disable-system --disable-bsd-user \
            --disable-tools --disable-guest-agent \
            --disable-docs --disable-plugins \
            --disable-brlapi --disable-cap-ng \
            --disable-curl --disable-gnutls \
            --disable-gtk --disable-sdl \
            --disable-vnc --disable-xen \
            --disable-libusb --disable-zstd \
            --extra-cflags="-fPIC -DANDROID" \
            --extra-ldflags="-L$DEPS_PREFIX/lib -liconv -lffi"
        ;;
    build)
        cd "$BUILD_DIR"
        ninja -j"$(nproc)" qemu-arm
        ;;
    clean)
        rm -rf "$BUILD_DIR"
        ;;
    reconfigure)
        "$0" configure
        "$0" build
        ;;
    *)
        echo "Usage: $0 {configure|build|clean|reconfigure}"
        exit 1
        ;;
esac
