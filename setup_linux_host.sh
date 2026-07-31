#!/usr/bin/env bash
set -Eeuo pipefail

if [[ "$(uname -s)" != "Linux" || ! -r /etc/os-release ]]; then
    echo "Questo script richiede Ubuntu Linux." >&2
    exit 1
fi

# shellcheck disable=SC1091
source /etc/os-release
if [[ "${ID:-}" != "ubuntu" || "${VERSION_ID:-}" != "20.04" ]]; then
    echo "Questa configurazione e' preparata per Ubuntu 20.04 (focal)." >&2
    echo "Sistema rilevato: ${PRETTY_NAME:-sconosciuto}" >&2
    exit 1
fi

case "$(uname -m)" in
    x86_64|amd64) ;;
    *)
        echo "La preparazione cross x64 -> ARM64 richiede un host x86_64." >&2
        exit 1
        ;;
esac

sudo apt-get update
sudo apt-get install -y --no-install-recommends software-properties-common
sudo add-apt-repository -y universe
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    curl \
    dpkg-dev \
    file \
    g++-aarch64-linux-gnu \
    gcc-aarch64-linux-gnu \
    libdbus-1-dev \
    libfontconfig1-dev \
    libfreetype6-dev \
    libgl1-mesa-dev \
    libglu1-mesa-dev \
    libice-dev \
    libsm-dev \
    libx11-dev \
    libx11-xcb-dev \
    libxcb-glx0-dev \
    libxcb-icccm4-dev \
    libxcb-image0-dev \
    libxcb-keysyms1-dev \
    libxcb-randr0-dev \
    libxcb-render-util0-dev \
    libxcb-render0-dev \
    libxcb-shape0-dev \
    libxcb-shm0-dev \
    libxcb-sync-dev \
    libxcb-util-dev \
    libxcb-xfixes0-dev \
    libxcb-xinerama0-dev \
    libxcb-xinput-dev \
    libxcb-xkb-dev \
    libxcb1-dev \
    libxext-dev \
    libxfixes-dev \
    libxi-dev \
    libxkbcommon-dev \
    libxkbcommon-x11-dev \
    libxrender-dev \
    make \
    ninja-build \
    patch \
    perl \
    pkg-config \
    python3 \
    xz-utils \
    zlib1g-dev

printf '\nToolchain installata:\n'
gcc --version | head -n 1
aarch64-linux-gnu-g++ --version | head -n 1
