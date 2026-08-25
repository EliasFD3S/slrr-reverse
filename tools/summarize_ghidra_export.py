#!/usr/bin/env python3
"""Turn Ghidra TSV exports into JVM/native anchor notes.

Usage:
  python native/tools/summarize_ghidra_export.py
"""
from __future__ import annotations

import csv
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXPORT = ROOT / "native" / "ghidra" / "exports"
OUT = ROOT / "native" / "docs" / "jvm_anchors.md"

PRIORITY = (
    "Natives.cpp",
    "JVM::compileSource",
    "JVM::addClass",
    "JVM::getClass",
    "JVM::",
    "Invictus",
    "GameRef",
    "Direct3DCreate9",
    "script error",
)


def load_tsv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def main() -> int:
    strings = load_tsv(EXPORT / "jvm_strings.tsv")
    summary = (EXPORT / "summary.txt").read_text(encoding="utf-8") if (EXPORT / "summary.txt").is_file() else ""
    functions = load_tsv(EXPORT / "functions.tsv")
    imports = load_tsv(EXPORT / "imports.tsv")

    lines: list[str] = []
    lines.append("# JVM / natives analysis anchors")
    lines.append("")
    lines.append("Generated from `native/ghidra/exports/` by `summarize_ghidra_export.py`.")
    lines.append("")
    if summary:
        lines.append("## Program summary")
        lines.append("")
        lines.append("```")
        lines.append(summary.strip())
        lines.append("```")
        lines.append("")
    lines.append(f"- Functions exported: **{len(functions)}**")
    lines.append(f"- Imports exported: **{len(imports)}**")
    lines.append(f"- Interesting strings: **{len(strings)}**")
    lines.append("")

    # Group imports by library
    by_lib: dict[str, list[str]] = defaultdict(list)
    for row in imports:
        by_lib[row.get("library") or "?"].append(row.get("name") or "")
    lines.append("## Imports (by DLL)")
    lines.append("")
    for lib in sorted(by_lib):
        names = sorted(set(by_lib[lib]))
        lines.append(f"### `{lib}` ({len(names)})")
        lines.append("")
        for n in names[:40]:
            lines.append(f"- `{n}`")
        if len(names) > 40:
            lines.append(f"- … +{len(names) - 40} more")
        lines.append("")

    lines.append("## Priority string xrefs")
    lines.append("")
    lines.append("| Priority | Address | Xrefs | String |")
    lines.append("|---|---|---|---|")

    used: set[str] = set()
    prioritized: list[dict[str, str]] = []
    for key in PRIORITY:
        for row in strings:
            s = row.get("string") or ""
            if key in s and row.get("address") not in used:
                prioritized.append(row)
                used.add(row.get("address") or "")
    for row in strings:
        if row.get("address") not in used:
            prioritized.append(row)
            used.add(row.get("address") or "")

    for row in prioritized[:80]:
        s = (row.get("string") or "")[:90].replace("|", "/")
        lines.append(
            f"| | `{row.get('address')}` | {row.get('xref_count')} | `{s}` |"
        )

    lines.append("")
    lines.append("## Suggested Ghidra renames")
    lines.append("")
    lines.append("For each string with xrefs, open the from-address function and rename:")
    lines.append("")
    rename_hints = [
        ("Natives.cpp", "Natives_cpp_path / RegisterNatives vicinity"),
        ("JVM::compileSource", "JVM_compileSource"),
        ("JVM::addClass", "JVM_addClass"),
        ("JVM::getClass", "JVM_getClass"),
        ("JVM::assignmentConversion", "JVM_assignmentConversion"),
        ("Direct3DCreate9", "import thunk (already named)"),
    ]
    for needle, hint in rename_hints:
        hits = [r for r in strings if needle in (r.get("string") or "")]
        if not hits:
            lines.append(f"- `{needle}` — not in export yet")
            continue
        top = hits[0]
        xrefs = top.get("xrefs") or ""
        first_fn = ""
        if "@" in xrefs:
            first_fn = xrefs.split(";")[0].split("@")[-1]
        lines.append(
            f"- `{needle}` @ `{top.get('address')}` → rename `{first_fn or '?'}` as **{hint}**"
        )

    lines.append("")
    lines.append("See also [abi_bridge.md](abi_bridge.md).")
    lines.append("")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {OUT} ({len(strings)} strings, {len(functions)} functions)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
