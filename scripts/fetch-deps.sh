#!/usr/bin/env bash
# Downloads pinned third-party sources into build/deps (not committed).
set -euo pipefail
cd "$(dirname "$0")/.."

MBEDTLS_VERSION=3.6.8
JSON_VERSION=3.12.0
QUICKJS_VERSION=0.18.0
EJS_VERSION=0.8.0
ZLIB_VERSION=1.4
OPUS_VERSION=1.6.1
FAAD2_VERSION=2.11.4
DEPS=build/deps
mkdir -p "$DEPS"

if [ ! -d "$DEPS/mbedtls-$MBEDTLS_VERSION" ]; then
  curl -sfL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2" \
    | tar -xj -C "$DEPS"
fi
if [ ! -f "$DEPS/nlohmann/json.hpp" ]; then
  mkdir -p "$DEPS/nlohmann"
  curl -sfL -o "$DEPS/nlohmann/json.hpp" "https://github.com/nlohmann/json/releases/download/v$JSON_VERSION/json.hpp"
fi
# QuickJS-ng (MIT): runs YouTube's player JS to decipher stream URLs of the fallback client.
if [ ! -d "$DEPS/quickjs-$QUICKJS_VERSION" ]; then
  curl -sfL "https://github.com/quickjs-ng/quickjs/archive/refs/tags/v$QUICKJS_VERSION.tar.gz" | tar -xz -C "$DEPS"
fi
# zlib (zlib license): gzip decompression for InnerTube / config HTTP responses.
if [ ! -d "$DEPS/zlib-$ZLIB_VERSION" ]; then
  curl -sfL "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz" \
    | tar -xz -C "$DEPS"
fi
# Opus 1.5.2 (BSD-3) & FAAD2 2.11.4 (GPLv2+): floating-point audio decoders for max-fidelity PCM DSP.
if [ ! -d "$DEPS/opus-$OPUS_VERSION" ]; then
  curl -sfL "https://downloads.xiph.org/releases/opus/opus-$OPUS_VERSION.tar.gz" \
    | tar -xz -C "$DEPS"
fi
if [ ! -d "$DEPS/faad2-$FAAD2_VERSION" ]; then
  curl -sfL "https://github.com/knik0/faad2/archive/refs/tags/$FAAD2_VERSION.tar.gz" \
    | tar -xz -C "$DEPS"
fi
# yt-dlp EJS solver (Unlicense; bundles meriyah ISC + astring MIT): finds the n / sig functions in the player JS.
if [ ! -f "$DEPS/ejs-$EJS_VERSION/core.min.js" ]; then
  mkdir -p "$DEPS/ejs-$EJS_VERSION"
  WHEEL=$(mktemp)
  curl -sfL -o "$WHEEL" "https://files.pythonhosted.org/packages/py3/y/yt-dlp-ejs/yt_dlp_ejs-$EJS_VERSION-py3-none-any.whl"
  for f in core.min.js lib.min.js; do
    python3 -I -c "import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read('yt_dlp_ejs/yt/solver/' + sys.argv[2]))" \
      "$WHEEL" "$f" > "$DEPS/ejs-$EJS_VERSION/$f"
  done
  rm -f "$WHEEL"
fi
# Mozilla CA bundle (as packaged by curl), embedded into the core for TLS verification.
if [ ! -f "$DEPS/cacert.pem" ]; then
  curl -sfL -o "$DEPS/cacert.pem" https://curl.se/ca/cacert.pem
fi
ls "$DEPS"
