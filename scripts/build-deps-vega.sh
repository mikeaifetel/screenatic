#!/usr/bin/env bash
# Cross-compiles the native dependencies of the Vega OS (Fire TV) receiver:
#   OpenSSL (libcrypto)   UxPlay's pairing/encryption
#   libplist              UxPlay's plist parsing
#   FFmpeg (aac only)     AAC-ELD decoder: Vega's MSE can't take AAC-ELD (see vega/README.md)
#   libFLAC               PCM -> FLAC, the audio format Vega's MSE actually plays
# Output: vega/third_party/prebuilt/<arch>/{include,lib}   (static, PIC)
#
# Uses the clang + glibc sysroot that `vega build` sets up, so run a native build once first:
#   (cd vega/native && npm run build:debug)
#
# Usage: scripts/build-deps-vega.sh [arch ...]   (default: armv7 aarch64 x86_64)
set -euo pipefail

OPENSSL_VERSION="3.5.9"
LIBPLIST_VERSION="2.8.1"
LIBPLIST_SHA256="2a1792e5820e1657c173735fde75baeebd6dd967d2ac47be1e3891ddc64ebefa"
FFMPEG_VERSION="7.1.2"
FLAC_VERSION="1.5.0"
# FFmpeg and FLAC publish signatures, not checksum files: pinned on first download.
FFMPEG_SHA256="089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304"  # pinned 2026-11-23
FLAC_SHA256="f2c1c76592a82ffff8413ba3c4a1299b6c7ab06c734dee03fd88630485c2b920"  # pinned 2026-11-23

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/vega/third_party"
SRC="$TP/build/src"        # downloaded sources (untrusted, own directory)
WORK="$TP/build/work"
OUT="$TP/prebuilt"
VEGA_BUILD="$ROOT/vega/native/build/private"
ARCHS=("${@:-armv7 aarch64 x86_64}")
# shellcheck disable=SC2206
ARCHS=(${ARCHS[*]})
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

mkdir -p "$SRC" "$WORK" "$OUT"

fetch() { # url file expected_sha256 (empty: print it so it can be pinned)
  local url="$1" file="$SRC/$2" want="$3"
  [ -f "$file" ] || curl -fL --retry 3 -o "$file" "$url"
  local got; got="$(shasum -a 256 "$file" | awk '{print $1}')"
  if [ -n "$want" ] && [ "$got" != "$want" ]; then
    echo "SHA256 mismatch for $2: got $got, want $want" >&2; exit 1
  fi
  echo "$2 sha256=$got"
}

OSSL_TGZ="openssl-$OPENSSL_VERSION.tar.gz"
OSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION"
if [ ! -f "$SRC/$OSSL_TGZ" ] && [ -f "$ROOT/protocol-airplay/third_party/build/src/$OSSL_TGZ" ]; then
  cp "$ROOT/protocol-airplay/third_party/build/src/$OSSL_TGZ" "$SRC/"
fi
fetch "$OSSL_URL/$OSSL_TGZ" "$OSSL_TGZ" "$(curl -fsSL "$OSSL_URL/$OSSL_TGZ.sha256" | awk '{print $1}')"

PLIST_TBZ="libplist-$LIBPLIST_VERSION.tar.bz2"
if [ ! -f "$SRC/$PLIST_TBZ" ] && [ -f "$ROOT/protocol-airplay/third_party/build/src/$PLIST_TBZ" ]; then
  cp "$ROOT/protocol-airplay/third_party/build/src/$PLIST_TBZ" "$SRC/"
fi
fetch "https://github.com/libimobiledevice/libplist/releases/download/$LIBPLIST_VERSION/$PLIST_TBZ" "$PLIST_TBZ" "$LIBPLIST_SHA256"

FF_TXZ="ffmpeg-$FFMPEG_VERSION.tar.xz"
fetch "https://ffmpeg.org/releases/$FF_TXZ" "$FF_TXZ" "$FFMPEG_SHA256"

FLAC_TXZ="flac-$FLAC_VERSION.tar.xz"
fetch "https://downloads.xiph.org/releases/flac/$FLAC_TXZ" "$FLAC_TXZ" "$FLAC_SHA256"

for ARCH in "${ARCHS[@]}"; do
  case "$ARCH" in
    armv7)   TRIPLE=armv7a-linux-gnueabihf; HOST=arm-linux-gnueabihf;  OSSL_TARGET=linux-armv4
             ARCHFLAGS="-mcpu=cortex-a7 -mthumb -mfpu=neon-vfpv4 -mfloat-abi=hard"; FF_ARCH=arm ;;
    aarch64) TRIPLE=aarch64-linux-gnu;       HOST=aarch64-linux-gnu;    OSSL_TARGET=linux-aarch64
             ARCHFLAGS=""; FF_ARCH=aarch64 ;;
    x86_64)  TRIPLE=x86_64-unknown-linux-gnu; HOST=x86_64-linux-gnu;    OSSL_TARGET=linux-x86_64
             ARCHFLAGS=""; FF_ARCH=x86_64 ;;
    *) echo "unsupported arch $ARCH" >&2; exit 1 ;;
  esac
  BIN="$VEGA_BUILD/toolchain/$ARCH/mac/bin"
  SYSROOT="$VEGA_BUILD/sysroot-$ARCH-debug"
  # Same runtime (compiler-rt, crtbegin, libunwind) the Vega build links with.
  RESDIR="$(ls -d "$VEGA_BUILD/toolchain/$ARCH/mac/$ARCH/lib/clang/"* | head -1)"
  [ -x "$BIN/clang" ] && [ -d "$SYSROOT/usr/include" ] || {
    echo "Vega toolchain for $ARCH not found under $VEGA_BUILD; run (cd vega/native && npm run build:debug) first" >&2; exit 1; }

  # Wrapper scripts so autotools/OpenSSL see a plain cross compiler.
  TC="$WORK/tc-$ARCH"; mkdir -p "$TC"
  for tool in clang clang++; do
    cat > "$TC/$TRIPLE-$tool" <<EOF
#!/bin/bash
exec "$BIN/$tool" --target=$TRIPLE --sysroot="$SYSROOT" -resource-dir="$RESDIR" $ARCHFLAGS -fPIC "\$@"
EOF
    chmod +x "$TC/$TRIPLE-$tool"
  done
  export CC="$TC/$TRIPLE-clang" CXX="$TC/$TRIPLE-clang++"
  export AR="$BIN/llvm-ar" RANLIB="$BIN/llvm-ranlib" NM="$BIN/llvm-nm" STRIP="$BIN/llvm-strip"
  export CFLAGS="-O2" CXXFLAGS="-O2" LDFLAGS="-fuse-ld=lld"
  PREFIX="$OUT/$ARCH"; mkdir -p "$PREFIX"

  if [ ! -f "$PREFIX/lib/libcrypto.a" ]; then
    echo "=== OpenSSL $OPENSSL_VERSION for $ARCH"
    rm -rf "$WORK/openssl-$ARCH" && mkdir -p "$WORK/openssl-$ARCH"
    tar -xzf "$SRC/$OSSL_TGZ" -C "$WORK/openssl-$ARCH" --strip-components=1
    # Neutral prefix: OpenSSL bakes --prefix/--openssldir into libcrypto as strings.
    ( cd "$WORK/openssl-$ARCH"
      ./Configure "$OSSL_TARGET" no-shared no-tests no-apps no-docs no-engine no-legacy no-ui-console \
        --prefix=/opt/openssl --openssldir=/opt/openssl/ssl --libdir=lib
      make -j"$JOBS" build_libs >/dev/null
      make install_dev DESTDIR="$WORK/openssl-$ARCH/stage" >/dev/null )
    cp -R "$WORK/openssl-$ARCH/stage/opt/openssl/include" "$WORK/openssl-$ARCH/stage/opt/openssl/lib" "$PREFIX/"
    rm -f "$PREFIX/lib/libssl.a"
  fi

  if [ ! -f "$PREFIX/lib/libplist-2.0.a" ]; then
    echo "=== libplist $LIBPLIST_VERSION for $ARCH"
    rm -rf "$WORK/libplist-$ARCH" && mkdir -p "$WORK/libplist-$ARCH"
    tar -xjf "$SRC/$PLIST_TBZ" -C "$WORK/libplist-$ARCH" --strip-components=1
    ( cd "$WORK/libplist-$ARCH"
      ./configure --host="$HOST" --prefix="$PREFIX" --enable-static --disable-shared \
        --without-cython --without-tests >/dev/null
      make -j"$JOBS" -C libcnary >/dev/null
      make -j"$JOBS" -C src >/dev/null
      make -C src install >/dev/null
      make -C include install >/dev/null )
  fi

  if [ ! -f "$PREFIX/lib/libavcodec.a" ]; then
    echo "=== FFmpeg $FFMPEG_VERSION (aac decoder only) for $ARCH"
    rm -rf "$WORK/ffmpeg-$ARCH" && mkdir -p "$WORK/ffmpeg-$ARCH"
    tar -xJf "$SRC/$FF_TXZ" -C "$WORK/ffmpeg-$ARCH" --strip-components=1
    ( cd "$WORK/ffmpeg-$ARCH"
      ./configure --prefix="$PREFIX" --enable-cross-compile --target-os=linux --arch="$FF_ARCH" \
        --cc="$CC" --cxx="$CXX" --ar="$AR" --ranlib="$RANLIB" --nm="$NM" --strip="$STRIP" \
        --enable-pic --enable-static --disable-shared --disable-programs --disable-doc \
        --disable-everything --enable-decoder=aac --enable-parser=aac \
        --disable-avdevice --disable-avformat --disable-swscale --disable-postproc --disable-avfilter \
        --disable-swresample --disable-network --disable-iconv --disable-zlib --disable-bzlib \
        --disable-lzma --disable-xlib --disable-vaapi --disable-vdpau --disable-x86asm \
        --disable-autodetect --disable-debug >/dev/null
      make -j"$JOBS" >/dev/null
      make install >/dev/null )
  fi

  if [ ! -f "$PREFIX/lib/libFLAC.a" ]; then
    echo "=== libFLAC $FLAC_VERSION for $ARCH"
    rm -rf "$WORK/flac-$ARCH" && mkdir -p "$WORK/flac-$ARCH"
    tar -xJf "$SRC/$FLAC_TXZ" -C "$WORK/flac-$ARCH" --strip-components=1
    ( cd "$WORK/flac-$ARCH"
      ./configure --host="$HOST" --prefix="$PREFIX" --enable-static --disable-shared --disable-ogg \
        --disable-programs --disable-examples --disable-cpplibs --disable-doxygen-docs \
        --disable-xmms-plugin --disable-stack-smash-protection >/dev/null
      make -j"$JOBS" -C src/libFLAC >/dev/null
      make -C src/libFLAC install >/dev/null
      make -C include install >/dev/null )
  fi
done

echo
echo "Done. Prebuilt deps:"
find "$OUT" -name "*.a" -exec ls -lh {} \; | awk '{print $5, $9}'
