#!/usr/bin/env python3
"""Expose target LLVM metadata to Meson without executing target binaries."""

import os
import subprocess
import sys


def native_query(native: str, option: str) -> str:
    return subprocess.check_output([native, option], text=True).strip()


def main() -> int:
    default_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sysroot = os.environ.get("XOS_LLVM_SYSROOT", os.path.join(default_root, "build", "sysroot"))
    native = os.environ.get("XOS_NATIVE_LLVM_CONFIG", "/usr/lib/llvm-18/bin/llvm-config")
    if not sysroot or not os.path.isabs(sysroot):
        print("llvm-config-target: XOS_LLVM_SYSROOT must be absolute", file=sys.stderr)
        return 2

    include_dir = os.path.join(sysroot, "usr", "include")
    lib_dir = os.path.join(sysroot, "usr", "lib")
    allowed_flags = {
        "--version", "--components", "--libs", "--libnames", "--libfiles",
        "--libdir", "--includedir", "--cppflags", "--cxxflags", "--ldflags",
        "--system-libs", "--shared-mode", "--has-rtti", "--link-shared",
    }
    unknown = [arg for arg in sys.argv[1:] if arg.startswith("-") and arg not in allowed_flags]
    if unknown:
        print(f"llvm-config-target: unsupported option: {unknown[0]}", file=sys.stderr)
        return 2

    modules = [arg for arg in sys.argv[1:] if not arg.startswith("-")]
    args = set(sys.argv[1:])
    output = []
    if "--version" in args:
        output.append(native_query(native, "--version"))
    if "--components" in args:
        output.append(native_query(native, "--components"))
    if "--includedir" in args:
        output.append(include_dir)
    if "--libdir" in args:
        output.append(lib_dir)
    if "--cppflags" in args or "--cxxflags" in args:
        output.extend([f"-I{include_dir}", "-stdlib=libc++"])
    if "--ldflags" in args:
        output.append(f"-L{lib_dir}")
    if "--libs" in args:
        output.append("-lLLVM")
    if "--libnames" in args:
        output.append("libLLVM.so.18.1")
    if "--libfiles" in args:
        output.append(os.path.join(lib_dir, "libLLVM.so.18.1"))
    if "--shared-mode" in args:
        output.append("shared")
    if "--has-rtti" in args:
        cache = os.path.join(default_root, "build", "llvm-target", "CMakeCache.txt")
        rtti = None
        try:
            with open(cache, encoding="utf-8") as stream:
                for line in stream:
                    if line.startswith("LLVM_ENABLE_RTTI:BOOL="):
                        rtti = line.rstrip().split("=", 1)[1]
                        break
        except FileNotFoundError:
            pass
        if rtti not in {"ON", "OFF"}:
            print("llvm-config-target: target LLVM RTTI setting unavailable", file=sys.stderr)
            return 2
        output.append("YES" if rtti == "ON" else "NO")
    # The monolithic target DSO already carries its platform dependencies.
    if "--system-libs" in args:
        pass

    if not output and not ({"--link-shared"} & args) and not modules:
        print("llvm-config-target: no query supplied", file=sys.stderr)
        return 2
    print(" ".join(output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
