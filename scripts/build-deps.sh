#!/usr/bin/env bash
# Cross-compiles UxPlay's native dependencies for Android:
#   OpenSSL (libcrypto, static)  and  libplist (static)
# Output: protocol-airplay/third_party/prebuilt/<abi>/{include,lib}
#
# Usage: scripts/build-deps.sh [abi ...]      (default: armeabi-v7a arm64-v8a)
# Env:   ANDROID_NDK_HOME (default: read sdk.dir from local.properties + ndk version below)
set -euo pipefail

OPENSSL_VERSION="3.5.9"   # LTS branch (supported to 2030)
# OpenSSL's checksum is read from the .sha256 file published with the release (below).
LIBPLIST_VERSION="2.8.1"
LIBPLIST_SHA256="2a1792e5820e1657c173735fde75baeebd6dd967d2ac47be1e3891ddc64ebefa"  # pinned 2026-11-19 (no upstream checksum file)
NDK_VERSION="28.2.13676358"
API=25                    # = minSdk

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/protocol-airplay/third_party"
SRC="$TP/build/src"       # downloaded sources (untrusted, own directory)
WORK="$TP/build/work"
OUT="$TP/prebuilt"
ABIS=("${@:-armeabi-v7a arm64-v8a}")
# shellcheck disable=SC2206
ABIS=(${ABIS[*]})

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
  SDK_DIR="$(sed -n 's/^sdk.dir=//p' "$ROOT/local.properties")"
  ANDROID_NDK_HOME="$SDK_DIR/ndk/$NDK_VERSION"
fi
TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64"
[ -d "$TOOLCHAIN" ] || TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
[ -x "$TOOLCHAIN/bin/llvm-ar" ] || { echo "NDK toolchain not found under $ANDROID_NDK_HOME" >&2; exit 1; }
export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
export PATH="$TOOLCHAIN/bin:$PATH"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

mkdir -p "$SRC" "$WORK" "$OUT"

fetch() { # url file expected_sha256
  local url="$1" file="$SRC/$2" want="$3"
  [ -f "$file" ] || curl -fL --retry 3 -o "$file" "$url"
  local got; got="$(shasum -a 256 "$file" | awk '{print $1}')"
  if [ -n "$want" ] && [ "$got" != "$want" ]; then
    echo "SHA256 mismatch for $2: got $got, want $want" >&2; exit 1
  fi
  echo "$2 sha256=$got"
}

# --- OpenSSL ------------------------------------------------------------------
OSSL_TGZ="openssl-$OPENSSL_VERSION.tar.gz"
OSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION"
# Verify against the checksum OpenSSL publishes with the release.
OPENSSL_SHA256="$(curl -fsSL "$OSSL_URL/$OSSL_TGZ.sha256" | awk '{print $1}')"
fetch "$OSSL_URL/$OSSL_TGZ" "$OSSL_TGZ" "$OPENSSL_SHA256"

# --- libplist -----------------------------------------------------------------
PLIST_TBZ="libplist-$LIBPLIST_VERSION.tar.bz2"
fetch "https://github.com/libimobiledevice/libplist/releases/download/$LIBPLIST_VERSION/$PLIST_TBZ" \
  "$PLIST_TBZ" "$LIBPLIST_SHA256"

for ABI in "${ABIS[@]}"; do
  case "$ABI" in
    arm64-v8a)   OSSL_TARGET=android-arm64; HOST=aarch64-linux-android;     CC_PREFIX=aarch64-linux-android$API ;;
    armeabi-v7a) OSSL_TARGET=android-arm;   HOST=arm-linux-androideabi;     CC_PREFIX=armv7a-linux-androideabi$API ;;
    *) echo "unsupported ABI $ABI" >&2; exit 1 ;;
  esac
  PREFIX="$OUT/$ABI"
  mkdir -p "$PREFIX"

  if [ ! -f "$PREFIX/lib/libcrypto.a" ]; then
    echo "=== OpenSSL $OPENSSL_VERSION for $ABI"
    rm -rf "$WORK/openssl-$ABI" && mkdir -p "$WORK/openssl-$ABI"
    tar -xzf "$SRC/$OSSL_TGZ" -C "$WORK/openssl-$ABI" --strip-components=1
    # Neutral prefix: OpenSSL bakes --prefix/--openssldir into libcrypto as strings, which would
    # otherwise leak this machine's paths into the APK. Install via DESTDIR, then copy into $PREFIX.
    ( cd "$WORK/openssl-$ABI"
      ./Configure "$OSSL_TARGET" \
        no-shared no-tests no-apps no-docs no-engine no-legacy no-ui-console \
        --prefix=/opt/openssl --openssldir=/opt/openssl/ssl --libdir=lib
      make -j"$JOBS" build_libs >/dev/null
      make install_dev DESTDIR="$WORK/openssl-$ABI/stage" >/dev/null )
    cp -R "$WORK/openssl-$ABI/stage/opt/openssl/include" "$WORK/openssl-$ABI/stage/opt/openssl/lib" "$PREFIX/"
    rm -f "$PREFIX/lib/libssl.a"   # UxPlay needs libcrypto only
  fi

  if [ ! -f "$PREFIX/lib/libplist-2.0.a" ]; then
    echo "=== libplist $LIBPLIST_VERSION for $ABI"
    rm -rf "$WORK/libplist-$ABI" && mkdir -p "$WORK/libplist-$ABI"
    tar -xjf "$SRC/$PLIST_TBZ" -C "$WORK/libplist-$ABI" --strip-components=1
    ( cd "$WORK/libplist-$ABI"
      CC="$CC_PREFIX-clang" CXX="$CC_PREFIX-clang++" AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip \
      CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC" \
      ./configure --host="$HOST" --prefix="$PREFIX" \
        --enable-static --disable-shared --without-cython --without-tests >/dev/null
      make -j"$JOBS" -C libcnary >/dev/null
      make -j"$JOBS" -C src >/dev/null
      make -C src install >/dev/null
      make -C include install >/dev/null )
  fi
done

echo
echo "Done. Prebuilt deps:"
find "$OUT" -name "*.a" -exec ls -lh {} \;
