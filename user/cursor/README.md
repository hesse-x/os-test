# Cursor artwork

This directory contains the compositor's cursor images, generated from
`gen_cursors.py`.

## License: MIT

The PNG cursor images in this directory are **original artwork** drawn from
scratch by `gen_cursors.py`. They were NOT derived from any third-party cursor
pack — in particular, they do not reuse, copy, or trace the pixel data or path
data of `ful1e5/apple_cursor` (which was removed from this repository as a
GPL-3.0 submodule). The shapes are generic cursor glyphs (arrow, I-beam, hand,
resize arrows) and the white-fill + black-outline look is an uncopyrightable
visual style.

```
Copyright (c) 2026 hesse

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Regenerating

```bash
python3 user/cursor/gen_cursors.py
```

Requires Pillow. Outputs the six 32x32 PNGs plus `cursors.h` (cursor name →
image-absolute disk path + hotspot table, compiled into the compositor) and
`cursors.json` (a human-readable mirror of the same table; the build stages
the PNGs via hand-written `os_image_path` rules in
`user/compositor/CMakeLists.txt`, it does not read `cursors.json`).