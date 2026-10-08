#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>

"""
mkfont.py - generate the UI framework's bitmap font tables (UI/cgl_font_data.c)

Rasterises a freely redistributable TrueType font into fixed-cell bitmap glyphs
and writes exactly the format `src/core/cgl_font.c` reads:

    byte index = row * bytes_per_row + (column / 8)
    bit  index = column % 8        (bit0 = leftmost pixel, i.e. LSB on the left)

The default font is DejaVu Sans Mono (Bitstream Vera license, redistribution and
modified derivatives allowed), installed locally together with matplotlib, and
the script finds it automatically; --font picks a different one.

Usage
-----
    # generate both 8x16 and 12x24 (default output UI/cgl_font_data.c)
    python tools/mkfont.py

    # generate 8x16 only, with another font and a custom threshold
    python tools/mkfont.py --sizes 8x16 --font C:/path/to/some-mono.ttf --threshold 45

    # preview without writing any file
    python tools/mkfont.py --dry-run

NOTE: the font license
----
The rasterised bitmap is a derivative work of the font, so the original font's
license notice must ship with the source. The script copies the full license to
--license-out (default licenses/LICENSE_DEJAVU.txt) and states the provenance in the
header of the generated file. If you switch to another font, check for yourself
that its license allows redistribution."""

import argparse
import os
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover
    sys.stderr.write("Pillow is required: pip install pillow\n")
    raise SystemExit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FIRST_CHAR = 0x20
LAST_CHAR = 0x7E
GLYPH_COUNT = LAST_CHAR - FIRST_CHAR + 1

# DejaVu Sans Mono font metrics (em normalised); used to derive a suitable render size
ADVANCE_EM = 0.60205
ASCENT_EM = 0.75970
DESCENT_EM = 0.24020

SUPERSAMPLE = 4  # Render large first, then downsample by area averaging, so thin strokes do not break up


def find_font(explicit):
    if explicit:
        if not os.path.exists(explicit):
            raise SystemExit("font not found: %s" % explicit)
        return explicit
    cands = []
    try:
        import matplotlib
        d = os.path.join(os.path.dirname(matplotlib.__file__), "mpl-data", "fonts", "ttf")
        cands += [os.path.join(d, n) for n in
                  ("DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf")]
    except ImportError:
        pass
    cands += [
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "C:/Windows/Fonts/DejaVuSansMono.ttf",
    ]
    for p in cands:
        if os.path.exists(p):
            return p
    raise SystemExit("DejaVu Sans Mono not found; use --font to point at a freely redistributable monospace font")


def find_license(font_path):
    """Look for the license file next to the font (matplotlib keeps LICENSE_DEJAVU in the same directory)."""
    d = os.path.dirname(font_path)
    for name in ("LICENSE_DEJAVU", "LICENSE", "LICENSE.txt", "COPYING"):
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None


def pick_em(cell_w, cell_h):
    """Pick a render size at which both the advance width and the height fit inside the cell."""
    em = min(cell_w / ADVANCE_EM, cell_h / (ASCENT_EM + DESCENT_EM))
    em = int(em)
    while em > 4:
        f = ImageFont.truetype(FONT_PATH, em)
        adv = f.getlength("M")
        asc, desc = f.getmetrics()
        if adv <= cell_w and (asc + desc) <= cell_h:
            return em
        em -= 1
    return em


def render_glyph(ch, cell_w, cell_h, em):
    """Render one character -> a cell_w x cell_h coverage matrix (0..255)."""
    big = SUPERSAMPLE
    font = ImageFont.truetype(FONT_PATH, em * big)
    adv = font.getlength("M")
    asc, desc = font.getmetrics()

    img = Image.new("L", (cell_w * big, cell_h * big), 0)
    d = ImageDraw.Draw(img)
    # Everything is computed in "upscaled" coordinates: centre the ascender+descender em box vertically in the cell
    x = (cell_w * big - adv) / 2.0
    y = (cell_h * big - (asc + desc)) // 2
    d.text((x, y), ch, font=font, fill=255)

    return img.resize((cell_w, cell_h), Image.BOX).load()


def build_face(cell_w, cell_h, threshold):
    em = pick_em(cell_w, cell_h)
    if em < 6:
        raise SystemExit("cell %dx%d is too small to lay out" % (cell_w, cell_h))

    row_bytes = (cell_w + 7) // 8
    glyphs = []
    for code in range(FIRST_CHAR, LAST_CHAR + 1):
        ch = chr(code)
        if ch == " ":
            glyphs.append(bytes(row_bytes * cell_h))
            continue
        px = render_glyph(ch, cell_w, cell_h, em)
        buf = bytearray(row_bytes * cell_h)
        for y in range(cell_h):
            for x in range(cell_w):
                if px[x, y] >= threshold:
                    buf[y * row_bytes + (x >> 3)] |= (1 << (x & 7))   # LSB = left
        glyphs.append(bytes(buf))
    return em, row_bytes, glyphs


def preview(glyphs, cell_w, cell_h, text, indent="    "):
    row_bytes = (cell_w + 7) // 8
    out = []
    for y in range(cell_h):
        line = indent
        for ch in text:
            g = glyphs[ord(ch) - FIRST_CHAR]
            for x in range(cell_w):
                on = (g[y * row_bytes + (x >> 3)] >> (x & 7)) & 1
                line += "#" if on else "."
        out.append(line)
    return "\n".join(out)


def emit(faces, font_path, license_rel, out_path):
    lines = []
    lines.append("// SPDX-License-Identifier: Apache-2.0 AND Bitstream-Vera")
    lines.append("// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>")
    lines.append("/**")
    lines.append("  ******************************************************************************")
    lines.append("  * @file    %s" % os.path.basename(out_path))
    lines.append("  * @brief   Dot matrix UI fonts (printable ASCII 0x20..0x7E).")
    lines.append("  *")
    lines.append("  *  AUTO GENERATED by tools/mkfont.py - do not edit by hand.")
    lines.append("  *")
    lines.append("  *  The table layout and the generator are Apache-2.0. The glyph")
    lines.append("  *  bitmaps themselves are a rasterisation of the source font below, so")
    lines.append("  *  they stay under that font's own licence.")
    lines.append("  *")
    lines.append("  *  Source font : %s" % os.path.basename(font_path))
    lines.append("  *  Font licence: Bitstream Vera / DejaVu - full text in %s" % license_rel)
    lines.append("  *  Regenerate  : python tools/mkfont.py")
    lines.append("  *")
    lines.append("  *  Layout (what cgl_font.c reads):")
    lines.append("  *      byte index = row * bytes_per_row + (column / 8)")
    lines.append("  *      bit  index = column % 8   (bit 0 = left most pixel)")
    lines.append("  ******************************************************************************")
    lines.append("  */")
    lines.append('#include <cytgl/cgl_font.h>')
    lines.append("")

    tables = []
    for (cell_w, cell_h), (em, row_bytes, glyphs) in faces.items():
        name = "font_%dx%d_glyphs" % (cell_w, cell_h)
        tables.append((cell_w, cell_h, name, row_bytes))
        lines.append("/* %d x %d, rendered at %d px em, %d bytes per glyph */"
                     % (cell_w, cell_h, em, row_bytes * cell_h))
        lines.append("static const uint8_t %s[%d][%d] = {" % (name, GLYPH_COUNT, row_bytes * cell_h))
        for idx, g in enumerate(glyphs):
            ch = chr(FIRST_CHAR + idx)
            label = "0x%02X" % (FIRST_CHAR + idx)
            if ch == "\\":
                ch = "backslash"
            lines.append("    {%s}, /* %s  %s */"
                         % (",".join("0x%02X" % b for b in g), label, ch))
        lines.append("};")
        lines.append("")

    for cell_w, cell_h, name, row_bytes in tables:
        lines.append("const cgl_font_t cgl_font_%dx%d = {" % (cell_w, cell_h))
        lines.append("    %du, %du, %du, (const uint8_t *)%s, 0x%02Xu, 0x%02Xu"
                     % (cell_w, cell_h, row_bytes * cell_h, name, FIRST_CHAR, LAST_CHAR))
        lines.append("};")
        lines.append("")

    text = "\n".join(lines).rstrip() + "\n"
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def verify(out_path, faces):
    """Read the generated file back and confirm that the glyph count and the pixel count match the expectation."""
    src = open(out_path, encoding="utf-8").read()
    ok = True
    for (cell_w, cell_h), (_em, row_bytes, glyphs) in faces.items():
        m = re.search(r"cgl_font_%dx%d = \{\s*(\d+)u, (\d+)u, (\d+)u" % (cell_w, cell_h), src)
        if not m:
            print("  verify failed: cgl_font_%dx%d definition not found" % (cell_w, cell_h)); ok = False; continue
        w, h, b = (int(x) for x in m.groups())
        if (w, h, b) != (cell_w, cell_h, row_bytes * cell_h):
            print("  verify failed: %dx%d size mismatch" % (cell_w, cell_h)); ok = False; continue
        body = re.search(r"font_%dx%d_glyphs\[\d+\]\[\d+\] = \{(.*?)\n\};" % (cell_w, cell_h), src, re.S)
        # Strip the end-of-line comments first, otherwise a "0x20" inside a comment is taken for glyph data
        body_text = re.sub(r"/\*.*?\*/", "", body.group(1), flags=re.S)
        data = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body_text)]
        if len(data) != GLYPH_COUNT * row_bytes * cell_h:
            print("  verify failed: %dx%d glyph data length mismatch" % (cell_w, cell_h)); ok = False; continue
        dots = sum(bin(v).count("1") for v in data)
        exp = sum(bin(v).count("1") for g in glyphs for v in g)
        if dots != exp:
            print("  verify failed: %dx%d set-bit count %d != %d" % (cell_w, cell_h, dots, exp)); ok = False
        else:
            print("  %-6s verify passed: %d glyphs, %d set pixels"
                  % ("%dx%d" % (cell_w, cell_h), GLYPH_COUNT, dots))
    return ok


# --------------------------------------------------------------------------
FONT_PATH = None


def main(argv=None):
    global FONT_PATH
    p = argparse.ArgumentParser(
        description="Generate the UI bitmap font tables (UI/cgl_font_data.c)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Example: python tools/mkfont.py')
    p.add_argument("--sizes", nargs="+", default=["8x16", "12x24"],
                   help="list of cell sizes, default 8x16 12x24")
    p.add_argument("--font", help="TrueType font path (default: find DejaVu Sans Mono automatically)")
    p.add_argument("--threshold", type=int, default=45,
                   help="coverage threshold %%, lower means thicker strokes (default 45)")
    p.add_argument("--out", default="src/core/cgl_font_data.c", help="output file")
    p.add_argument("--license-out", default="licenses/LICENSE_DEJAVU.txt",
                   help="where to write the full font license text")
    p.add_argument("--dry-run", action="store_true", help="preview only, write no file")
    args = p.parse_args(argv)

    FONT_PATH = find_font(args.font)
    print("font      : %s" % FONT_PATH)
    lic = find_license(FONT_PATH)
    print("license   : %s" % (lic if lic else "(not found, please supply it yourself)"))
    print()

    sizes = []
    for s in args.sizes:
        m = re.fullmatch(r"(\d+)x(\d+)", s)
        if not m:
            raise SystemExit("size format must be WxH, e.g. 8x16: %s" % s)
        sizes.append((int(m.group(1)), int(m.group(2))))

    faces = {}
    for (w, h) in sizes:
        em, row_bytes, glyphs = build_face(w, h, args.threshold * 255 // 100)
        faces[(w, h)] = (em, row_bytes, glyphs)
        print("cell %-6s em=%dpx  %d bytes per glyph" % ("%dx%d" % (w, h), em, row_bytes * h))

    sample = "CytStudio 0123 fps"
    for (w, h), (em, row_bytes, glyphs) in faces.items():
        print("\npreview %dx%d  %r" % (w, h, sample))
        print(preview(glyphs, w, h, sample))

    if args.dry_run:
        print("\n(--dry-run: no files written)")
        return 0

    out_path = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    lic_rel = os.path.basename(args.license_out)
    if lic:
        dst = args.license_out if os.path.isabs(args.license_out) else os.path.join(ROOT, args.license_out)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(lic, "r", encoding="utf-8", errors="replace") as src, \
             open(dst, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(src.read())
        print("\nlicense   -> %s" % dst)

    emit(faces, FONT_PATH, lic_rel, out_path)
    print("written   : %s" % out_path)
    print("readback  :")
    return 0 if verify(out_path, faces) else 1


if __name__ == "__main__":
    raise SystemExit(main())
