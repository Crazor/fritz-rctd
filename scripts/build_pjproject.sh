#!/usr/bin/env bash
#
# Builds the PJPROJECT core libraries (including the pjsua2 C++ API) for
# the native fritz-rctd helper. Video/SDL/ffmpeg and the sound driver are
# deliberately disabled: video isn't needed, and no audio backend
# (CoreAudio/ALSA/PulseAudio) is required, because the helper doesn't use
# any local audio hardware (both call legs run over real phones, not this
# machine).
#
# Supported platforms: macOS and Linux (both use PJPROJECT's
# ./configure + make build; Windows uses a different build system
# entirely and isn't covered here).
#
# Result lands in $PJ_DIR (default: /tmp/pjproject).
#
# PJ_REPO_URL overrides where PJPROJECT is cloned from - useful in
# sandboxed environments that only allow access to repos you own (e.g. a
# fork of pjsip/pjproject), not arbitrary third-party ones.
set -euo pipefail

# Set when the caller explicitly points us at a directory, so we know not
# to override it with an auto-detected checkout below.
PJ_DIR_EXPLICIT="${PJ_DIR:-}"
PJ_DIR="${PJ_DIR:-/tmp/pjproject}"
PJ_VERSION="${PJ_VERSION:-2.17}"
PJ_REPO_URL="${PJ_REPO_URL:-https://github.com/pjsip/pjproject}"

# Look for a PJPROJECT source tree already checked out somewhere in the
# container (e.g. an agent sandbox that pre-clones the repo into some
# workspace directory) so we can build that instead of hitting the
# network. Identified by a file path unique enough to PJPROJECT's layout
# that false positives are essentially impossible.
find_existing_pjproject_checkout() {
    local marker="pjsip/include/pjsip/sip_transaction.h"
    local hit candidate
    while IFS= read -r hit; do
        candidate="${hit%/$marker}"
        if [[ -x "$candidate/configure" && -d "$candidate/pjlib/include/pj" ]]; then
            echo "$candidate"
            return 0
        fi
    done < <(find / \( -path /proc -o -path /sys -o -path /dev \) -prune -o \
                   -type f -path "*/$marker" -print 2>/dev/null)
    return 1
}

if [[ -z "$PJ_DIR_EXPLICIT" && ! -d "$PJ_DIR" ]]; then
    existing="$(find_existing_pjproject_checkout || true)"
    if [[ -n "$existing" ]]; then
        echo "Found existing PJPROJECT checkout at $existing, building that instead of cloning."
        PJ_DIR="$existing"
    fi
fi

if [[ ! -d "$PJ_DIR" ]]; then
    echo "Cloning PJPROJECT from $PJ_REPO_URL to $PJ_DIR ..."
    git clone --depth 1 --branch "$PJ_VERSION" "$PJ_REPO_URL" "$PJ_DIR" \
        || git clone --depth 1 "$PJ_REPO_URL" "$PJ_DIR"
fi

cd "$PJ_DIR"

cat > pjlib/include/pj/config_site.h << 'EOF'
#define PJ_HAS_SSL_SOCK 1
#define PJMEDIA_HAS_VIDEO 0
#define PJMEDIA_VIDEO_DEV_HAS_SDL 0
#define PJMEDIA_VIDEO_DEV_HAS_DARWIN 0
#define PJMEDIA_VIDEO_DEV_HAS_METAL 0
#define PJMEDIA_HAS_FFMPEG 0
#define PJMEDIA_HAS_FFMPEG_VID_CODEC 0
#define PJMEDIA_HAS_LIBAVDEVICE 0
#define PJMEDIA_HAS_LIBAVFORMAT 0
#define PJMEDIA_HAS_LIBAVCODEC 0
#define PJMEDIA_HAS_LIBSWSCALE 0
#define PJMEDIA_HAS_LIBAVUTIL 0
EOF

CONFIGURE_CFLAGS="-DPJ_HAS_IPV6=1"
CONFIGURE_LDFLAGS=""
NPROC="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN)"

if [[ "$(uname)" == "Darwin" ]]; then
    # OpenSSL isn't in the default search path on macOS (not installed by
    # the system, and Homebrew doesn't link into /usr/include or
    # /usr/lib) - point configure at the Homebrew prefix explicitly. On
    # Linux, distro packages (e.g. libssl-dev) install into the default
    # search path already, so no extra flags are needed there.
    OPENSSL_PREFIX="$(brew --prefix openssl@3)"
    CONFIGURE_CFLAGS="${CONFIGURE_CFLAGS} -I${OPENSSL_PREFIX}/include"
    CONFIGURE_LDFLAGS="-L${OPENSSL_PREFIX}/lib"
    NPROC="$(sysctl -n hw.ncpu)"
fi

./configure --enable-shared --disable-video --disable-sdl --disable-ffmpeg --disable-sound \
    CFLAGS="${CONFIGURE_CFLAGS}" \
    LDFLAGS="${CONFIGURE_LDFLAGS}"

make dep
make -j"${NPROC}"

echo "PJPROJECT build finished in $PJ_DIR"
