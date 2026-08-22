#!/usr/bin/env python3
"""Generate the ED047TC2 LovyanGFX waveform tables from the vendor waveform blob.

Input is the epdiy-format vendor waveform header for the ED047TC2 panel used by
the LilyGo T5 S3 Pro (`epdiy_ED047TC2.h` / `ED047TC2.h` -- the two are the same
data under different symbol names).  Output is
`libs/hardware/BoardT5S3/src/ED047TC2Waveform.cpp`.

    python tools/gen_ed047tc2_waveform.py path/to/epdiy_ED047TC2.h

Vendor blob layout
------------------
Each `epd_wp_ED047TC2_<mode>_<range>_data[phases][16][4]` table holds, for one
draw mode and one temperature range, a 2-bit drive code per (destination level,
source level) pair per phase:

    code 0 = no drive,  1 = drive toward black,  2 = drive toward white

The outer index of the 16 rows is the DESTINATION level; the 4 bytes of each row
hold the 16 SOURCE levels, four per byte, most significant pair first.  Level 0
is black, level 15 is white.

Why the tables below are only two columns wide
----------------------------------------------
Decoding the blob shows every mode/range is a separable impulse waveform:

    net_frames(to, from) == L[to] - L[from]

with a zero diagonal, for one impulse vector L per temperature range shared by
all three modes (DU, GC16, GL16).  The DU tables are exactly `L[15] + 1` phases
long and drive black->white as `L[15]` consecutive white frames (and white->black
as `L[15]` consecutive black frames), so DU reduces to a single number per
temperature range: the drive length.  This script asserts all of that against the
blob before emitting anything, so a different or corrupted input fails loudly
rather than producing a plausible-looking wrong waveform.

The emitted banks turn that vector into a single-push architecture:

epd_fast/epd_fastest get the DU rails plus two SELF-NORMALIZING grey columns.
A LUT column is indexed by destination alone and cannot see where a pixel came
from, so a column that must land on a mid level from an arbitrary source first
saturates at a rail and then walks to its target: L[15] frames toward white
(any source becomes white -- the rail clamps), then L[15]-L[g] frames toward
black. One column, correct from every source state. This is what lets a page
carry its greys in the same push as its text: no separate B/W base pass exists
to pre-position the fringe, and none is needed.

epd_text/epd_quality become a true GC16-style refresh: every one of the 16
columns rail-normalizes (dark half to white, light half to black) and then
walks to its exact level. Destination-indexed yet source-independent, it both
scrubs residue and resets any accumulated DC bias, because the saturating rail
visit erases a pixel's drive history.

Panel_EPD keeps one uint8_t block index per bank (lut_2pixel is addressed as
lindex >> 8), so the five banks together must stay within 255 rows. The
generator asserts that for every temperature range.

"""

import re
import sys
from pathlib import Path

BLOB_RE = re.compile(
    r"const uint8_t epd_wp_ED047TC2_(\d+)_(\d+)_data\[(\d+)\]\[16\]\[4\]\s*=\s*(.*?);",
    re.S | re.I,
)
INTERVAL_RE = re.compile(
    r"const EpdWaveformTempInterval ed047tc2_intervals\[(\d+)\]\s*=\s*(.*?);", re.S
)

MODE_DU, MODE_GC16, MODE_GL16 = 1, 2, 5

# The vendor blob carries ranges 5..11 of the 14-entry interval table.
FIRST_RANGE = 5
RANGE_COUNT = 7


def parse(path):
    text = Path(path).read_text(encoding="utf-8", errors="replace")

    tables = {}
    for m in BLOB_RE.finditer(text):
        mode, rng, phases = int(m.group(1)), int(m.group(2)), int(m.group(3))
        vals = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(4))]
        if len(vals) != phases * 16 * 4:
            raise SystemExit(f"mode {mode} range {rng}: expected {phases*16*4} bytes, got {len(vals)}")
        table = []
        for p in range(phases):
            rows = []
            for r in range(16):
                base = (p * 16 + r) * 4
                row = []
                for byte in vals[base : base + 4]:
                    row += [(byte >> (6 - 2 * k)) & 3 for k in range(4)]
                rows.append(row)
            table.append(rows)
        tables[(mode, rng)] = table

    im = INTERVAL_RE.search(text)
    if not im:
        raise SystemExit("no ed047tc2_intervals table found")
    nums = [int(x) for x in re.findall(r"\.(?:min|max)\s*=\s*(-?\d+)", im.group(2))]
    intervals = list(zip(nums[0::2], nums[1::2]))

    return tables, intervals


def impulse_vector(table):
    """Return L with L[0] == 0 such that net(to, from) == L[to] - L[from]."""
    net = [[0] * 16 for _ in range(16)]
    for phase in table:
        for to in range(16):
            for src in range(16):
                code = phase[to][src]
                if code == 1:
                    net[to][src] -= 1
                elif code == 2:
                    net[to][src] += 1
                elif code == 3:
                    raise SystemExit("unexpected drive code 3 in vendor blob")
    return net, [net[to][0] - net[0][0] for to in range(16)]


def check(tables, intervals):
    """Validate every structural assumption the generated tables rely on."""
    ranges = list(range(FIRST_RANGE, FIRST_RANGE + RANGE_COUNT))
    for mode in (MODE_DU, MODE_GC16, MODE_GL16):
        for rng in ranges:
            if (mode, rng) not in tables:
                raise SystemExit(f"vendor blob is missing mode {mode} range {rng}")
    if len(intervals) < FIRST_RANGE + RANGE_COUNT:
        raise SystemExit("interval table too short for ranges 5..11")

    impulses = {}
    for rng in ranges:
        net_gc, l_gc = impulse_vector(tables[(MODE_GC16, rng)])
        # GC16 must be exactly separable with a zero diagonal.
        for to in range(16):
            for src in range(16):
                if net_gc[to][src] != l_gc[to] - l_gc[src]:
                    raise SystemExit(f"range {rng}: GC16 is not separable at ({to},{src})")
            if net_gc[to][to] != 0:
                raise SystemExit(f"range {rng}: GC16 diagonal is not zero at {to}")
        if l_gc != sorted(l_gc):
            raise SystemExit(f"range {rng}: GC16 impulse vector is not monotonic: {l_gc}")

        # GL16 must agree with GC16 on the impulse vector.
        _, l_gl = impulse_vector(tables[(MODE_GL16, rng)])
        if l_gl != l_gc:
            raise SystemExit(f"range {rng}: GL16 impulse {l_gl} != GC16 impulse {l_gc}")

        # DU must be the two-level restriction of the same vector, run as one
        # contiguous burst starting at phase 0, and nothing else.
        du = tables[(MODE_DU, rng)]
        drive = l_gc[15]
        if len(du) != drive + 1:
            raise SystemExit(f"range {rng}: DU has {len(du)} phases, expected {drive + 1}")
        for to in range(16):
            for src in range(16):
                seq = [phase[to][src] for phase in du]
                if to == 15 and src == 0:
                    want = [2] * drive + [0]
                elif to == 0 and src == 15:
                    want = [1] * drive + [0]
                elif to in (0, 15):
                    continue  # intermediate sources: unused by a binarised canvas
                else:
                    want = [0] * len(du)
                if seq != want:
                    raise SystemExit(f"range {rng}: DU[{to}][{src}] = {seq}, expected {want}")
        impulses[rng] = l_gc
    return impulses


# Optical targets for the two AA greys, as a fraction of the impulse it takes to
# cross from black to white. The font buckets a glyph edge at 25-50% ink (its
# light grey) and 50-75% ink (its dark grey); these are those buckets' midpoints,
# so the panel reproduces the coverage the font quantiser measured.
#
# Do not reach for these to make text heavier or lighter -- that is what the
# reader's stroke weight setting is for, and it works by remapping which bucket
# gets which tone rather than by moving the tones themselves. Retune these only
# against a grey that reads as the wrong *shade* next to its neighbours.
GRAY_TARGET_DARK = 0.375
GRAY_TARGET_LIGHT = 0.625

# A grey is only usable if it stays clear of white and clear of the other grey.
GRAY_MAX_FRACTION = 0.95
GRAY_MIN_SEPARATION = 0.08

# Panel_EPD's per-bank offsets are uint8_t block indices, so all five banks
# (eraser + quality + text + fast + fastest) share a 255-row budget.
LUT_ROW_BUDGET = 255
ERASER_ROWS = 3  # LovyanGFX lut_eraser: 2 drive rows + terminator
UNUSED_SLOT_ROWS = 1  # the board config stubs epd_quality/epd_fastest (see there)

# The binding constraint is NOT the 255-row table: Panel_EPD stores per-pixel
# progress in uint16_t and flags fast modes with +0x8000, so a fast bank's
# STARTING block must be <= 127. Slot order is eraser, quality, text, fast,
# fastest; with quality stubbed the fast start is eraser + stub + clean.
FAST_START_BUDGET = 127


def pick_gray_levels(impulse):
    """Pick the (dark, light) canvas levels that best hit the targets in one range.

    The vendor impulse vector is not evenly spaced and its spacing changes with
    temperature -- at 33..38 C levels 12..15 are all the same optical white, so a
    level that reads as a good light grey when cold is no grey at all when warm.
    Choosing per range is what keeps both greys real across the whole table.

    Runs of equal impulse make exact ties common (at 24..27 C levels 8 through 11
    are one optical step), so separation breaks them: of two pairs that score the
    same, the one whose greys are further apart is the one you can actually tell
    apart on the panel.
    """
    white = impulse[15]
    frac = [impulse[lv] / white for lv in range(16)]

    best = None
    for dark in range(1, 15):
        for light in range(dark + 1, 15):
            if frac[light] > GRAY_MAX_FRACTION:
                continue
            sep = frac[light] - frac[dark]
            if sep < GRAY_MIN_SEPARATION:
                continue
            err = (frac[dark] - GRAY_TARGET_DARK) ** 2 + (frac[light] - GRAY_TARGET_LIGHT) ** 2
            key = (round(err, 9), -sep)
            if best is None or key < best[0]:
                best = (key, dark, light)
    if best is None:
        raise SystemExit("no usable grey pair for impulse %s" % impulse)
    return best[1], best[2]


def fast_rows(l15, dark, l_dark, light, l_light):
    """The differential bank: DU rails plus two self-normalizing grey columns.

    Rails run the vendor DU verbatim: L[15] frames of full drive, then park.
    Each grey column saturates at the white rail for the same L[15] frames --
    which erases whatever state the pixel arrived in, rail clamp doing the work
    a source index would otherwise have to -- and then walks back down toward
    black for L[15]-L[g] frames to land on its level. The walk-down phases park
    the rails at no-op, so the bank is L[15] + max walk-down rows long.
    """
    back_dark, back_light = l15 - l_dark, l15 - l_light
    rows = []
    for f in range(l15 + max(back_dark, back_light)):
        codes = [3] * 16
        if f < l15:
            codes[0] = 1
            codes[15] = 2
            codes[dark] = 2
            codes[light] = 2
        else:
            if f - l15 < back_dark:
                codes[dark] = 1
            if f - l15 < back_light:
                codes[light] = 1
        rows.append("    LUT_MAKE(%s)," % ", ".join(str(c) for c in codes))
    return rows


def clean_rows(impulse):
    """The GC16-style clean bank for epd_text / epd_quality.

    Three phases, uniform across all 16 columns so the refresh reads as a
    blink rather than as noise: every pixel drives to the black rail for L[15]
    frames, then to the white rail for L[15], then walks down from white to its
    exact level (L[15]-L[i] frames toward black). The double rail excursion is
    the scrub -- it erases drive history and accumulated DC bias -- and the
    final descent lands every level, greys included, precisely.

    An earlier cut of this bank sent each pixel to the rail OPPOSITE its
    destination and back, which scrubbed and landed just as well but showed the
    old page fading THROUGH the new page inverted, both at once: on a real
    device the cadence refresh read as a screenful of garbage before the text
    resolved. Same physics, reordered for the eye watching it.
    """
    l15 = impulse[15]
    rows = []
    for _ in range(l15):
        rows.append("    LUT_MAKE(%s)," % ", ".join(["1"] * 16))
    for _ in range(l15):
        rows.append("    LUT_MAKE(%s)," % ", ".join(["2"] * 16))
    for f in range(l15):
        codes = ["1" if f < l15 - impulse[i] else "3" for i in range(16)]
        rows.append("    LUT_MAKE(%s)," % ", ".join(codes))
    return rows


HEADER = '''// GENERATED FILE -- DO NOT EDIT BY HAND.
//
// Regenerate with:
//     python tools/gen_ed047tc2_waveform.py <vendor ED047TC2 waveform header>
//
// Source: the ED047TC2 vendor waveform in epdiy form -- LilyGo ships it as
// Waveform_header/ED047TC2.h in Xinyuan-LilyGO/LilyGo-EPD47, epdiy as
// epdiy_ED047TC2.h; the two are the same data under different symbol names.
// See tools/gen_ed047tc2_waveform.py for the blob layout and for the structural
// checks the generator runs against it.
//
// The vendor waveform is a separable impulse waveform: a transition from source
// level `f` to destination level `t` needs `L[t] - L[f]` frames of drive, toward
// white when positive and toward black when negative, where L is the per
// temperature range impulse vector below (level 0 is black, 15 is white):
//
{impulse_comment}
//
// Both emitted banks are SELF-NORMALIZING: a LovyanGFX LUT column is indexed by
// destination alone, so any column that must land on a mid level from an
// arbitrary source first saturates at a rail -- the clamp erases the pixel\'s
// history -- and then walks to its target. That is what lets a page carry its
// anti-aliasing greys in the same push as its text, with no separate B/W base
// pass to pre-position the fringe.
//
// kFastLut (epd_fast / epd_fastest): the vendor DU rails verbatim, plus a grey
// column per AA tone that spends L[15] frames at the white rail and then
// descends L[15]-L[g] frames to its level. Panel_EPD\'s per-pixel diff means a
// pixel is only driven when its target changes, so a fringe pixel that stays
// the same grey across a page turn is not driven at all -- which is also the
// charge story: drives happen on content changes only, each one begins with a
// saturating rail visit, and the rail visit resets whatever DC bias the pixel
// had accumulated.
//
// kCleanLut (epd_text / epd_quality): a GC16-style refresh. Every one of the 16
// columns rail-normalizes (dark half to white, light half to black) and then
// walks to its exact level, so the periodic clean page both scrubs residue and
// re-lands every grey precisely. The previous bank drove each level away and
// symmetrically back, which is only a correct landing for the two rails; any
// grey pushed through it ended on a rail.
//
// Which levels carry the greys is decided per temperature range, because the
// vendor vector is not evenly spaced and its spacing moves with temperature: at
// 33..38 C levels 12 through 15 are all the same optical white. The board config
// reads kGrayLevelDark/kGrayLevelLight for the range it selected the LUT for and
// writes the matching canvas bytes, so the two cannot drift apart.
//
{gray_comment}
//
// Keeping the drive length matched to the panel temperature IS the temperature
// compensation: e-ink particles move more slowly when cold, so a cold panel
// needs a longer push for the same optical result.
//
// Panel_EPD addresses its expanded LUT with uint8_t block indices, so all five
// banks together (eraser + quality + text + fast + fastest) must fit in 255
// rows. Worst case here: {worst_rows} rows.

#include <ED047TC2Waveform.h>

namespace freeink {{
namespace ed047tc2 {{

namespace {{

// One frame of a LovyanGFX Panel_EPD LUT: sixteen 2-bit drive codes, indexed by
// the destination level. 0 ends the sequence, 1 drives toward black, 2 drives
// toward white, 3 is a no-op that keeps the sequence running.
#define LUT_MAKE(d0, d1, d2, d3, d4, d5, d6, d7, d8, d9, da, db, dc, dd, de, df)         \\
  (uint32_t)((d0 << 0) | (d1 << 2) | (d2 << 4) | (d3 << 6) | (d4 << 8) | (d5 << 10) |    \\
             (d6 << 12) | (d7 << 14) | (d8 << 16) | (d9 << 18) | (da << 20) |            \\
             (db << 22) | (dc << 24) | (dd << 26) | (de << 28) | (df << 30))

'''

FOOTER = '''#undef LUT_MAKE

}}  // namespace

const TempRange kTempRanges[kTempRangeCount] = {{
{ranges}
}};

// The canvas levels the AA grey columns are cut for, per temperature range. A
// board config turns these into the grey bytes its canvas writes; they are not
// independently tunable -- retarget them in tools/gen_ed047tc2_waveform.py and
// regenerate, or the canvas will address a column the LUT does not drive.
const uint8_t kGrayLevelDark[kTempRangeCount] = {{{gray_dark_levels}}};

const uint8_t kGrayLevelLight[kTempRangeCount] = {{{gray_light_levels}}};

const uint32_t* const kFastLut[kTempRangeCount] = {{
{fast_ptrs}
}};

const size_t kFastLutStep[kTempRangeCount] = {{
{fast_steps}
}};

const uint32_t* const kCleanLut[kTempRangeCount] = {{
{clean_ptrs}
}};

const size_t kCleanLutStep[kTempRangeCount] = {{
{clean_steps}
}};

const uint8_t kDriveFrames[kTempRangeCount] = {{{drive_frames}}};

size_t tempRangeIndex(int tempC) {{
  for (size_t i = 0; i < kTempRangeCount; ++i) {{
    if (tempC < kTempRanges[i].maxC) return i;
  }}
  return kTempRangeCount - 1;
}}

}}  // namespace ed047tc2
}}  // namespace freeink
'''


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    tables, intervals = parse(sys.argv[1])
    impulses = check(tables, intervals)

    ranges = list(range(FIRST_RANGE, FIRST_RANGE + RANGE_COUNT))
    drives = [impulses[r][15] for r in ranges]
    grays = {r: pick_gray_levels(impulses[r]) for r in ranges}

    impulse_comment = "\n".join(
        "//   {:>2}..{:<2} C  L = [{}]".format(
            intervals[r][0], intervals[r][1], ", ".join("%2d" % v for v in impulses[r])
        )
        for r in ranges
    )
    gray_comment = "\n".join(
        "//   {:>2}..{:<2} C  dark  = level {:>2} ({:>3.0f}% to white)"
        "   light = level {:>2} ({:>3.0f}% to white)".format(
            intervals[r][0],
            intervals[r][1],
            grays[r][0],
            100.0 * impulses[r][grays[r][0]] / impulses[r][15],
            grays[r][1],
            100.0 * impulses[r][grays[r][1]] / impulses[r][15],
        )
        for r in ranges
    )

    # One bank pair per distinct (vector, grey pair); dedup by content.
    fast_banks = {}   # rows-tuple -> name
    clean_banks = {}
    fast_of = {}
    clean_of = {}
    worst_rows = 0
    for idx, r in enumerate(ranges):
        L = impulses[r]
        d, l = grays[r]
        fr = tuple(fast_rows(L[15], d, L[d], l, L[l]))
        cr = tuple(clean_rows(L))
        fast_of[r] = fast_banks.setdefault(fr, "kFastR%d" % idx)
        clean_of[r] = clean_banks.setdefault(cr, "kCleanR%d" % idx)
        # +1 per bank for the terminator row Panel_EPD copies too. Quality and
        # fastest are stubbed in the board config, so they cost one row each.
        total = ERASER_ROWS + 2 * UNUSED_SLOT_ROWS + (len(cr) + 1) + (len(fr) + 1)
        worst_rows = max(worst_rows, total)
        if total > LUT_ROW_BUDGET:
            raise SystemExit(
                "range %d: %d LUT rows exceeds Panel_EPD's %d-row budget"
                % (r, total, LUT_ROW_BUDGET)
            )
        fast_start = ERASER_ROWS + UNUSED_SLOT_ROWS + (len(cr) + 1)
        if fast_start > FAST_START_BUDGET:
            raise SystemExit(
                "range %d: fast bank starts at block %d > %d -- the 0x8000 fast"
                " flag would overflow Panel_EPD's uint16 step words and blank"
                " the display" % (r, fast_start, FAST_START_BUDGET)
            )

    body = HEADER.format(
        impulse_comment=impulse_comment, gray_comment=gray_comment, worst_rows=worst_rows
    )
    for rows, name in fast_banks.items():
        body += "// Differential bank: DU rails + self-normalizing AA grey columns.\n"
        body += "constexpr uint32_t %s[] = {\n%s\n    0u,\n};\n\n" % (name, "\n".join(rows))
    for rows, name in clean_banks.items():
        body += "// GC16-style clean: every level rail-normalizes, then lands exactly.\n"
        body += "constexpr uint32_t %s[] = {\n%s\n    0u,\n};\n\n" % (name, "\n".join(rows))

    body += FOOTER.format(
        ranges="\n".join(
            "    {%d, %d}," % (intervals[r][0], intervals[r][1]) for r in ranges
        ),
        gray_dark_levels=", ".join(str(grays[r][0]) for r in ranges),
        gray_light_levels=", ".join(str(grays[r][1]) for r in ranges),
        fast_ptrs="\n".join("    %s," % fast_of[r] for r in ranges),
        fast_steps="\n".join(
            "    sizeof(%s) / sizeof(%s[0])," % (fast_of[r], fast_of[r]) for r in ranges
        ),
        clean_ptrs="\n".join("    %s," % clean_of[r] for r in ranges),
        clean_steps="\n".join(
            "    sizeof(%s) / sizeof(%s[0])," % (clean_of[r], clean_of[r]) for r in ranges
        ),
        drive_frames=", ".join(str(d) for d in drives),
    )

    out = (
        Path(__file__).resolve().parent.parent
        / "libs/hardware/BoardT5S3/src/ED047TC2Waveform.cpp"
    )
    out.write_text(body, encoding="utf-8")
    print("wrote", out)
    for idx, r in enumerate(ranges):
        L = impulses[r]
        d, l = grays[r]
        print(
            "  range %2d  %2d..%-2d C  drive=%2d  fast=%s(%d rows)  clean=%s(%d rows)  greys=lvl %d/%d"
            % (
                r, intervals[r][0], intervals[r][1], L[15],
                fast_of[r], len(fast_rows(L[15], d, L[d], l, L[l])),
                clean_of[r], len(clean_rows(L)), d, l,
            )
        )


if __name__ == "__main__":
    main()
