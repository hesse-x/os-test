#!/usr/bin/env python3
"""Audit the target Vulkan ELF/manifest closure before image creation."""

import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
SYSROOT_LIB = os.path.join(BUILD, "sysroot", "usr", "lib")
MANIFEST = os.path.join(BUILD, "image_manifest.txt")
TARGETS = [
    os.path.join(BUILD, "test_vulkan_smoke.elf"),
    os.path.join(BUILD, "libvulkan.so.1"),
    os.path.join(BUILD, "libvulkan_lvp.so"),
    os.path.join(SYSROOT_LIB, "libLLVM.so.18.1"),
]


def readelf(*args: str) -> str:
    env = os.environ.copy()
    env["LC_ALL"] = "C"
    return subprocess.check_output(["readelf", *args], text=True, env=env)


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    destinations = set()
    with open(MANIFEST, encoding="utf-8") as stream:
        for line in stream:
            if not line.strip() or line.startswith("#"):
                continue
            _, destination, _ = line.rstrip("\n").split("\t")
            destinations.add(destination)

    required_destinations = {
        "test/vulkan_smoke.elf",
        "lib/libvulkan.so", "lib/libvulkan.so.1",
        "lib/libvulkan_lvp.so", "lib/libLLVM.so.18.1",
        "usr/share/vulkan/icd.d/lvp_icd.x86_64.json",
    }
    missing = required_destinations - destinations
    if missing:
        raise SystemExit(f"Vulkan audit: image manifest missing {sorted(missing)}")

    json_path = os.path.join(BUILD, "lvp_icd.x86_64.json")
    with open(json_path, encoding="utf-8") as stream:
        icd = json.load(stream)
    if icd.get("ICD", {}).get("library_path") != "/lib/libvulkan_lvp.so":
        raise SystemExit("Vulkan audit: ICD.library_path is not fixed to /lib/libvulkan_lvp.so")

    available = {os.path.basename(path) for path in TARGETS}
    for directory in (BUILD, SYSROOT_LIB):
        available.update(name for name in os.listdir(directory)
                         if os.path.isfile(os.path.join(directory, name)))
    available.update(os.path.basename(path) for path in destinations if path.startswith("lib/"))
    forbidden = {"libstdc++.so", "libstdc++.so.6", "libgcc_s.so.1", "libc.so.6"}
    allowed_relocations = {
        "R_X86_64_64", "R_X86_64_COPY", "R_X86_64_DTPMOD64",
        "R_X86_64_DTPOFF64", "R_X86_64_GLOB_DAT", "R_X86_64_IRELATIVE",
        "R_X86_64_JUMP_SLOT", "R_X86_64_RELATIVE", "R_X86_64_TLSDESC",
        "R_X86_64_TPOFF64",
    }
    for target in TARGETS:
        if not os.path.isfile(target):
            raise SystemExit(f"Vulkan audit: missing regular file {target}")
        header = readelf("-hW", target)
        if "Class:                             ELF64" not in header or \
           "Machine:                           Advanced Micro Devices X86-64" not in header:
            raise SystemExit(f"Vulkan audit: wrong ELF ABI: {target}")
        dynamic = readelf("-dW", target)
        needed = re.findall(r"\(NEEDED\).*?\[([^]]+)\]", dynamic)
        bad = forbidden.intersection(needed)
        if bad:
            raise SystemExit(f"Vulkan audit: forbidden dependency in {target}: {sorted(bad)}")
        unresolved = [name for name in needed if name not in available]
        if unresolved:
            raise SystemExit(f"Vulkan audit: unresolved DT_NEEDED in {target}: {unresolved}")
        relocations = set(re.findall(r"\b(R_X86_64_[A-Z0-9_]+)\b", readelf("-rW", target)))
        unsupported = relocations - allowed_relocations
        if unsupported:
            raise SystemExit(f"Vulkan audit: unsupported relocations in {target}: {sorted(unsupported)}")

    smoke_program_headers = readelf("-lW", TARGETS[0])
    if "/lib/ld-musl-x86_64.so.1" not in smoke_program_headers:
        raise SystemExit("Vulkan audit: smoke interpreter is not target musl")
    if sha256(os.path.join(BUILD, "libvulkan.so")) != sha256(os.path.join(BUILD, "libvulkan.so.1")):
        raise SystemExit("Vulkan audit: FAT32 Loader aliases are not identical real copies")
    print("Vulkan ELF/manifest closure audit: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
