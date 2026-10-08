"""Detect files the editor truncated.

On 2026-10-06 several files came back rewritten and cut at exactly 131070
characters (the 128 KB buffer cap). GameRef.cpp lost 72% of its body that way
and nothing flagged it until the compiler did, two tickets later.

Run after any pass that rewrites sources. Exit code 1 means something is cut.

  python native/tools/check_file_integrity.py            # full repo scan
  python native/tools/check_file_integrity.py a.cpp b.hpp  # just these
"""

import json
import os
import re
import sys

CAP = 131072  # the observed buffer cap, in characters
CODE = (".cpp", ".hpp", ".h", ".inc")
ROOTS = ("native/engine", "native/tools", "native/docs")
SKIP_DIRS = {"build", "__pycache__", ".git", "resconvert"}


def strip_literals(s):
    """Drop string/char literals and comments, keeping the rest of the code.

    Comments matter here: most files legitimately end on
    `}  // namespace inv`, whose last character is a letter.
    """
    out = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == "/" and i + 1 < n and s[i + 1] == "/":
            j = s.find("\n", i)
            if j < 0:
                break
            i = j
        elif c == "/" and i + 1 < n and s[i + 1] == "*":
            j = s.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c in "\"'":
            q, i = c, i + 1
            while i < n and s[i] != q:
                i += 2 if s[i] == "\\" else 1
            i += 1
            out.append("X")  # a literal is a complete token
        else:
            out.append(c)
            i += 1
    return "".join(out)


FRAG = re.compile(r"^(.*)_part(\d+)\.inc$")


def is_fragment(path):
    """A split fragment (see split_source.py) is only whole once concatenated."""
    return FRAG.match(os.path.basename(path)) is not None


def check(path, skip_braces=False):
    """Return a problem string, or None when the file looks whole."""
    try:
        raw = open(path, "rb").read()
    except OSError as e:
        return "unreadable: %s" % e
    if len(raw) == 0:
        return "EMPTY (0 bytes)"
    if len(raw) <= 2:
        return "EMPTY (%d bytes)" % len(raw)

    text = raw.decode("utf-8", "replace")
    ext = os.path.splitext(path)[1].lower()

    if ext in CODE and skip_braces:
        pass  # a fragment is checked as part of its group, below
    elif ext in CODE:
        code = strip_literals(text)
        opened, closed = code.count("{"), code.count("}")
        if opened != closed:
            return "UNBALANCED braces: %d open, %d close" % (opened, closed)
        tail = code.rstrip()
        if tail and tail[-1] not in "};)X":
            return "ends mid-token: %r" % text.rstrip()[-40:]
    elif ext == ".json":
        try:
            json.loads(text.lstrip("\ufeff"))
        except Exception as e:
            return "invalid JSON: %s" % str(e)[:60]
    elif ext == ".py":
        try:
            compile(text, path, "exec")
        except SyntaxError as e:
            return "SyntaxError at line %s" % e.lineno

    # A file sitting exactly on the cap is the signature of the 2026-10-06 loss.
    if abs(len(text) - CAP) <= 4:
        return "EXACTLY AT THE %d-CHAR CAP — almost certainly truncated" % CAP
    return None


def iter_repo():
    for root in ROOTS:
        for dp, dirs, files in os.walk(root):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for f in files:
                if f.endswith(CODE + (".py", ".json")):
                    yield os.path.join(dp, f)


def check_fragment_groups(targets):
    """Brace-check each split .cpp together with the fragments it includes."""
    problems = []
    for p in targets:
        if not p.endswith(".cpp"):
            continue
        try:
            shell = open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        frags = re.findall(r'#include\s+"([^"]*_part\d+\.inc)"', shell)
        if not frags:
            continue
        joined = strip_literals(shell)
        missing = []
        for f in frags:
            fp = os.path.join(os.path.dirname(p), f)
            if not os.path.exists(fp):
                missing.append(f)
                continue
            joined += strip_literals(
                open(fp, encoding="utf-8", errors="replace").read())
        if missing:
            problems.append((p, "missing fragments: %s" % ", ".join(missing)))
            continue
        o, c = joined.count("{"), joined.count("}")
        if o != c:
            problems.append(
                (p, "reassembled from %d fragments is UNBALANCED: %d open, %d close"
                 % (len(frags), o, c)))
    return problems


def main():
    targets = sys.argv[1:] or sorted(iter_repo())
    bad = []
    near = []
    for p in targets:
        problem = check(p, skip_braces=is_fragment(p))
        if problem:
            bad.append((p, problem))
        try:
            n = os.path.getsize(p)
        except OSError:
            continue
        if n > CAP * 0.78:  # within ~22% of the cap
            near.append((n, p))

    bad.extend(check_fragment_groups(targets))

    if bad:
        print("DAMAGED (%d):" % len(bad))
        for p, why in bad:
            print("  %-58s %s" % (p, why))
    else:
        print("ok: %d files intact" % len(targets))

    if near:
        near.sort(reverse=True)
        print()
        print("close to the %d-char cap (%d) — split candidates:" % (CAP, len(near)))
        for n, p in near:
            print("  %7.0f KB  %s" % (n / 1024, p))

    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
