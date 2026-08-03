#!/bin/bash
# Build x86_64 Unix-side dependencies for Wine on Apple Silicon.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"  # deps are built into $ROOT/deps
PREFIX=$ROOT/deps
SRC=$ROOT/deps-src
mkdir -p "$PREFIX" "$SRC"
cd "$SRC"

export PATH="/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export CFLAGS="-arch x86_64 -O2"
export CXXFLAGS="-arch x86_64 -O2"
export LDFLAGS="-arch x86_64 -L$PREFIX/lib"
export CPPFLAGS="-I$PREFIX/include"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export MACOSX_DEPLOYMENT_TARGET=12.0
JOBS=10

fetch() {
  local url=$1 file=${1##*/}
  [ -f "$file" ] || curl -fsSLO "$url"
  case "$file" in
    *.tar.xz) tar -xJf "$file" ;;
    *.tar.gz) tar -xzf "$file" ;;
  esac
}

echo "=== freetype ==="
[ -f "$PREFIX/lib/libfreetype.dylib" ] || {
fetch https://downloads.sourceforge.net/project/freetype/freetype2/2.13.3/freetype-2.13.3.tar.xz
cd freetype-2.13.3
arch -x86_64 ./configure --prefix="$PREFIX" --host=x86_64-apple-darwin \
  --with-png=no --with-harfbuzz=no --with-brotli=no --with-bzip2=no \
  --enable-static=no --enable-shared=yes >/dev/null
make -j$JOBS >/dev/null
make install >/dev/null
cd "$SRC"
}

echo "=== gmp ==="
fetch https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz
cd gmp-6.3.0
arch -x86_64 ./configure --prefix="$PREFIX" --host=x86_64-apple-darwin \
  --disable-assembly --enable-static=no --enable-shared=yes >/dev/null
make -j$JOBS >/dev/null
make install >/dev/null
cd "$SRC"

echo "=== nettle ==="
fetch https://ftp.gnu.org/gnu/nettle/nettle-3.10.1.tar.gz
cd nettle-3.10.1
arch -x86_64 ./configure --prefix="$PREFIX" --host=x86_64-apple-darwin \
  --disable-assembler --disable-documentation --enable-shared --disable-static >/dev/null
make -j$JOBS >/dev/null
make install >/dev/null
cd "$SRC"

echo "=== gnutls ==="
fetch https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.9.tar.xz
cd gnutls-3.8.9
arch -x86_64 ./configure --prefix="$PREFIX" --host=x86_64-apple-darwin \
  --with-included-libtasn1 --with-included-unistring --without-p11-kit \
  --without-idn --with-brotli=no --with-zstd=no --without-tpm --without-tpm2 \
  --disable-doc --disable-tests --disable-guile --disable-cxx \
  --enable-shared --disable-static >/dev/null
make -j$JOBS >/dev/null
make install >/dev/null

echo "=== DONE ==="
ls "$PREFIX/lib"
