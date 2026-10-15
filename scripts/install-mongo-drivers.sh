#!/usr/bin/env bash
set -euo pipefail

# 仅安装到显式指定的前缀；工作目录由本脚本独占，退出时只清理它。
prefix="${1:?请指定驱动安装前缀的绝对路径}"
case "$prefix" in /*) ;; *) printf '%s\n' '安装前缀必须为绝对路径' >&2; exit 1 ;; esac
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/cpp-http-drivers.XXXXXXXX")"
trap 'rm -rf -- "$work_dir"' EXIT
c_version=1.25.4
cxx_version=3.9.0

curl --fail --location --retry 3 "https://github.com/mongodb/mongo-c-driver/releases/download/${c_version}/mongo-c-driver-${c_version}.tar.gz" -o "$work_dir/c.tar.gz"
tar -xzf "$work_dir/c.tar.gz" -C "$work_dir"
cmake -S "$work_dir/mongo-c-driver-${c_version}" -B "$work_dir/c-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF
cmake --build "$work_dir/c-build" --parallel 2
cmake --install "$work_dir/c-build"

curl --fail --location --retry 3 "https://github.com/mongodb/mongo-cxx-driver/releases/download/r${cxx_version}/mongo-cxx-driver-r${cxx_version}.tar.gz" -o "$work_dir/cxx.tar.gz"
tar -xzf "$work_dir/cxx.tar.gz" -C "$work_dir"
cmake -S "$work_dir/mongo-cxx-driver-r${cxx_version}" -B "$work_dir/cxx-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_CXX_STANDARD=17 -DBUILD_VERSION="$cxx_version" \
    -DBUILD_TESTING=OFF -DENABLE_TESTS=OFF -DBUILD_SHARED_LIBS=ON
cmake --build "$work_dir/cxx-build" --parallel 2
cmake --install "$work_dir/cxx-build"
PKG_CONFIG_PATH="$prefix/lib/pkgconfig" pkg-config --modversion libmongoc-1.0 libmongocxx libbsoncxx
