"""Split an oversized .cpp into included fragments.

Why fragments and not separate translation units: these files carry a lot of
mutable state in anonymous namespaces (System.cpp 46 globals, render_d3d9.cpp
44, GameRef.cpp 37). Splitting them into real TUs would duplicate those blocks
and give each half its own copy of the state. Including fragments keeps the
translation unit byte-identical, so the split cannot change behaviour, and it
needs no CMake change since .inc files are not compiled on their own.

The point is the 128 KB editor buffer cap: on 2026-10-06 GameRef.cpp was
rewritten truncated at exactly 131070 characters and lost 72% of its body.
Keeping every file on disk well under the cap removes that failure mode.

  python native/tools/split_source.py <file.cpp> [--parts N] [--apply]

Without --apply it only reports the cut points it would use.
"""

import argparse
import os
import re
import sys

CAP = 131072
TARGET = 90 * 1024  # aim well under the cap so later edits stay safe


def preamble_end(lines, pp):
    """First line past the leading directive block.

    Any `#` directive counts, not just includes: the leading block routinely
    contains #ifdef _WIN32 / #define / #undef / #endif, and stopping in the
    middle of one leaves the shell file with an unterminated #if (C1070).
    `pp` is the preprocessor depth per line; the boundary must sit at depth 0.
    """
    last = 0
    for i, l in enumerate(lines):
        s = l.strip()
        if s.startswith("#"):
            last = i + 1
        elif s.startswith("//") or s == "":
            continue
        else:
            break
    while last < len(pp) and pp[last] != 0:
        last += 1
    return last


def pp_depth(lines):
    """Preprocessor conditional depth entering each line."""
    out = [0] * len(lines)
    p = 0
    for i, l in enumerate(lines):
        s = l.lstrip()
        if re.match(r"^#\s*endif", s):
            p -= 1
        out[i] = p
        if re.match(r"^#\s*if", s):
            p += 1
    return out


def cut_points(lines, start, parts, pp):
    """Pick `parts-1` cut lines at the shallowest blank lines available.

    A fragment boundary can sit anywhere -- the fragments are concatenated by
    the preprocessor -- but cutting at a blank line where brace depth is at a
    local minimum keeps each fragment readable on its own.
    """
    # Brace depth may be split freely -- the fragments are concatenated. A
    # conditional-compilation block may NOT: the preprocessor requires #if and
    # #endif to be balanced within one file (C1070), hence the `pp` guard.
    depth = [0] * len(lines)
    d = 0
    for i, l in enumerate(lines):
        depth[i] = d
        d += l.count("{") - l.count("}")

    total = sum(len(l) + 1 for l in lines[start:])
    chunk = total / parts
    cuts = []
    acc = 0
    want = chunk
    for i in range(start, len(lines)):
        acc += len(lines[i]) + 1
        if acc < want or len(cuts) >= parts - 1:
            continue
        # Walk forward to the next blank line at the shallowest depth nearby.
        # Keep the window tight: a wide one lets the first fragment overshoot
        # badly while chasing a depth-0 boundary far ahead.
        best, best_depth = None, None
        for j in range(i, min(i + 60, len(lines))):
            if lines[j].strip() or pp[j] != 0:
                continue
            if best_depth is None or depth[j] < best_depth:
                best, best_depth = j, depth[j]
            if depth[j] == 0:
                best, best_depth = j, 0
                break
        if best is None:
            # No usable blank line nearby (dense tables do this). Cut at the
            # first line outside any #if block: fragments are concatenated, so
            # a brace boundary is free but a preprocessor one is not.
            best = next((j for j in range(i, len(lines)) if pp[j] == 0), None)
            if best is None:
                continue
            best_depth = depth[best]
        cuts.append((best, best_depth))
        want += chunk
    return cuts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--parts", type=int, default=0)
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()

    raw = open(a.path, encoding="utf-8", errors="replace").read()
    nl = "\r\n" if "\r\n" in raw else "\n"
    lines = raw.replace("\r\n", "\n").split("\n")
    size = len(raw)

    pp = pp_depth(lines)
    parts = a.parts or max(2, -(-size // TARGET))
    start = preamble_end(lines, pp)
    cuts = cut_points(lines, start, parts, pp)
    if len(cuts) != parts - 1:
        print("could not find %d cut points (got %d)" % (parts - 1, len(cuts)))
        return 1

    base = os.path.splitext(os.path.basename(a.path))[0]
    bounds = [start] + [c[0] for c in cuts] + [len(lines)]
    print("%s  %.0f KB -> %d fragments (preamble ends line %d)"
          % (os.path.basename(a.path), size / 1024, parts, start))
    for n in range(parts):
        seg = lines[bounds[n]:bounds[n + 1]]
        nbytes = sum(len(l) + 1 for l in seg)
        depth = 0 if n == 0 else cuts[n - 1][1]
        print("  %s_part%d.inc  lines %5d-%-5d  %6.0f KB  (cut at depth %d)"
              % (base, n + 1, bounds[n] + 1, bounds[n + 1], nbytes / 1024, depth))
        if nbytes > CAP * 0.85:
            print("     WARNING: still close to the cap")

    if not a.apply:
        print("\n(dry run — pass --apply to write)")
        return 0

    outdir = os.path.dirname(a.path)
    for n in range(parts):
        seg = lines[bounds[n]:bounds[n + 1]]
        frag = os.path.join(outdir, "%s_part%d.inc" % (base, n + 1))
        header = [
            "// Fragment %d/%d of %s — included, not compiled on its own."
            % (n + 1, parts, os.path.basename(a.path)),
            "// Split only to stay under the 128 KB editor buffer cap; the",
            "// translation unit is unchanged. See native/tools/split_source.py.",
            "",
        ]
        open(frag, "w", encoding="utf-8", newline="").write(
            nl.join(header + seg))

    shell = lines[:start] + [
        "",
        "// Body split into fragments to stay under the 128 KB editor buffer",
        "// cap (see native/tools/split_source.py). They are concatenated here,",
        "// so this translation unit is identical to the single-file version.",
    ] + ['#include "%s_part%d.inc"' % (base, n + 1) for n in range(parts)] + [""]
    open(a.path, "w", encoding="utf-8", newline="").write(nl.join(shell))
    print("\nwrote %d fragments; %s is now %d lines"
          % (parts, os.path.basename(a.path), len(shell)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
