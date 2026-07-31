#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"
QT_VERSION="5.15.2"
QT_INSTALL_BASE="${PEARE_QT_INSTALL_BASE:-$HOME/.local/opt/Qt/${QT_VERSION}}"
QT_X64_ROOT="${QT5_ROOT_X64:-$QT_INSTALL_BASE/gcc_64}"
QT_ARM64_ROOT="${QT5_ROOT_ARM64:-$QT_INSTALL_BASE/gcc_arm64}"
QT_ARM64_HOST_ROOT="${QT5_HOST_ROOT_ARM64:-$QT_INSTALL_BASE/gcc_arm64_host}"
ARM64_SYSROOT="${PEARE_ARM64_SYSROOT:-$HOME/.cache/peare/sysroots/ubuntu-focal-arm64}"
QT_BUILDER="$ROOT_DIR/cmake/linux/build_qt515.sh"
ARM64_TOOLCHAIN="$ROOT_DIR/cmake/toolchains/linux-arm64-gnu.cmake"

usage() {
    cat <<'USAGE'
Uso: ./build_linux.sh [--arch x64|arm64|all] [--clean] [--run] [--rebuild-qt]

Build Linux senza Docker:
  x64    compilazione nativa GNU/Linux x86-64
  arm64  cross-compilazione GNU/Linux AArch64 dalla VM x86-64
  all    entrambe

Qt Base 5.15.2 viene compilato automaticamente dai sorgenti se manca.

Variabili:
  BUILD_TYPE             Release, Debug, RelWithDebInfo o MinSizeRel
  JOBS                   parallelismo della build Peare
  QT_BUILD_JOBS          parallelismo della build Qt
  PEARE_QT_INSTALL_BASE  base Qt, default ~/.local/opt/Qt/5.15.2
  QT5_ROOT_X64           Qt 5.15 x64 gia' installato
  QT5_ROOT_ARM64         Qt 5.15 ARM64 gia' installato
  QT5_HOST_ROOT_ARM64    tool Qt host x64 della build ARM64
  PEARE_ARM64_SYSROOT    sysroot Focal ARM64 locale
USAGE
}

normalize_arch() {
    case "$1" in
        x64|amd64|x86_64) printf '%s\n' x64 ;;
        arm64|aarch64) printf '%s\n' arm64 ;;
        all) printf '%s\n' all ;;
        *) return 1 ;;
    esac
}

case "$(uname -m)" in
    x86_64|amd64) host_arch=x64 ;;
    aarch64|arm64) host_arch=arm64 ;;
    *) echo "Architettura host non supportata: $(uname -m)" >&2; exit 1 ;;
esac

target_arch="$host_arch"
clean=0
run=0
rebuild_qt=0

while (($#)); do
    case "$1" in
        --arch)
            (($# >= 2)) || { echo "--arch richiede un valore" >&2; exit 2; }
            target_arch="$(normalize_arch "$2")" || {
                echo "Architettura non supportata: $2" >&2
                exit 2
            }
            shift 2
            ;;
        --clean) clean=1; shift ;;
        --run) run=1; shift ;;
        --rebuild-qt) rebuild_qt=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Argomento sconosciuto: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if (( run )) && [[ "$target_arch" == all ]]; then
    echo "--run richiede una sola architettura." >&2
    exit 2
fi

for command_name in cmake file ninja; do
    command -v "$command_name" >/dev/null 2>&1 || {
        echo "Tool mancante: $command_name. Esegui ./setup_linux_host.sh" >&2
        exit 1
    }
done

ensure_qt() {
    local arch="$1" root
    case "$arch" in
        x64) root="$QT_X64_ROOT" ;;
        arm64) root="$QT_ARM64_ROOT" ;;
    esac

    local valid=0
    if [[ "$arch" == x64 && -x "$root/bin/qmake" ]]; then
        [[ "$($root/bin/qmake -query QT_VERSION 2>/dev/null || true)" == "$QT_VERSION" ]] && valid=1
    elif [[ "$arch" == arm64 \
            && -f "$root/lib/libQt5Core.so.${QT_VERSION}" \
            && -x "$QT_ARM64_HOST_ROOT/bin/moc" \
            && -f "$ARM64_SYSROOT/usr/lib/aarch64-linux-gnu/libX11.so" ]]; then
        file "$root/lib/libQt5Core.so.${QT_VERSION}" | grep -q 'ARM aarch64' && valid=1
    fi

    if (( rebuild_qt )) || (( ! valid )); then
        local -a args=(--arch "$arch")
        (( rebuild_qt )) && args+=(--clean)
        "$QT_BUILDER" "${args[@]}"
    fi
}

configure_and_build_x64() {
    local build_dir="$ROOT_DIR/build-linux-x64"
    ensure_qt x64
    (( clean )) && rm -rf "$build_dir"

    echo "=== Peare: Qt ${QT_VERSION} / x64 / ${BUILD_TYPE} ==="
    cmake -S "$ROOT_DIR" -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_PREFIX_PATH="$QT_X64_ROOT" \
        -DQt5_DIR="$QT_X64_ROOT/lib/cmake/Qt5" \
        -DPEARE_QT_MAJOR=5 \
        -DPEARE_CXX_STANDARD=11
    cmake --build "$build_dir" --parallel "$JOBS"

    file "$build_dir/Peare" | grep -q 'x86-64' || {
        echo "Il binario x64 prodotto ha un'architettura inattesa." >&2
        file "$build_dir/Peare" >&2
        exit 1
    }
    file "$build_dir/Peare"
}

configure_and_build_arm64() {
    local build_dir="$ROOT_DIR/build-linux-arm64"
    ensure_qt arm64
    command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 || {
        echo "Cross-compiler ARM64 mancante. Esegui ./setup_linux_host.sh" >&2
        exit 1
    }
    (( clean )) && rm -rf "$build_dir"

    echo "=== Peare: Qt ${QT_VERSION} / ARM64 / ${BUILD_TYPE} ==="
    PEARE_ARM64_SYSROOT="$ARM64_SYSROOT" \
    QT5_ROOT_ARM64="$QT_ARM64_ROOT" \
    PATH="$QT_ARM64_HOST_ROOT/bin:$PATH" \
    cmake -S "$ROOT_DIR" -B "$build_dir" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ARM64_TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_PREFIX_PATH="$QT_ARM64_ROOT" \
        -DQt5_DIR="$QT_ARM64_ROOT/lib/cmake/Qt5" \
        -DPEARE_QT_HOST_PATH="$QT_ARM64_HOST_ROOT" \
        -DPEARE_QT_MAJOR=5 \
        -DPEARE_CXX_STANDARD=11
    PEARE_ARM64_SYSROOT="$ARM64_SYSROOT" \
    QT5_ROOT_ARM64="$QT_ARM64_ROOT" \
    PATH="$QT_ARM64_HOST_ROOT/bin:$PATH" \
        cmake --build "$build_dir" --parallel "$JOBS"

    file "$build_dir/Peare" | grep -q 'ARM aarch64' || {
        echo "Il binario ARM64 prodotto ha un'architettura inattesa." >&2
        file "$build_dir/Peare" >&2
        exit 1
    }
    file "$build_dir/Peare"
}

run_target() {
    local arch="$1" executable qt_root
    executable="$ROOT_DIR/build-linux-$arch/Peare"
    [[ -x "$executable" ]] || { echo "Eseguibile mancante: $executable" >&2; exit 1; }

    if [[ "$arch" == arm64 && "$host_arch" != arm64 ]]; then
        echo "Build ARM64 completata: $executable" >&2
        echo "L'esecuzione richiede un sistema Linux ARM64." >&2
        exit 1
    fi

    case "$arch" in
        x64) qt_root="$QT_X64_ROOT" ;;
        arm64) qt_root="$QT_ARM64_ROOT" ;;
    esac
    LD_LIBRARY_PATH="$qt_root/lib:$ROOT_DIR/build-linux-$arch${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    QT_PLUGIN_PATH="$qt_root/plugins" \
    QT_QPA_PLATFORM_PLUGIN_PATH="$qt_root/plugins/platforms" \
        "$executable"
}

case "$target_arch" in
    x64) configure_and_build_x64 ;;
    arm64) configure_and_build_arm64 ;;
    all)
        configure_and_build_x64
        configure_and_build_arm64
        ;;
esac

if (( run )); then
    run_target "$target_arch"
fi
