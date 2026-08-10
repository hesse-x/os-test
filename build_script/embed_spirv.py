#!/usr/bin/env python3
"""Embed a validated SPIR-V module as an aligned uint32_t C++ header."""

import pathlib
import struct
import sys


def main() -> None:
    source = pathlib.Path(sys.argv[1])
    destination = pathlib.Path(sys.argv[2])
    symbol = sys.argv[3]
    data = source.read_bytes()
    if not data or len(data) % 4:
        raise SystemExit(f"invalid SPIR-V size: {source}: {len(data)}")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != 0x07230203:
        raise SystemExit(f"invalid SPIR-V magic: {source}")
    rows = []
    for offset in range(0, len(words), 8):
        rows.append("    " + ", ".join(f"0x{word:08x}u" for word in words[offset:offset + 8]))
    destination.write_text(
        "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n"
        f"alignas(4) static const uint32_t {symbol}[] = {{\n"
        + ",\n".join(rows)
        + "\n};\n"
        f"static const size_t {symbol}_word_count = sizeof({symbol}) / sizeof({symbol}[0]);\n",
        encoding="ascii",
    )


if __name__ == "__main__":
    main()
