#!/usr/bin/env bash
# Installs the C++ dependencies on Ubuntu 24.04 WITHOUT Conan, for use with
#   cmake -B build -DGTPROXY_USE_CONAN=OFF -DCMAKE_PREFIX_PATH=$PREFIX
# This is the path used to build and test the fork in a sandbox where Conan
# Center was unreachable. Windows/macOS use Conan (see README).
#
# usage: scripts/dev/linux-deps.sh [prefix]     (default prefix: $HOME/.gtproxy-deps)
set -euo pipefail
PREFIX="${1:-$HOME/.gtproxy-deps}"
SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo"

$SUDO apt-get update -qq
$SUDO apt-get install -y -qq build-essential cmake pkg-config git \
    libspdlog-dev libfmt-dev libgtest-dev libgmock-dev liblua5.4-dev libglm-dev \
    libpcg-cpp-dev libssl-dev zlib1g-dev

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
# Header-only libraries that Ubuntu does not package, at the versions conanfile.py resolves to.
clone() { git clone -q --depth 1 -b "$2" "https://github.com/$1" "$work/$(basename "$1")"; }
clone wqking/eventpp v0.1.3
clone stephenberry/glaze v6.2.0
clone Neargye/magic_enum v0.9.7
clone ThePhD/sol2 v3.5.0

for d in eventpp glaze magic_enum sol2; do
  cmake -S "$work/$d" -B "$work/b_$d" -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBUILD_TESTING=OFF \
        -DEVENTPP_BUILD_TESTS=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF \
        -Dglaze_DEVELOPER_MODE=OFF -DSOL2_BUILD_LUA=OFF -DSOL2_TESTS=OFF >/dev/null
  cmake --install "$work/b_$d" >/dev/null
done
echo "Dependencies installed. Configure with:"
echo "  cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGTPROXY_USE_CONAN=OFF -DCMAKE_PREFIX_PATH=$PREFIX"
