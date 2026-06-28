#!/usr/bin/env bash

###
# build_pi.sh — Build the PokerTH QML client (with the LLM eval autopilot) natively
# on a Raspberry Pi 5 / 64-bit Raspberry Pi OS (Debian). Installs the apt build and
# QML runtime dependencies, then configures and builds the pokerth_qml-client target.
#
# Requires a new-enough OS: Qt >= 6.7 and Boost >= 1.83 from apt. That means a
# Debian 13 "trixie"-based Raspberry Pi OS (or newer). Bookworm's Qt 6.4 / Boost
# 1.74 are too old — upgrade the OS. (aqtinstall is NOT an option here: it only
# ships x86_64 Linux Qt, not ARM64.)
#
# Usage:
#   ./build_pi.sh                  # install deps, then build
#   SKIP_DEPS=1 ./build_pi.sh      # skip apt install (deps already present)
#   BUILD_DIR=build ./build_pi.sh  # override build dir   (default: build)
#   JOBS=4 ./build_pi.sh           # override parallel jobs (default: nproc)
###

set -euo pipefail

BUILD_TARGET="${BUILD_TARGET:-pokerth_qml-client}"
BUILD_DIR="${BUILD_DIR:-build}"
JOBS="${JOBS:-$(nproc)}"
SKIP_DEPS="${SKIP_DEPS:-0}"
MIN_QT="6.7.0"
MIN_BOOST="1.83.0"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

log()  { echo "▶ $*"; }
warn() { echo "⚠ $*" >&2; }
die()  { echo "✗ $*" >&2; exit 1; }
command_exists() { command -v "$1" >/dev/null 2>&1; }

########################################
# 0. Host sanity
########################################

command_exists apt-get || die "apt-get not found. This script targets Raspberry Pi OS / Debian.
  On another distro, install the equivalents of the package lists below and run the
  cmake steps manually (see docs/llm_eval.md)."

########################################
# 1. Dependencies
########################################

# Required to compile.
BUILD_DEPS=(
  build-essential cmake ninja-build git
  libssl-dev libwebsocketpp-dev libprotobuf-dev protobuf-compiler
  libboost-iostreams-dev libboost-random-dev libboost-thread-dev
  libboost-filesystem-dev libboost-program-options-dev
  libboost-system-dev libboost-date-time-dev
  qt6-base-dev qt6-declarative-dev qt6-websockets-dev qt6-multimedia-dev
  qt6-shadertools-dev qt6-tools-dev libqt6opengl6-dev
  libgl-dev libegl-dev libx11-dev libxkbcommon-dev
  libfontconfig-dev libfreetype-dev libpulse-dev
)

# Needed to RUN the QML GUI (and the SQLite driver). Package names vary slightly
# across releases, so these are installed best-effort (a missing one is skipped).
RUNTIME_DEPS=(
  libqt6sql6-sqlite
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts
  qml6-module-qtquick-window qml6-module-qtquick-templates qml6-module-qtquick-shapes
  qml6-module-qtquick-dialogs qml6-module-qtmultimedia qml6-module-qtqml-workerscript
)

if [ "$SKIP_DEPS" != "1" ]; then
  log "Installing build dependencies via apt (sudo may prompt)…"
  sudo apt-get update
  sudo apt-get install -y "${BUILD_DEPS[@]}"

  log "Installing QML runtime modules (best-effort)…"
  for pkg in "${RUNTIME_DEPS[@]}"; do
    sudo apt-get install -y "$pkg" \
      || warn "runtime package '$pkg' not available on this release — skipping (install the right qml6-module-* later if the app reports a missing module)."
  done
else
  log "SKIP_DEPS=1 — skipping apt install."
fi

########################################
# 2. Version gate (Qt >= 6.7, Boost >= 1.83)
########################################

check_version() {
  # $1 = package, $2 = minimum version, $3 = human label
  local pkg="$1" min="$2" label="$3" ver
  ver="$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null || true)"
  if [ -z "$ver" ]; then
    warn "$label: '$pkg' not installed — cannot verify version here (cmake will check)."
    return 0
  fi
  if dpkg --compare-versions "$ver" ge "$min"; then
    log "$label OK: $pkg $ver (>= $min)"
  else
    local codename="unknown"
    [ -r /etc/os-release ] && codename="$(. /etc/os-release; echo "${VERSION_CODENAME:-unknown}")"
    warn "$label TOO OLD: $pkg $ver (< $min) on '$codename'. The build will likely fail."
    warn "  Upgrade to a Debian 13 'trixie'-based Raspberry Pi OS (or newer) to get a recent Qt/Boost."
  fi
}
check_version qt6-base-dev          "$MIN_QT"    "Qt6"
check_version libboost-filesystem-dev "$MIN_BOOST" "Boost"

########################################
# 3. Configure + build
########################################

# The repo ships a custom cmake/FindBoost.cmake that interferes with system-Boost
# discovery. The project's own Debian/Ubuntu CI removes it for native builds; do the
# same. This only changes your local tree — restore with: git checkout cmake/FindBoost.cmake
if [ -f cmake/FindBoost.cmake ]; then
  log "Removing cmake/FindBoost.cmake so CMake uses system Boost (local tweak; do not commit)."
  rm -f cmake/FindBoost.cmake
fi

log "Configuring CMake build in '$BUILD_DIR'…"
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_DEFAULT_CMP0144=NEW \
  -S . -B "$BUILD_DIR"

log "Building $BUILD_TARGET (-j$JOBS)… (≈10–20 min on a Pi 5)"
cmake --build "$BUILD_DIR" --target "$BUILD_TARGET" -j"$JOBS"

BIN="$BUILD_DIR/bin/$BUILD_TARGET"
[ -x "$BIN" ] || die "Build finished but binary not found at $BIN"

cat <<EOF

✔ Build complete: $BIN

Run the LLM eval autopilot (point it at your LAN model server):

  export POKERTH_LLM_ENABLE=1
  export POKERTH_LLM_ENDPOINT=http://<lan-ip>:11434/v1/chat/completions
  export POKERTH_LLM_MODEL=<model-name>
  $BIN

Then start a Local Game from the UI — seat 0 plays itself. Decisions are logged to
~/pokerth_llm_eval.jsonl  (summarise with: python3 analyze_llm_eval.py).
All options are documented in docs/llm_eval.md.
EOF
