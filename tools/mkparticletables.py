#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
"""
Generate the two constant tables the cgl_particles engine needs.

    python tools/mkparticletables.py --out include/ext/cgl_particles_tables.h \
                                     --src src/ext/cgl_particles_tables.c

The engines on this chip may not call `sinf`/`cosf` and may not divide in the hot
path (see `include/ext/cgl_particles.h`), so both are shipped as tables computed
here, at build-authoring time, in exact integer arithmetic. Generating them beats
hand-pasting 320 numbers for two reasons that both matter on an embedded target:

  1. **The rounding rule is stated once, in code.** `(v * 256 + 128) // 256` is
     round-half-up of a Q8.8 value; a hand-typed table quietly accumulates
     whichever rule the author had in their head that day, and a table that is
     off by one at a quadrant boundary produces a visible kink in the orbit.
  2. **Reachability is checked by the generator, not discovered on hardware.**
     Both tables are range-checked against the type they are stored in (`uint8_t`
     for the sine, `uint16_t` for the reciprocal) and the script fails loudly
     instead of emitting a wrapped value.

The output
----------
Two files, because a `static const` table in a header is emitted once per
translation unit that includes it:

    --out include/ext/cgl_particles_tables.h   `extern const` declarations
    --src src/ext/cgl_particles_tables.c       the definitions. The module mirrors its
                                     two trees (include/ext/x.h <-> src/ext/x.c), so this
                                     is also what --src defaults to; pass it explicitly
                                     only when generating into a different tree
"""

import argparse
import math
import os
import sys

SIN_ENTRIES = 64          # entries in the 0..90 deg quarter
RECIP_NEAR = 128          # first depth the reciprocal table covers, px
RECIP_ENTRIES = 256       # 128..383 inclusive
Q8 = 256                  # Q8.8 scale
Q16 = 1 << 16             # Q16.16 scale

# sin(x) in Q8.8 reaches exactly 256 at 90 deg, which does not fit the `uint8_t`
# the table is stored in. Two ways out: widen the table to `int16_t` (128 B of
# flash, plus a 16 bit load in the inner loop) or clamp to 255, i.e. let the
# quantised sine saturate at 0.996 of full scale one step before the peak.
#
# Clamping is the right trade here: the value is a rotation coefficient, 255/256
# is 0.22 deg of angular error -- far below the 1.4 deg resolution of the angle
# type itself -- and it keeps the load 8 bit, which matters because the coefficient
# is read twice per particle per band. The clamp is applied **loudly**: an
# unclamped 256 emitted as `0x100` into a `uint8_t` array truncates to `0`, which
# turns sin(90 deg) into zero and makes the camera matrix singular. That is not a
# hypothetical; it is what the first version of this generator did.
SIN_MAX = Q8 - 1


def round_div(num, den):
    """Round-half-up integer division that also behaves for negative num."""
    if num >= 0:
        return (num + den // 2) // den
    return -((-num + den // 2) // den)


def sin_table():
    """[(index, value)] for sin(k*90/64 deg) in Q8.8, saturated at SIN_MAX."""
    out = []
    for k in range(SIN_ENTRIES):
        # k = 64 would be exactly 90 deg = 1.0, but it is not in the table: the
        # fold in cgl_particles_sin_q8() maps 90 deg onto index 0 of the mirrored quadrant.
        v = math.sin(math.radians(90.0 * k / SIN_ENTRIES))
        q = round_div(int(round(v * 1e12)), int(round(1e12 / Q8)))
        if q > SIN_MAX:
            q = SIN_MAX
        if not (0 <= q <= SIN_MAX):
            raise SystemExit("sin table entry %d out of 8 bit range: %d" % (k, q))
        out.append((k, q))
    return out


def recip_table():
    """[(depth_px, value)] for 1/depth in Q16.16, depth = 0..RECIP_NEAR+RECIP_ENTRIES-1.

    The depths below RECIP_NEAR are the **padding that makes the index an
    identity**: nothing reads them (the engine clamps the depth to the near plane
    first), but their presence is what lets `cgl_particles_recip_q16()` use the integer part of
    the depth as the index with no subtraction. Emitting them here rather than
    leaving a hole at the front of the array is what keeps "the table's index" and
    "the depth" the same number in the generator, in the header's doc comment, in
    the engine and in the tests.
    """
    out = []
    for d in range(0, RECIP_NEAR + RECIP_ENTRIES):
        if d < RECIP_NEAR:
            # padding: clamped-to-near value, so a mis-indexed read is still sane
            q = round_div(Q16, RECIP_NEAR)
        else:
            q = round_div(Q16, d)
        if not (0 <= q <= 0xFFFF):
            raise SystemExit("recip table entry %d out of uint16 range: %d" % (d, q))
        out.append((d, q))
    return out


HEADER = """// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_particles_tables.h
  * @brief   Precomputed sine / reciprocal tables for the bullet time particle
  *          engine. **Auto generated by tools/mkparticletables.py -- do not edit.**
  *
  *  Regenerate:
  *      python tools/mkparticletables.py --out include/ext/cgl_particles_tables.h
  *
  *  Why these are tables and not calls
  *  ----------------------------------
  *  The target has no FPU and the module may not link libm. `sinf()`/`cosf()`
  *  would be a soft-float call of several hundred cycles *per lookup*, and the
  *  camera evaluates the perspective divide once **per particle per band**, which
  *  is thousands of times per frame. A table look-up is a load; a `sdiv` is
  *  ~2..12 cycles plus the libgcc entry. Both are replaced here.
  *
  *  WARNING: the tables are `extern const` **declarations**, and their definitions
  *  live in `cgl_particles.c`. Do not turn them into `static const` arrays
  *  in this header: this header is included by the engine, by the page and by the
  *  host tests, and a `static` table would then be emitted **once per translation
  *  unit** -- 578 B of flash each time, for data that is identical in all of them.
  *
  *  Size: %(sin_bytes)d B of sine + %(recip_bytes)d B of reciprocal = %(total)d B
  *  of **flash** (const, so it is never copied to RAM), in exchange for which the
  *  inner loops contain no division and no transcendental call at all.
  ******************************************************************************
  */
#ifndef CGL_PARTICLES_TABLES_H
#define CGL_PARTICLES_TABLES_H

#include <stdint.h>

/**
 * sin(k * 90 deg / %(sin_entries)d), Q8.8, k = 0..%(sin_last)d.
 *
 * Only 0..90 deg is stored. The other quadrants are folded in the accessors
 * (`cgl_particles_sin_q8` / `cgl_particles_cos_q8`, see cgl_particles.h):
 *
 *      2nd quadrant (angle byte & 0x40)   mirror the index:  sin(90+x) =  sin(90-x)
 *      3rd/4th        (angle byte & 0x80) negate the result: sin(180+x) = -sin(x)
 *
 * 64 entries per quadrant is exactly the resolution of a 16 bit angle (256 steps
 * per turn), so no accuracy is lost to the fold: a bigger table would be indexed
 * by angles that cannot be represented.
 *
 * **The entries saturate at %(sin_max)d, not at %(sin_entries)d** (which equals 1.0 in
 * Q8.8). sin(90 deg) rounds to 256, and 256 does not fit the `uint8_t` this table
 * is stored in. The last two entries are therefore clamped to %(sin_max)d, i.e.
 * 0.996 of full scale, which is 0.22 deg of angular error -- below the 1.4 deg
 * resolution of the angle type -- and it buys an 8 bit load for a coefficient read
 * twice per particle per band. An unclamped 256 would be emitted as `0x100`,
 * truncate to 0 in the array, and make sin(90 deg) **zero**: a singular camera
 * matrix, not a rounding artefact. `tools/mkparticletables.py` clamps it at
 * generation time so the number in the source can never be the wrapped one.
 *
 * Definition: %(src_name)s.
 */
extern const uint8_t cgl_particles__sin_q8[%(sin_entries)d];

/**
 * 1/depth in Q16.16 for depth = 0..%(recip_last)d px.
 *
 * **The table is indexed by the depth itself**, so `cgl_particles__recip[d]` is
 * `round(65536/d)` for every depth `d` the camera can produce. The first
 * `%(recip_near)d` entries (depths 0..%(recip_pad_last)d) are never read -- the
 * engine clamps the depth into `%(recip_near)d..%(recip_last)d` first, and that clamp
 * *is* the near/far plane -- but they exist so that "depth -> index" is an
 * **identity**, with no subtraction and no shift in the inner loop. That costs
 * %(recip_pad_bytes)d B of flash and saves an instruction on a path that runs twice per
 * particle per band.
 *
 * The `+ 1` final entry is what the interpolation's `k + 1` read lands on at the
 * far clamp, so the index can never leave the array.
 *
 * The value is exact for every integer depth; the only error in the whole
 * perspective divide is the linear interpolation between two neighbours, which is
 * below 0.8%% relative (see `cgl_particles_recip_q16()`), i.e. well under 1/100 px at the
 * screen edge.
 *
 * Definition: %(src_name)s.
 */
extern const uint16_t cgl_particles__recip[%(recip_entries_total)d];

#endif /* CGL_PARTICLES_TABLES_H */
"""


SOURCE = """%(source_head)s

#include <ext/cgl_particles_tables.h>

/* The two tables. They live here and not in the header so that the engine, the
   page and the host tests all link to **one** copy of 578 B of flash -- see the
   WARNING in cgl_particles_tables.h. */

const uint8_t cgl_particles__sin_q8[%(sin_entries)d] = {
%(sin_rows)s};

const uint16_t cgl_particles__recip[%(recip_entries_total)d] = {
%(recip_rows)s};
"""



def fmt_rows(values, per_row, fmt, indent="    "):
    """Format integers as C initialiser rows, `per_row` values each."""
    lines = []
    for i in range(0, len(values), per_row):
        chunk = values[i:i + per_row]
        lines.append(indent + "".join((fmt % v) + "," for v in chunk))
    return "\n".join(lines) + "\n"


def default_src_path(out_path):
    """Where the .c definitions go when --src is not given.

    The module keeps its two trees mirrored: `include/ext/x.h` declares the tables and
    `src/ext/x.c` defines them. Deriving "next to the header" would therefore drop the
    .c into `include/ext/`, where no build collects it -- the real source would stay
    stale and the regenerated table would silently never reach the firmware, which is
    exactly the failure class this repository sizes builds to catch. So when --out sits
    in an `include/ext/` directory the mirrored `src/ext/` path is used; any other
    layout keeps the old "same directory and stem as --out" rule.
    """
    head = os.path.dirname(os.path.normpath(out_path))
    if os.path.basename(head) == "ext" and os.path.basename(os.path.dirname(head)) == "include":
        return os.path.join(os.path.dirname(os.path.dirname(head)), "src", "ext",
                            os.path.splitext(os.path.basename(out_path))[0] + ".c")
    return os.path.splitext(out_path)[0] + ".c"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--out", required=True, help="path of the header to write")
    ap.add_argument("--src", default=None,
                    help="path of the .c definitions to write (default: the module's "
                         "mirrored source tree, include/ext/x.h -> src/ext/x.c; falls "
                         "back to 'same directory and stem as --out')")
    args = ap.parse_args()

    sins = sin_table()
    recips = recip_table()

    # The interpolation in cgl_particles_recip_q16() reads cgl_particles__recip[k + 1], so the table
    # carries one extra entry beyond the far plane (the near clamp's value, which
    # can never be read by a correctly clamped depth) and the index can never leave
    # the array.
    recip_values = [v for _, v in recips]
    recip_values.append(round_div(Q16, RECIP_NEAR + RECIP_ENTRIES - 1))

    src_path = args.src
    if src_path is None:
        src_path = default_src_path(args.out)

    src_name = os.path.basename(src_path)

    common = {
        "src_name": src_name,
        "sin_bytes": len(sins),
        "recip_bytes": len(recip_values) * 2,
        "total": len(sins) + len(recip_values) * 2,
        "sin_entries": SIN_ENTRIES,
        "sin_last": SIN_ENTRIES - 1,
        "sin_max": SIN_MAX,
        "recip_near": RECIP_NEAR,
        "recip_pad_last": RECIP_NEAR - 1,
        "recip_pad_bytes": RECIP_NEAR * 2,
        "recip_last": RECIP_NEAR + RECIP_ENTRIES - 1,
        "recip_entries_next": len(recip_values) - 1,
        "recip_entries_total": len(recip_values),
        "sin_rows": fmt_rows([v for _, v in sins], 8, "0x%02X"),
        "recip_rows": fmt_rows(recip_values, 8, "0x%04X"),
    }

    source_head = """// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    %s
  * @brief   Definitions of the bullet time particle engine's sine / reciprocal
  *          tables. **Auto generated by tools/mkparticletables.py -- do not edit.**
  *
  *  Regenerate:
  *      python tools/mkparticletables.py --out include/ext/cgl_particles_tables.h
  *
  *  Why the definitions are not `static const` in the header: cgl_particles_tables.h is
  *  included by the engine, by the page and by the host tests, and a `static`
  *  table in a header costs %d B of flash **per translation unit** for data that
  *  is byte identical in all of them.
  ******************************************************************************
  */""" % (src_name, common["total"])

    text = HEADER % common
    src_text = SOURCE % dict(common, source_head=source_head)

    for path, payload in ((args.out, text), (src_path, src_text)):
        out_dir = os.path.dirname(os.path.abspath(path))
        if out_dir and not os.path.isdir(out_dir):
            os.makedirs(out_dir)
        with open(path, "w", newline="\n") as f:
            f.write(payload)

    # Report what was generated, so the build log carries the numbers a reviewer
    # would otherwise have to count by hand.
    print("wrote %s" % args.out)
    print("wrote %s" % src_path)
    print("  cgl_particles__sin_q8[%d]  %d B  (sin(0)=%d, sin(90-)=%d)"
          % (len(sins), len(sins), sins[0][1], sins[-1][1]))
    print("  cgl_particles__recip[%d]   %d B  (1/%d=%.4f -> %d, 1/%d=%.4f -> %d)"
          % (len(recip_values), len(recip_values) * 2,
             RECIP_NEAR, 1.0 / RECIP_NEAR, recip_values[0],
             RECIP_NEAR + RECIP_ENTRIES - 1, 1.0 / (RECIP_NEAR + RECIP_ENTRIES - 1),
             recip_values[-2]))
    print("  total flash     %d B" % (len(sins) + len(recip_values) * 2))


if __name__ == "__main__":
    sys.exit(main())
