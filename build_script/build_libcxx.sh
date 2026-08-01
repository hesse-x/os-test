#!/bin/bash
# build_libcxx.sh — opt-in 单独编译 libc++ (LLVM runtimes 子构建) 装进 build/sysroot。
#
# 默认 ./build.sh 不调用本脚本；--cxx（或手动 bash build_script/build_libcxx.sh）才编。
# 已装好则秒过不重编；缺前置则明确报错引导，不产出难懂的 cmake 链接错误。
#
# 详见 refact_cmake.md（libc++ 纳入可复现的 opt-in 单独编译，定稿方案）。
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$SRC/build}"
SYSROOT="${SYSROOT:-$BUILD/sysroot}"
LIBCXX_BUILD="$BUILD/libcxx-build"
# 探测锚点：sysroot 里的 libc++.so（symlink→libc++.so.1→libc++.so.1.0）+ 头树。
# 双条件才认定“完整已装”——单看 .so 可能漏头、单看头可能漏库。
LIBCXX_SO="$SYSROOT/usr/lib/libc++.so"
LIBCXX_HEADERS="$SYSROOT/usr/include/c++/v1"

# ---- 0. 探测：已构建则秒过（不重复构建）----
if [ -e "$LIBCXX_SO" ] && [ -d "$LIBCXX_HEADERS" ] && \
   readelf -d "$LIBCXX_SO" 2>/dev/null | grep -Fq "Shared library: [libclang_rt.so]"; then
  echo "libc++ already installed at $SYSROOT with libclang_rt.so — nothing to do."
  exit 0
fi

if [ -e "$LIBCXX_SO" ] || [ -d "$LIBCXX_HEADERS" ]; then
  echo "libc++ installation lacks libclang_rt.so dependency — rebuilding."
fi

# ---- 1. 前置校验：sysroot 必须就绪（crt + stub + 头 + libc.so 导出符号）----
# 这些正是 install-libs.sh 的 mandatory 块 + install-headers.sh 的产物，也是
# cpp_worklist §已修 里 libc++ 编译踩过的所有缺口。提前校验把“编译中报 undefined”
# 转化为“入口报缺项 + 引导”。
check_sysroot() {
  local miss=0
  # crt（install-libs.sh 从 build/musl/lib 拷来）
  for o in crt1.o Scrt1.o crti.o crtn.o; do
    [ -f "$SYSROOT/usr/lib/$o" ] || { echo "FAIL: $SYSROOT/usr/lib/$o missing — run ./build.sh first." >&2; miss=1; }
  done
  # libc（fused libc.so 既是库也是 interpreter）和 compiler-rt int128 runtime。
  [ -f "$SYSROOT/usr/lib/libc.so" ] || { echo "FAIL: libc.so missing — run ./build.sh first." >&2; miss=1; }
  [ -f "$SYSROOT/usr/lib/libclang_rt.so" ] || { echo "FAIL: libclang_rt.so missing — run ./build.sh first." >&2; miss=1; }
  [ -f "$SYSROOT/usr/lib/ld-musl-x86_64.so.1" ] || { echo "FAIL: ld-musl interpreter missing — run ./build.sh first." >&2; miss=1; }
  # stub .so（musl 折进 libc 的 5 个 INPUT(libc.so) 脚本）
  for s in librt libdl libpthread libresolv libxnet; do
    [ -f "$SYSROOT/usr/lib/$s.so" ] || { echo "FAIL: stub $s.so missing — run ./build.sh first." >&2; miss=1; }
  done
  # 头树（install-headers.sh 产，含 libc++ 要的 link.h/elf.h/nl_types.h/langinfo.h/
  # sys/syscall.h/linux/futex.h）
  [ -d "$SYSROOT/usr/include" ] || { echo "FAIL: $SYSROOT/usr/include missing — run install-headers.sh (via ./build.sh)." >&2; miss=1; }
  if [ "$miss" -ne 0 ]; then
    echo "Sysroot not ready. Run ./build.sh (default flow) before --cxx." >&2
    exit 1
  fi
}
check_sysroot

# ---- 2. 前置校验：clang-18 + llvm-project 子模块就绪 ----
# runtimes 子构建必须 clang-18（release/18.x 子模块的 libcxx 源对齐 clang-18）。
command -v clang++ >/dev/null 2>&1 || { echo "FAIL: clang++ not found (runtimes needs clang-18)." >&2; exit 1; }
[ -d "$SRC/third_party/llvm-project/runtimes" ] || {
  echo "FAIL: llvm-project runtimes/ missing — run:" >&2
  echo "  git submodule update --init third_party/llvm-project" >&2
  exit 1
}

# ---- 3. 配置 + 编译 + 装回 sysroot ----
# 参数同 cpp_worklist.md “构建复现”段已验证编通那套（非脑补）：
#   -DLLVM_ENABLE_RUNTIMES           三件套一起编
#   -DCMAKE_SYSROOT + 各 --sysroot    交叉 sysroot 隔离（memory [[mesa-cross-sysroot-isolation]]）
#   -nodefaultlibs -lc               去隐式 -lc 后补回（Scrt1.o _start 调 __libc_start_main）
#   -nostdinc++                       禁用编译器自带 C++ 头，只用 sysroot 的 c++/v1
#   -DLIBCXX_HAS_MUSL_LIBC=ON         走默认 rune table（musl 不在 libc++ 平台名单）
#   -DCMAKE_INSTALL_PREFIX=sysroot/usr 装回 sysroot（非 /usr/local）
echo "Configuring libc++ runtimes (cmake -S runtimes) → $LIBCXX_BUILD"
cmake -G Ninja -S "$SRC/third_party/llvm-project/runtimes" -B "$LIBCXX_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_AR=/usr/bin/ar -DCMAKE_RANLIB=/usr/bin/ranlib \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
  -DCMAKE_INSTALL_PREFIX="$SYSROOT/usr" -DCMAKE_SYSROOT="$SYSROOT" \
  -DCMAKE_C_FLAGS="--sysroot=$SYSROOT -nodefaultlibs -I$SYSROOT/usr/include" \
  -DCMAKE_CXX_FLAGS="--sysroot=$SYSROOT -nodefaultlibs -I$SYSROOT/usr/include -nostdinc++" \
  -DCMAKE_EXE_LINKER_FLAGS="--sysroot=$SYSROOT -nodefaultlibs -lc" \
  -DCMAKE_SHARED_LINKER_FLAGS="--sysroot=$SYSROOT -nodefaultlibs -Wl,--no-as-needed -lclang_rt -Wl,--as-needed" \
  -DLIBCXX_HAS_MUSL_LIBC=ON \
  -DLIBCXX_ENABLE_SHARED=ON -DLIBCXX_ENABLE_STATIC=ON \
  -DLIBCXXABI_ENABLE_SHARED=ON -DLIBCXXABI_ENABLE_STATIC=ON \
  -DLIBUNWIND_ENABLE_SHARED=ON -DLIBUNWIND_ENABLE_STATIC=ON \
  -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF

echo "Building libc++ (ninja) → $LIBCXX_BUILD"
ninja -C "$LIBCXX_BUILD"

echo "Installing libc++ → $SYSROOT/usr/{lib,include/c++/v1}"
ninja -C "$LIBCXX_BUILD" install
# 产物: $SYSROOT/usr/lib/{libc++,libc++abi,libunwind}.so{,.1,.1.0} + .a
#       $SYSROOT/usr/include/c++/v1/*  (含 __config_site / module.modulemap)

echo "Done. libc++ installed at $SYSROOT (re-run ./build.sh [--cxx] to ship into disk.img)."
