#!/usr/bin/env bash
set -Eeuo pipefail

QT_VERSION="5.15.2"
QT_SHA256="909fad2591ee367993a75d7e2ea50ad4db332f05e1c38dd7a5a274e156a4e0f8"
QT_ARCHIVE="qtbase-everywhere-src-${QT_VERSION}.tar.xz"
QT_URL="https://download.qt.io/archive/qt/5.15/${QT_VERSION}/submodules/${QT_ARCHIVE}"
QT_INSTALL_BASE="${PEARE_QT_INSTALL_BASE:-$HOME/.local/opt/Qt/${QT_VERSION}}"
QT_CACHE_BASE="${PEARE_QT_CACHE_BASE:-$HOME/.cache/peare/qt-${QT_VERSION}}"
QT_SOURCE_DIR="$QT_CACHE_BASE/source"
QT_ARCHIVE_PATH="$QT_CACHE_BASE/$QT_ARCHIVE"
ARM64_SYSROOT="${PEARE_ARM64_SYSROOT:-$HOME/.cache/peare/sysroots/ubuntu-focal-arm64}"
ARM64_APT_ROOT="$QT_CACHE_BASE/apt-focal-arm64"

cpu_count="$(nproc)"
if (( cpu_count > 4 )); then
    default_qt_jobs=4
else
    default_qt_jobs="$cpu_count"
fi
QT_BUILD_JOBS="${QT_BUILD_JOBS:-$default_qt_jobs}"

usage() {
    cat <<'USAGE'
Uso: cmake/linux/build_qt515.sh --arch x64|arm64|all [--clean]

Compila Qt Base 5.15.2 senza account Qt, Docker o emulazione.
La build ARM64 usa GCC cross e un sysroot Ubuntu 20.04 ARM64 locale.

Installazione predefinita:
  ~/.local/opt/Qt/5.15.2/gcc_64
  ~/.local/opt/Qt/5.15.2/gcc_arm64
  ~/.local/opt/Qt/5.15.2/gcc_arm64_host

Variabili:
  PEARE_QT_INSTALL_BASE  directory di installazione Qt
  PEARE_QT_CACHE_BASE    sorgenti e directory di compilazione
  PEARE_ARM64_SYSROOT    sysroot Ubuntu Focal ARM64
  QT_BUILD_JOBS          parallelismo della compilazione Qt
USAGE
}

arch=""
clean=0
while (($#)); do
    case "$1" in
        --arch)
            (($# >= 2)) || { echo "--arch richiede un valore" >&2; exit 2; }
            case "$2" in
                x64|amd64|x86_64) arch=x64 ;;
                arm64|aarch64) arch=arm64 ;;
                all) arch=all ;;
                *) echo "Architettura non supportata: $2" >&2; exit 2 ;;
            esac
            shift 2
            ;;
        --clean) clean=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Argomento sconosciuto: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ -n "$arch" ]] || { usage >&2; exit 2; }

for command_name in apt-get curl dpkg-deb file make perl pkg-config sha256sum tar; do
    command -v "$command_name" >/dev/null 2>&1 || {
        echo "Tool mancante: $command_name. Esegui ./setup_linux_host.sh" >&2
        exit 1
    }
done

mkdir -p "$QT_CACHE_BASE" "$QT_INSTALL_BASE"

prepare_source() {
    if [[ ! -f "$QT_ARCHIVE_PATH" ]]; then
        curl --fail --location --retry 5 --retry-delay 2 \
            --output "$QT_ARCHIVE_PATH.part" "$QT_URL"
        mv "$QT_ARCHIVE_PATH.part" "$QT_ARCHIVE_PATH"
    fi

    echo "$QT_SHA256  $QT_ARCHIVE_PATH" | sha256sum -c -

    if [[ ! -x "$QT_SOURCE_DIR/configure" ]]; then
        rm -rf "$QT_SOURCE_DIR"
        mkdir -p "$QT_SOURCE_DIR"
        tar -xJf "$QT_ARCHIVE_PATH" -C "$QT_SOURCE_DIR" --strip-components=1
    fi
}

arm64_packages=(
    libc6-dev
    libstdc++-9-dev
    libdbus-1-dev
    libfontconfig1-dev
    libfreetype6-dev
    libgl1-mesa-dev
    libglu1-mesa-dev
    libice-dev
    libsm-dev
    libx11-dev
    libx11-xcb-dev
    libxcb-glx0-dev
    libxcb-icccm4-dev
    libxcb-image0-dev
    libxcb-keysyms1-dev
    libxcb-randr0-dev
    libxcb-render-util0-dev
    libxcb-render0-dev
    libxcb-shape0-dev
    libxcb-shm0-dev
    libxcb-sync-dev
    libxcb-util-dev
    libxcb-xfixes0-dev
    libxcb-xinerama0-dev
    libxcb-xinput-dev
    libxcb-xkb-dev
    libxcb1-dev
    libxext-dev
    libxfixes-dev
    libxi-dev
    libxkbcommon-dev
    libxkbcommon-x11-dev
    libxrender-dev
    zlib1g-dev
)

prepare_arm64_sysroot() {
    local stamp="$ARM64_SYSROOT/.peare-focal-arm64-sysroot"
    local package_signature
    package_signature="$( { printf '%s\n' "${arm64_packages[@]}"; printf '%s\n' 'sysroot-layout-v3'; } | sha256sum | cut -d' ' -f1)"

    if [[ -f "$stamp" ]] && grep -qx "$package_signature" "$stamp"; then
        return
    fi

    # Preserve downloaded .deb files across retries. Only reset APT state and
    # the extracted sysroot; deleting ARM64_APT_ROOT here caused apt to
    # redownload every package and discarded manually recovered archives.
    rm -rf "$ARM64_APT_ROOT/state" "$ARM64_SYSROOT"
    mkdir -p \
        "$ARM64_APT_ROOT/state/lists/partial" \
        "$ARM64_APT_ROOT/cache/archives/partial" \
        "$ARM64_SYSROOT"
    : > "$ARM64_APT_ROOT/state/status"

    cat > "$ARM64_APT_ROOT/sources.list" <<'SOURCES'
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports focal main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports focal-updates main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports focal-security main restricted universe multiverse
SOURCES

    local apt_user
    apt_user="$(id -un)"
    local -a apt_options=(
        -o "APT::Architecture=arm64"
        -o "APT::Architectures::=arm64"
        -o "APT::Sandbox::User=$apt_user"
        -o "Debug::NoLocking=true"
        -o "Dir::State=$ARM64_APT_ROOT/state"
        -o "Dir::State::status=$ARM64_APT_ROOT/state/status"
        -o "Dir::Cache=$ARM64_APT_ROOT/cache"
        -o "Dir::Cache::archives=$ARM64_APT_ROOT/cache/archives"
        -o "Dir::Etc::sourcelist=$ARM64_APT_ROOT/sources.list"
        -o "Dir::Etc::sourceparts=-"
        -o "APT::Get::List-Cleanup=0"
    )

    apt-get "${apt_options[@]}" update
    apt-get "${apt_options[@]}" \
        --download-only --yes --no-install-recommends \
        install "${arm64_packages[@]}"

    local deb count=0
    while IFS= read -r -d '' deb; do
        dpkg-deb -x "$deb" "$ARM64_SYSROOT"
        ((count += 1))
    done < <(find "$ARM64_APT_ROOT/cache/archives" -maxdepth 1 -type f -name '*.deb' -print0 | sort -z)

    (( count > 0 )) || {
        echo "Nessun pacchetto ARM64 scaricato per il sysroot." >&2
        exit 1
    }

    [[ -f "$ARM64_SYSROOT/usr/lib/aarch64-linux-gnu/libX11.so" ]] || {
        echo "Sysroot ARM64 incompleto: libX11.so assente." >&2
        exit 1
    }
    [[ -f "$ARM64_SYSROOT/usr/include/stdlib.h" ]] || {
        echo "Sysroot ARM64 incompleto: header libc assenti." >&2
        exit 1
    }

    # I pacchetti libc6-dev estratti fuori da dpkg possono non creare i linker
    # stub .so normalmente predisposti durante l'installazione. Senza questi,
    # -lpthread/-ldl/-lrt ricadono sugli archivi statici e il link di Qt fallisce.
    # Versioni SONAME glibc su Ubuntu Focal ARM64:
    # pthread -> .so.0, dl -> .so.2, rt -> .so.1.
    # Usare .so.0 per tutte faceva ricadere -ldl/-lrt sugli archivi statici.
    local glibc_lib glibc_soname dev_link runtime_lib
    while read -r glibc_lib glibc_soname; do
        dev_link="$ARM64_SYSROOT/usr/lib/aarch64-linux-gnu/lib${glibc_lib}.so"
        runtime_lib="$ARM64_SYSROOT/lib/aarch64-linux-gnu/lib${glibc_lib}.so.${glibc_soname}"
        [[ -e "$runtime_lib" ]] || {
            echo "Sysroot ARM64 incompleto: $runtime_lib assente." >&2
            exit 1
        }
        rm -f "$dev_link"
        ln -s "../../../lib/aarch64-linux-gnu/lib${glibc_lib}.so.${glibc_soname}" "$dev_link"
    done <<'EOF_GLIBC_SONAMES'
pthread 0
dl 2
rt 1
EOF_GLIBC_SONAMES

    for glibc_lib in pthread dl rt; do
        [[ -L "$ARM64_SYSROOT/usr/lib/aarch64-linux-gnu/lib${glibc_lib}.so" ]] || {
            echo "Sysroot ARM64 incompleto: lib${glibc_lib}.so condivisa assente." >&2
            exit 1
        }
    done

    printf '%s\n' "$package_signature" > "$stamp"
}

qt_x64_valid() {
    local prefix="$QT_INSTALL_BASE/gcc_64"
    [[ -x "$prefix/bin/qmake" ]] && \
        [[ "$($prefix/bin/qmake -query QT_VERSION 2>/dev/null || true)" == "$QT_VERSION" ]] && \
        file "$prefix/lib/libQt5Core.so.${QT_VERSION}" 2>/dev/null | grep -q 'x86-64'
}

qt_arm64_valid() {
    local prefix="$QT_INSTALL_BASE/gcc_arm64"
    local host_prefix="$QT_INSTALL_BASE/gcc_arm64_host"
    [[ -f "$prefix/lib/libQt5Core.so.${QT_VERSION}" ]] && \
        file "$prefix/lib/libQt5Core.so.${QT_VERSION}" | grep -q 'ARM aarch64' && \
        [[ -x "$host_prefix/bin/moc" ]] && \
        file "$host_prefix/bin/moc" | grep -q 'x86-64'
}

configure_common=(
    -opensource
    -confirm-license
    -release
    -shared
    -nomake examples
    -nomake tests
    -opengl desktop
    -xcb
    -no-pch
    -no-use-gold-linker
)

build_x64() {
    local prefix="$QT_INSTALL_BASE/gcc_64"
    local build_dir="$QT_CACHE_BASE/build-x64"

    if (( clean )); then
        rm -rf "$build_dir" "$prefix"
    elif qt_x64_valid; then
        echo "Qt ${QT_VERSION} x64 gia' presente: $prefix"
        return
    fi

    prepare_source
    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    unset QMAKESPEC QMAKEPATH QT_SELECT CMAKE_PREFIX_PATH PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
    (
        cd "$build_dir"
        "$QT_SOURCE_DIR/configure" \
            -prefix "$prefix" \
            "${configure_common[@]}"
        make -j"$QT_BUILD_JOBS"
        make install
    )

    qt_x64_valid || {
        echo "Installazione Qt x64 non valida: $prefix" >&2
        exit 1
    }
}

build_arm64() {
    local prefix="$QT_INSTALL_BASE/gcc_arm64"
    local host_prefix="$QT_INSTALL_BASE/gcc_arm64_host"
    local target_prefix="/opt/Qt/${QT_VERSION}/gcc_arm64"
    local build_dir="$QT_CACHE_BASE/build-arm64"

    command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 || {
        echo "Cross-compiler ARM64 mancante. Esegui ./setup_linux_host.sh" >&2
        exit 1
    }

    prepare_arm64_sysroot

    if (( clean )); then
        rm -rf "$build_dir" "$prefix" "$host_prefix"
    elif qt_arm64_valid; then
        echo "Qt ${QT_VERSION} ARM64 gia' presente: $prefix"
        return
    fi

    prepare_source
    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    unset QMAKESPEC QMAKEPATH QT_SELECT CMAKE_PREFIX_PATH PKG_CONFIG_PATH
    export PKG_CONFIG_DIR=""
    export PKG_CONFIG_LIBDIR="$ARM64_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$ARM64_SYSROOT/usr/share/pkgconfig"
    export PKG_CONFIG_SYSROOT_DIR="$ARM64_SYSROOT"

    (
        cd "$build_dir"
        "$QT_SOURCE_DIR/configure" \
            -prefix "$target_prefix" \
            -extprefix "$prefix" \
            -hostprefix "$host_prefix" \
            -xplatform linux-aarch64-gnu-g++ \
            -sysroot "$ARM64_SYSROOT" \
            -pkg-config \
            "${configure_common[@]}"
        make -j"$QT_BUILD_JOBS"
        make install
    )

    qt_arm64_valid || {
        echo "Installazione Qt ARM64 non valida: $prefix" >&2
        exit 1
    }
}

case "$arch" in
    x64) build_x64 ;;
    arm64) build_arm64 ;;
    all)
        build_x64
        build_arm64
        ;;
esac
