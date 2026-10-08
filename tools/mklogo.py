#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>

"""
mklogo.py - generate the Logo bitmap for the dot-matrix smash animation (App/logo_bitmap.h)

Turns a piece of text or an image into the dot-matrix bitmap used by the
animation: every "dot" becomes a small physics ball in the firmware, laid out on
screen at a LOGO_STEP_Q4 spacing. The generated file is fully compatible with the
firmware interface (LOGO_GRID_W / LOGO_GRID_H / LOGO_ROW_BYTES / LOGO_DOT_COUNT /
logo_bitmap[] / logo_bitmap_get()), so not a single line of the C side changes.

Usage
----
    # text Logo (\n starts a new line)
    python tools/mklogo.py --text "CytStudio"
    python tools/mklogo.py --text "Cyt\nStudio" --align center

    # with a specific font, adjusting weight and tracking
    python tools/mklogo.py --text "CytStudio" --font C:/Windows/Fonts/impact.ttf
    python tools/mklogo.py --text "CytStudio" --tracking 4 --dot-threshold 35

    # from an image (PNG/JPG/BMP, dark background with light text inverts automatically)
    python tools/mklogo.py --image docs/logo.png --invert

    # preview only, write nothing
    python tools/mklogo.py --text "CytStudio" --dry-run

    # keep blank margins on both sides: type spaces (WYSIWYG), or pad by cells with --pad-x
    python tools/mklogo.py --text "   CytStudio   "
    python tools/mklogo.py --text "CytStudio" --pad-x 2

    # write it back into the firmware and build/flash
    python tools/mklogo.py --text "CytStudio" --out App/logo_bitmap.h

WYSIWYG
----
In text mode the **horizontal** bounds = the ink + the spaces you typed; it no
longer crops to the ink unconditionally:

* the leading/trailing spaces of `--text "  CytStudio  "` are kept: as many
  spaces as you type is as much margin as you get;
* inter-word spaces, `--tracking` spacing and trailing whitespace are preserved
  as well;
* inside `--text`, `\n` means a newline and `\\` means one backslash, so the
  `--text "Cyt\nStudio"` above can be pasted directly into cmd / PowerShell / bash;
* the glyph's own left/right side bearings (invisible font metrics) are still
  cropped, so with no spaces typed the output is **exactly the same** as the old
  version and does not silently shrink;
* multi-line text is left-aligned per line by default, and leading indentation is
  WYSIWYG too (the old version centred each line; use `--align center`).

The grid width is capped by the screen limit (106 cells by default), so padding is
"taken out of the glyphs" -- the more padding, the smaller the text. To add a
fixed margin without typing spaces, use `--pad-x N` (in cells, unaffected by the
glyph side bearings).

Only the **vertical** direction is still cropped to the ink: the ascent/descent
inside the line height `size*1.35` are font metrics, not content, and not cropping
them would waste a large part of the height.

Add `--tight` when you need the old behaviour (crop horizontally to the ink too).

Constraints (the script reads them from the firmware sources itself, and they can
also be overridden by hand)
----
* grid width  W <= screen width / cell step      (default 320 px / 3 px = 106)
* grid height H <= animation viewport height / cell step (default 134 px / 3 px = 44)
* dot count   <= CGL_PHYS2D_MAX_BODIES - 1           (default 640 - 1 = 639, leaving margin)
                                                  NOTE: this is **read live from
                                                  include/ext/cgl_phys2d_config.h**, so a
                                                  change to the pool follows
                                                  automatically (see
                                                  "resource usage" in the CytGL
                                                  manual, section 17.1).
If a constraint is not met the script scales down proportionally and prints the
final size and usage.

Dependencies
----
    pip install pillow

Font license
----
`tools/DelaGothicOne-Regular.ttf` is under SIL OFL 1.1 (license in
licenses/LICENSE_DELAGOTHIC.txt). The rasterised dot matrix is a derivative work of
that font; when switching fonts, confirm the license in the same way and put the
license into the repository next to the font."""

import argparse
import math
import os
import re
import sys

# Force UTF-8 console output, so Windows pipes/terminals do not turn the text into mojibake
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

try:
    from PIL import Image, ImageDraw, ImageFont, ImageOps
except ImportError:  # pragma: no cover
    sys.stderr.write("Pillow is required: pip install pillow\n")
    raise SystemExit(2)


def ink_bbox(img, cut=24):
    """Return the bounding box of the non-empty pixels (used to crop the surrounding whitespace)."""
    return img.point(lambda v: 255 if v > cut else 0).getbbox()

# --------------------------------------------------------------------------
# Reading the firmware configuration: keeps the script in step with the constants in the project
# --------------------------------------------------------------------------
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Where the demo application lives. The firmware sources below used to sit at the
# repository root and were moved under examples/ by the layout migration, which
# left the paths here pointing at nothing: every lookup failed and silently fell
# back to its default. That is invisible while a default happens to equal the real
# value (LOGO_STEP_Q4 = 48, CGL_CFG_SCREEN_W = 320) and wrong where it does not
# (CGL_PHYS2D_MAX_BODIES read as 640 while the firmware uses 528), so the dot-count
# gate was not actually guarding anything. Look in the demo directory as well as
# the root, and the real value wins from either invocation directory.
DEMO = os.path.join(ROOT, "examples", "STM32G431_CytStudio_Demo")


def read_define(rel_path, name, default):
    """Read an integer macro from a C header/source; fall back to default when it is not found."""
    path = os.path.join(ROOT, rel_path)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = re.match(r"\s*#define\s+%s\s+\(?\s*(\d+)" % re.escape(name), line)
                if m:
                    return int(m.group(1))
    except OSError:
        pass
    return default


def read_define_any(paths, name, default):
    """read_define() over several candidate files, searched against both the
    repository root and the demo directory.

    The first file that defines `name` wins; `default` is returned when none does.
    Two reasons it takes a list: a macro may have moved between headers (the body
    pool lives in `include/ext/cgl_phys2d_config.h` today, in `App/phys2d_config.h` before the
    module moved to its own tree, and in `App/phys2d.h` in older trees), and
    the same relative path means different things depending on which of ROOT/DEMO
    the caller meant."""
    if isinstance(paths, str):
        paths = (paths,)
    for rel in paths:
        for base in (ROOT, DEMO):
            val = read_define(os.path.join(base, rel), name, None)
            if val is not None:
                return val
    return default


def firmware_limits():
    """Derive the grid / dot-count limits for this run from the firmware sources."""
    step_q4 = read_define_any("App/app_logo_smash.c", "LOGO_STEP_Q4", 48)
    step_px = step_q4 / 16.0
    screen_w = read_define_any(
        ("include/cytgl/cytgl_config.h", "UI/cgl_config.h"), "CGL_CFG_SCREEN_W", 320)
    screen_h = read_define_any(
        ("include/cytgl/cytgl_config.h", "UI/cgl_config.h"), "CGL_CFG_SCREEN_H", 172)
    bar_h = read_define_any("App/app_page_smash.c", "BAR_H", 18)
    # The body pool is a knob of the cgl_phys2d module and lives in its config header.
    # WARNING: the module's public headers moved from the demo's App/ into the module's
    # own tree (`include/ext/`, reached as `<ext/cgl_phys2d_config.h>`) -- and, further
    # back, the macro moved out of cgl_phys2d.h into cgl_phys2d_config.h -- so the
    # first path that actually defines the macro wins and the older locations stay
    # in the list for older trees. Getting this wrong is silent: the lookup falls
    # back to 640, the dot-count gate then allows bitmaps the firmware cannot hold.
    # `--dry-run` prints the budget it read, which is how this is checked by hand.
    max_bodies = read_define_any(
        ("include/ext/cgl_phys2d_config.h",
         "App/phys2d_config.h", "App/phys2d.h"),
        "CGL_PHYS2D_MAX_BODIES", 640)

    # The animation viewport = screen height - the top and bottom bars - two 1px separator lines
    viewport_h = screen_h - (2 * bar_h) - 2
    return {
        "step_q4": step_q4,
        "step_px": step_px,
        "screen_w": screen_w,
        "screen_h": screen_h,
        "viewport_h": viewport_h,
        "max_w": int(screen_w // step_px),
        "max_h": int(viewport_h // step_px),
        "max_bodies": max_bodies,
        "max_dots": max_bodies - 1,
    }


# --------------------------------------------------------------------------
# Default font selection
#
# WARNING: the generated dot matrix is a derivative work of the font: when shipping with the source, the font license must allow redistribution.
#   That is why the default is the freely redistributable DejaVu only (Bitstream Vera license, shipped with matplotlib).
#   When pointing --font at another font, confirm the license yourself (the script prints a reminder).
# --------------------------------------------------------------------------
FREE_FONT_HINTS = ("dejavu", "noto", "liberation", "terminus", "ubuntu",
                   "cascadia", "opensans", "firasans", "jetbrains")

# The dot matrix is a product of rasterising the font, so the generated file's SPDX states the font license as well
# (the same as UI/cgl_font_data.c does). A font that cannot be recognised is marked GPL only.
FONT_SPDX = (("dejavu", "Bitstream-Vera", "licenses/LICENSE_DEJAVU.txt"),
             ("delagothic", "OFL-1.1", "licenses/LICENSE_DELAGOTHIC.txt"))


def font_license_of(font_path):
    """Infer (SPDX identifier, license file path) from the font file name; return (None, None) when unknown."""
    if not font_path:
        return (None, None)
    name = os.path.basename(font_path).lower()
    for hint, spdx, lic_file in FONT_SPDX:
        if hint in name:
            return (spdx, lic_file)
    return (None, None)


def _dejavu_candidates():
    out = []
    try:
        import matplotlib
        d = os.path.join(os.path.dirname(matplotlib.__file__), "mpl-data", "fonts", "ttf")
        out += [os.path.join(d, n) for n in ("DejaVuSans-Bold.ttf", "DejaVuSans.ttf",
                                             "DejaVuSansMono-Bold.ttf")]
    except ImportError:
        pass
    out += ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "C:/Windows/Fonts/DejaVuSans-Bold.ttf"]
    return out


def pick_font(explicit):
    if explicit:
        if not os.path.exists(explicit):
            raise SystemExit("font not found: %s" % explicit)
        if not any(h in os.path.basename(explicit).lower() for h in FREE_FONT_HINTS):
            print("Note: %s does not look like a freely redistributable font. The dot matrix is a derivative work of it,"
                  "so confirm that the font license allows redistribution before an open-source release." % os.path.basename(explicit))
        return explicit
    for path in _dejavu_candidates():
        if os.path.exists(path):
            return path
    raise SystemExit("No freely redistributable font (DejaVu) found; use --font to point at one")


# --------------------------------------------------------------------------
# Mask generation
# --------------------------------------------------------------------------
def render_text_mask(text, font_path, size, tracking, align="left", tight=False):
    """Draw text as a high-resolution greyscale mask (255 = ink, 0 = empty).

    WYSIWYG: the horizontal bounds = the ink + the spaces **you typed** at the
    start/end of a line. As many spaces as you type is as much margin as you get;
    inter-word spaces, tracking and trailing whitespace are all preserved.

    Only two things are cropped, and neither is something you wrote:
      * the glyph's own left/right side bearings (invisible font metrics) -- so
        with no spaces typed the output is exactly as before, it does not silently
        shrink;
      * the vertical line-box padding (ascent/descent; normally only about 70
        percent of size*1.35 is ink) -- without cropping it would not fit into the
        44-cell-high animation viewport.

    align="left"   left-align each line (matches a text editor; leading indentation is WYSIWYG)
    align="center" centre each line (the old behaviour)
    tight=True     crop horizontally to the ink as well (the old behaviour; leading/trailing spaces are cut off)"""
    font = ImageFont.truetype(font_path, size)
    lines = text.split("\n")

    # Lay out character by character, which is the only way to control the tracking here
    layout = []      # [(chars, total advance width, leading space width, trailing space width)]
    for line in lines:
        chars = []
        pen = 0.0
        for ch in line:
            adv = font.getlength(ch)
            chars.append((ch, pen))
            pen += adv + tracking
        total = max(0.0, pen - tracking)
        # Leading/trailing all-space runs: this is whitespace the user typed and must be preserved as is
        lead = 0.0
        for ch, _ in chars:
            if ch != " ":
                break
            lead += font.getlength(ch) + tracking
        trail = 0.0
        for ch, _ in reversed(chars):
            if ch != " ":
                break
            trail += font.getlength(ch) + tracking
        layout.append((chars, total, min(lead, total), min(trail, total)))

    # The block's left/right padding is decided by the outermost line (the same rule as the ink bounding box)
    lead_px = min(item[2] for item in layout)
    trail_px = min(item[3] for item in layout)

    line_h = int(size * 1.35)
    content_w = int(math.ceil(max(1.0, max(item[1] for item in layout))))
    guard = 8                                   # Only to accommodate the part of italics/ligatures that overhangs the advance width
    width = content_w + 2 * guard
    height = line_h * len(lines) + guard

    mask = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(mask)
    for idx, (chars, line_w, _, _) in enumerate(layout):
        y = guard // 2 + idx * line_h
        x_off = (content_w - line_w) / 2.0 if align == "center" else 0.0
        for ch, pen in chars:
            draw.text((guard + x_off + pen, y), ch, font=font, fill=255)

    bbox = ink_bbox(mask)
    if bbox is None:
        return mask                             # The whole image is empty (for example only spaces were entered)

    y0, y1 = bbox[1], bbox[3]                   # Vertical: crop the line-box padding
    if tight:
        x0, x1 = bbox[0], bbox[2]               # Old behaviour: crop horizontally to the ink as well
    else:
        # Horizontal: add the spaces you typed on top of the ink (floor/ceil guarantee we never keep less)
        x0 = int(math.floor(bbox[0] - lead_px))
        x1 = int(math.ceil(bbox[2] + trail_px))
        x0, x1 = max(0, min(x0, bbox[0])), min(width, max(x1, bbox[2]))
    return mask.crop((x0, y0, x1, y1))


def load_image_mask(path, invert, autocrop):
    """Turn an image into a greyscale mask; a dark background with light text can be inverted automatically."""
    img = Image.open(path)
    img = img.convert("L")

    if autocrop:
        # Use the median as the background colour and crop the surrounding whitespace
        hist = img.histogram()
        total = sum(hist)
        acc = 0
        median = 0
        for val, cnt in enumerate(hist):
            acc += cnt
            if acc >= total / 2:
                median = val
                break
        if median > 127:          # Light background with dark text -> invert first, then crop
            work = ImageOps.invert(img)
        else:
            work = img
        bbox = ink_bbox(work)
        if bbox:
            img = img.crop(bbox)

    if invert:
        img = ImageOps.invert(img)
    return img


# --------------------------------------------------------------------------
# Downsampling + threshold + adaptive scaling
# --------------------------------------------------------------------------
def downsample(mask, grid_w, grid_h, threshold_pct):
    """Downsample by coverage to grid_w x grid_h; return [[0/1], ...] and the dot count."""
    small = mask.resize((grid_w, grid_h), Image.BOX)   # BOX = area average
    px = small.load()
    cut = threshold_pct * 255 // 100
    rows = []
    dots = 0
    for y in range(grid_h):
        row = []
        for x in range(grid_w):
            bit = 1 if px[x, y] >= cut else 0
            row.append(bit)
            dots += bit
        rows.append(row)
    return rows, dots


def fit(mask, lim, max_dots, threshold_pct, min_w=16, pad_x=0):
    """Take the largest proportional grid that fits inside the grid limits and the dot budget.

    pad_x: how many cells of blank to keep on each side (0 = the ink goes right
    up to the grid edge). The padding does not take part in font scaling, it is
    only subtracted from the width limit, so it does not shrink the text -- it just
    leaves the two sides of the dot matrix empty, so the outermost dots do not
    touch the screen edge during the smash animation."""
    src_w, src_h = mask.size
    aspect = src_h / float(src_w)

    # Usable content width = grid limit - padding on both sides
    avail_w = max(1, lim["max_w"] - 2 * max(0, pad_x))
    grid_w = min(lim["max_w"], avail_w)
    grid_h = max(1, int(round(grid_w * aspect)))
    if grid_h > lim["max_h"]:
        grid_h = lim["max_h"]
        grid_w = max(1, int(round(grid_h / aspect)))

    rows, dots = downsample(mask, grid_w, grid_h, threshold_pct)
    scale_warning = None
    if grid_w > src_w:
        scale_warning = ("source image / text render size %dx%d is smaller than the target grid %dx%d,"
                         "which means upscaling; consider increasing --size" % (src_w, src_h, grid_w, grid_h))

    # When the dot count exceeds the budget, shrink everything until it fits
    while dots > max_dots and grid_w > min_w:
        grid_w = max(min_w, int(grid_w * 0.94))
        grid_h = max(4, int(round(grid_w * aspect)))
        rows, dots = downsample(mask, grid_w, grid_h, threshold_pct)

    # Pad both sides with empty cells
    if pad_x > 0:
        blank = [0] * pad_x
        rows = [blank + r + blank for r in rows]

    return rows, dots, scale_warning


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------
def preview(rows, indent="    "):
    out = []
    for row in rows:
        out.append(indent + "".join("#" if b else "." for b in row))
    return "\n".join(out)


def escape_text(s):
    """Turn text into a form that can be pasted straight into a command-line argument."""
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def unescape_text(s):
    r"""Parse the escapes in --text: \n -> newline, \\ -> one backslash; any other backslash is kept as is."""
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            if nxt == "n":
                out.append("\n")
                i += 2
                continue
            if nxt == "\\":
                out.append("\\")
                i += 2
                continue
        out.append(c)
        i += 1
    return "".join(out)


def rel_to_root(path):
    """Paths inside the repository are written relative, paths outside stay absolute; always with / so they paste across platforms."""
    p = os.path.abspath(path)
    if p.startswith(os.path.abspath(ROOT) + os.sep):
        p = os.path.relpath(p, ROOT)
    return p.replace("\\", "/")


def regen_command(args, font_path, out_path):
    """Rebuild a complete command line that reproduces the current dot matrix, written into the Regenerate line of the generated file header."""
    if args.text is not None:
        parts = ["python", "tools/mklogo.py", "--text", '"%s"' % escape_text(args.text)]
        parts += ["--font", '"%s"' % rel_to_root(font_path)]
    else:
        parts = ["python", "tools/mklogo.py", "--image", '"%s"' % rel_to_root(args.image)]
    parts += ["--size", str(args.size), "--tracking", "%g" % args.tracking]
    if args.text is not None:
        parts += ["--align", args.align]
        if args.tight:
            parts.append("--tight")
    else:
        if args.invert:
            parts.append("--invert")
        if args.no_autocrop:
            parts.append("--no-autocrop")
    parts += ["--dot-threshold", str(args.dot_threshold)]
    if args.pad_x:
        parts += ["--pad-x", str(args.pad_x)]
    if args.max_w:
        parts += ["--max-w", str(args.max_w)]
    if args.max_h:
        parts += ["--max-h", str(args.max_h)]
    if args.max_dots:
        parts += ["--max-dots", str(args.max_dots)]
    parts += ["--out", '"%s"' % rel_to_root(out_path)]
    return " ".join(parts)


def emit_header(rows, source_desc, out_path, lim, regen_cmd=None, font_lic=None):
    grid_h = len(rows)
    grid_w = len(rows[0]) if grid_h else 0
    row_bytes = (grid_w + 7) // 8
    dots = sum(sum(r) for r in rows)

    font_spdx, lic_file = font_lic if font_lic else (None, None)
    lines = []
    spdx = "Apache-2.0" + (" AND %s" % font_spdx if font_spdx else "")
    lines.append("// SPDX-License-Identifier: %s" % spdx)
    lines.append("// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>")
    lines.append("/**")
    lines.append("  ******************************************************************************")
    lines.append("  * @file    %s" % os.path.basename(out_path))
    lines.append("  * @brief   Dot matrix logo - auto generated by tools/mklogo.py.")
    lines.append("  *")
    lines.append("  *  Source : %s" % source_desc.replace("*/", "* /"))
    lines.append("  *  Grid   : %d x %d cells, %d lit dots (one physics ball each)" % (grid_w, grid_h, dots))
    lines.append("  *  Screen : %d x %d px at %.2f px per cell (viewport %d x %d)"
                 % (int(grid_w * lim["step_px"]), int(grid_h * lim["step_px"]),
                    lim["step_px"], lim["screen_w"], lim["viewport_h"]))
    lines.append("  *")
    lines.append("  *  Font notice: the text mode rasterises glyphs, so the bitmap is a")
    lines.append("  *  derivative of the source font. Check that font's licence before")
    lines.append("  *  redistributing; the default is DejaVu (Bitstream Vera, see")
    lines.append("  *  licenses/LICENSE_DEJAVU.txt).")
    if lic_file:
        lines.append("  *")
        lines.append("  *  This file: font licence %s, full text in %s."
                     % (font_spdx, lic_file))
    lines.append("  *")
    lines.append("  *  Bit layout: byte index = row * LOGO_ROW_BYTES + x/8, bit = 7 - (x %% 8).")
    if regen_cmd is None:
        regen_cmd = 'python tools/mklogo.py --text "..." --out %s' % rel_to_root(out_path)
    lines.append("  *  Regenerate:  %s" % regen_cmd.replace("*/", "* /"))
    lines.append("  ******************************************************************************")
    lines.append("  */")
    lines.append("#ifndef LOGO_BITMAP_H")
    lines.append("#define LOGO_BITMAP_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("#define LOGO_GRID_W %d" % grid_w)
    lines.append("#define LOGO_GRID_H %d" % grid_h)
    lines.append("#define LOGO_ROW_BYTES %d" % row_bytes)
    lines.append("#define LOGO_DOT_COUNT %d" % dots)
    lines.append("")
    lines.append("static const uint8_t logo_bitmap[LOGO_GRID_H * LOGO_ROW_BYTES] = {")
    for y, row in enumerate(rows):
        packed = []
        for k in range(row_bytes):
            val = 0
            for b in range(8):
                x = k * 8 + b
                if x < grid_w and row[x]:
                    val |= 0x80 >> b
            packed.append(val)
        lines.append("    " + ",".join("0x%02X" % v for v in packed) + ", /* row %2d */" % y)
    lines.append("};")
    lines.append("")
    lines.append("/** Test one dot of the logo grid. */")
    lines.append("static inline int logo_bitmap_get(int x, int y)")
    lines.append("{")
    lines.append("    return (logo_bitmap[(x >> 3) + (y * LOGO_ROW_BYTES)] >> (~x & 0x07)) & 0x01;")
    lines.append("}")
    lines.append("")
    lines.append("#endif /* LOGO_BITMAP_H */")

    text = "\n".join(lines) + "\n"
    # LF and no BOM, to avoid Windows editor/compiler differences
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    return grid_w, grid_h, dots


def verify_roundtrip(out_path, rows):
    """Re-parse the generated file and confirm that the bit order and the dot count match the expectation."""
    with open(out_path, "r", encoding="utf-8") as fh:
        src = fh.read()
    grid_w = int(re.search(r"#define LOGO_GRID_W (\d+)", src).group(1))
    grid_h = int(re.search(r"#define LOGO_GRID_H (\d+)", src).group(1))
    declared = int(re.search(r"#define LOGO_DOT_COUNT (\d+)", src).group(1))
    body = re.search(r"logo_bitmap\[[^\]]*\] = \{(.*?)\};", src, re.S).group(1)
    data = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    row_bytes = (grid_w + 7) // 8

    dots = 0
    for y in range(grid_h):
        for x in range(grid_w):
            if (data[y * row_bytes + (x >> 3)] >> (~x & 7)) & 1:
                dots += 1
    ok = (dots == declared) and (dots == sum(sum(r) for r in rows))
    return ok, dots, declared


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main(argv=None):
    p = argparse.ArgumentParser(
        description="Generate the dot-matrix Logo bitmap (App/logo_bitmap.h)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Example: python tools/mklogo.py --text \"CytStudio\"")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--text", help="text content, \\n starts a new line")
    src.add_argument("--image", help="image path (PNG/JPG/BMP, light text on a dark background)")

    p.add_argument("--font", help="font file path (by default a bold sans-serif is picked automatically)")
    p.add_argument("--size", type=int, default=160, help="render height in pixels (default 160, larger is finer)")
    p.add_argument("--tracking", type=float, default=0.0, help="tracking, in render pixels")
    p.add_argument("--align", choices=("left", "center"), default="left",
                   help="alignment of multi-line text: left = left align, the spaces you typed are preserved (default);"
                        "center = centre each line (the old behaviour)")
    p.add_argument("--tight", action="store_true",
                   help="crop the text horizontally to the ink as well (the old behaviour; leading/trailing spaces are cut off)")
    p.add_argument("--invert", action="store_true", help="invert the image (dark background, light text)")
    p.add_argument("--no-autocrop", action="store_true", help="do not auto-crop whitespace from the image")

    p.add_argument("--max-w", type=int, help="grid width limit (derived from the screen by default)")
    p.add_argument("--max-h", type=int, help="grid height limit (derived from the animation viewport by default)")
    p.add_argument("--max-dots", type=int, help="dot count limit (default CGL_PHYS2D_MAX_BODIES-1)")
    p.add_argument("--dot-threshold", type=int, default=40,
                   help="coverage threshold %%, lower means thicker strokes (default 40)")
    p.add_argument("--pad-x", type=int, default=0, metavar="N",
                   help="keep N cells of blank on each side (default 0, i.e. the ink touches the grid edge)")

    p.add_argument("--out", default="examples/common/App/logo_bitmap.h", help="output header file")
    p.add_argument("--dry-run", action="store_true", help="preview only, write no file")
    p.add_argument("--no-preview", action="store_true", help="do not print the dot matrix preview")
    args = p.parse_args(argv)
    if args.text is not None:
        args.text = unescape_text(args.text)     # Support --text "Cyt\nStudio"

    lim = firmware_limits()
    if args.max_w:
        lim["max_w"] = args.max_w
    if args.max_h:
        lim["max_h"] = args.max_h
    if args.max_dots:
        lim["max_dots"] = args.max_dots
    lim["max_dots"] = min(lim["max_dots"], lim["max_bodies"] - 1)

    # --- generate the mask ---
    font = None
    if args.text is not None:
        if args.text.strip() == "":
            raise SystemExit("text must not be empty")
        font = pick_font(args.font)
        mask = render_text_mask(args.text, font, args.size, args.tracking,
                                align=args.align, tight=args.tight)
        source = 'text %r, font %s @%dpx, tracking %.1f' % (
            args.text, os.path.basename(font), args.size, args.tracking)
    else:
        if not os.path.exists(args.image):
            raise SystemExit("image not found: %s" % args.image)
        mask = load_image_mask(args.image, args.invert, not args.no_autocrop)
        source = "image %s%s" % (args.image, " (inverted)" if args.invert else "")

    # --- fit + threshold ---
    rows, dots, warn = fit(mask, lim, lim["max_dots"], args.dot_threshold,
                           pad_x=max(0, args.pad_x))
    grid_w = len(rows[0]) if rows else 0
    grid_h = len(rows)

    # --- report ---
    print("source    : %s" % source)
    print("mask      : %d x %d" % mask.size)
    print("grid      : %d x %d cells  (limit %d x %d)"
          % (grid_w, grid_h, lim["max_w"], lim["max_h"]))
    if args.pad_x > 0:
        print("pad       : %d cells on each side (%.0f px total), content width %d cells"
              % (args.pad_x, 2 * args.pad_x * lim["step_px"],
                 max(0, grid_w - 2 * args.pad_x)))
    print("dots      : %d  (budget %d, %.0f%%)"
          % (dots, lim["max_dots"], 100.0 * dots / max(1, lim["max_dots"])))
    print("on screen : %d x %d px  (cell %.2f px, viewport %d x %d)"
          % (int(grid_w * lim["step_px"]), int(grid_h * lim["step_px"]),
             lim["step_px"], lim["screen_w"], lim["viewport_h"]))
    if warn:
        print("warning   : %s" % warn)
    if grid_h < 12:
        print("warning   : the grid is only %d cells high, the text will be very small;"
              "try wrapping with \\n, shortening the text or using a narrower font" % grid_h)
    if grid_w < 24 or grid_h < 6:
        print("warning   : the grid is rather small, the text may be too long")
    if dots > lim["max_dots"]:
        print("error     : the dot count exceeds the budget, cannot generate")

    if not args.no_preview:
        print("")
        print(preview(rows))

    if args.dry_run:
        print("\n(--dry-run: no files written)")
        return 0

    # --- write file + read-back verification ---
    out_path = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    gw, gh, n = emit_header(rows, source, out_path, lim,
                            regen_command(args, font, out_path),
                            font_license_of(font))
    ok, re_dots, declared = verify_roundtrip(out_path, rows)
    print("\nwritten   : %s  (%d x %d, %d dots)" % (out_path, gw, gh, n))
    print("readback  : %s (decoded %d dots, declared %d dots)"
          % ("pass" if ok else "fail", re_dots, declared))
    print("\nnext      : Build -> flash; to revert the changes use git checkout")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
