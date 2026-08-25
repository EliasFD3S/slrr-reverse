#!/usr/bin/env python3
"""Build natives inventory from Java sources + PE strings.

Usage (from repo root):
  python native/tools/inventory_natives.py
  python native/tools/inventory_natives.py --json native/docs/natives_inventory.json
"""
from __future__ import annotations

import argparse
import json
import re
import struct
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ROOT / "sources"
EXE = ROOT / "Street Legal Racing - Redline" / "StreetLegal_Redline.exe"
DOCS = ROOT / "native" / "docs"

NATIVE_RE = re.compile(
    r"(?P<mods>(?:(?:public|private|protected|static|final|synchronized|native)\s+)+)"
    r"(?P<ret>[\w.\[\]<>,\s]+?)\s+"
    r"(?P<name>\w+)\s*\((?P<args>[^)]*)\)\s*;",
    re.MULTILINE,
)
CLASS_RE = re.compile(r"\b(?:public\s+)?(?:abstract\s+)?(?:final\s+)?class\s+(\w+)")
PACKAGE_RE = re.compile(r"^\s*package\s+([\w.]+)\s*;", re.MULTILINE)

# Invictus FQN from on-disk layout (PE uses Ljava.lang.System; etc.).
# Explicit package declarations (rare) win when present.
_PATH_FQN_RULES: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"^system/Scripts/lang/(?:src/)?", re.I), "java.lang."),
    (re.compile(r"^system/Scripts/io/(?:src/)?", re.I), "java.io."),
    (re.compile(r"^system/Scripts/render/(?:src/)?", re.I), "java.render."),
    (re.compile(r"^system/Scripts/sound/(?:src/)?", re.I), "java.sound."),
    (re.compile(r"^system/Scripts/util/resource/(?:src/)?", re.I), "java.util.resource."),
    (re.compile(r"^system/Scripts/util/(?:src/)?", re.I), "java.util."),
    (re.compile(r"^sl/Scripts/game/(?:src/)?", re.I), "java.game."),
    (re.compile(r"^parts/scripts/bodypart/(?:SRC/|src/)?", re.I), "java.game.parts.bodypart."),
    (re.compile(r"^parts/scripts/(?:src/)?", re.I), "java.game.parts."),
]


def java_rel_class(path: Path, sources_root: Path) -> str:
    return path.relative_to(sources_root).as_posix()


def infer_fqn(path: Path, sources_root: Path, package: str, classname: str) -> str:
    if package:
        return f"{package}.{classname}"
    rel = java_rel_class(path, sources_root)
    for pat, prefix in _PATH_FQN_RULES:
        if pat.search(rel):
            return prefix + classname
    return classname


def parse_native_methods(path: Path, sources_root: Path) -> list[dict]:
    text = path.read_text(encoding="utf-8", errors="replace")
    pkg_m = PACKAGE_RE.search(text)
    package = pkg_m.group(1) if pkg_m else ""
    class_m = CLASS_RE.search(text)
    classname = class_m.group(1) if class_m else path.stem
    fqn = infer_fqn(path, sources_root, package, classname)

    out: list[dict] = []
    for m in NATIVE_RE.finditer(text):
        mods = m.group("mods")
        if "native" not in mods.split():
            continue
        args = re.sub(r"\s+", " ", m.group("args").strip())
        out.append(
            {
                "file": java_rel_class(path, sources_root),
                "package": package or ".".join(fqn.split(".")[:-1]),
                "class": classname,
                "fqn": fqn,
                "modifiers": " ".join(mods.split()),
                "return": m.group("ret").strip(),
                "name": m.group("name"),
                "args": args,
                "signature": f"{m.group('ret').strip()} {m.group('name')}({args})",
            }
        )
    return out

def extract_pe_strings(data: bytes, min_len: int = 6) -> list[str]:
    strings: list[str] = []
    for m in re.finditer(rb"[\x20-\x7e]{" + str(min_len).encode() + rb",}", data):
        strings.append(m.group().decode("ascii"))
    return strings


def pe_interesting(strings: list[str]) -> dict:
    keys = (
        "JVM::",
        "compileSource",
        "Invictus",
        "GameRef",
        "Natives.cpp",
        "Direct3D",
        "d3d9",
        "script error",
        "no such class",
        "Native",
        "Ljava.",
    )
    buckets: dict[str, list[str]] = defaultdict(list)
    path_hints: list[str] = []
    for s in strings:
        if "\\" in s and (".cpp" in s.lower() or ".h" in s.lower() or "Invictus" in s):
            path_hints.append(s)
        for k in keys:
            if k.lower() in s.lower():
                buckets[k].append(s)
                break
    # dedupe preserve order
    for k, vals in list(buckets.items()):
        seen: set[str] = set()
        uniq: list[str] = []
        for v in vals:
            if v not in seen:
                seen.add(v)
                uniq.append(v)
        buckets[k] = uniq
    path_seen: set[str] = set()
    paths: list[str] = []
    for p in path_hints:
        if p not in path_seen:
            path_seen.add(p)
            paths.append(p)
    return {"by_key": dict(buckets), "source_paths": paths}


def pe_sections(data: bytes) -> list[dict]:
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    machine, numsec, ts, _, _, optsize, chars = struct.unpack_from(
        "<HHIIIHH", data, e_lfanew + 4
    )
    sec_off = e_lfanew + 24 + optsize
    sections = []
    for i in range(numsec):
        off = sec_off + i * 40
        name = data[off : off + 8].split(b"\0")[0].decode("latin1")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        sections.append(
            {
                "name": name,
                "va": f"0x{va:08X}",
                "vsize": f"0x{vsize:08X}",
                "raw": f"0x{raw:08X}",
                "rsize": f"0x{rsize:08X}",
            }
        )
    return {
        "machine": f"0x{machine:04X}",
        "timestamp": ts,
        "characteristics": f"0x{chars:04X}",
        "sections": sections,
    }


def write_markdown(inventory: dict, path: Path) -> None:
    lines: list[str] = []
    lines.append("# Natives inventory — Street Legal Racing Redline")
    lines.append("")
    lines.append("Generated by `native/tools/inventory_natives.py`.")
    lines.append("")
    pe = inventory["pe"]
    lines.append("## Binary")
    lines.append("")
    lines.append(f"- Path: `{pe['path']}`")
    lines.append(f"- Size: {pe['size']} bytes")
    lines.append(f"- Machine: {pe['headers']['machine']} (x86 PE32)")
    lines.append("")
    lines.append("| Section | VA | VSize | Raw | RSize |")
    lines.append("|---|---|---|---|---|")
    for s in pe["headers"]["sections"]:
        lines.append(
            f"| `{s['name']}` | {s['va']} | {s['vsize']} | {s['raw']} | {s['rsize']} |"
        )
    lines.append("")
    lines.append("## Source path hints (PE strings)")
    lines.append("")
    for p in pe["strings"]["source_paths"][:40]:
        lines.append(f"- `{p}`")
    lines.append("")
    lines.append("## JVM / engine strings (sample)")
    lines.append("")
    for key in ("JVM::", "compileSource", "Invictus", "Natives.cpp", "GameRef"):
        vals = pe["strings"]["by_key"].get(key, [])
        lines.append(f"### `{key}` ({len(vals)})")
        lines.append("")
        for v in vals[:25]:
            lines.append(f"- `{v}`")
        if len(vals) > 25:
            lines.append(f"- … +{len(vals) - 25} more")
        lines.append("")

    by_class = inventory["natives_by_class"]
    lines.append("## Java `native` methods")
    lines.append("")
    lines.append(f"Total: **{inventory['native_count']}** methods in **{len(by_class)}** classes.")
    lines.append("")
    for fqn in sorted(by_class.keys()):
        methods = by_class[fqn]
        file = methods[0]["file"]
        lines.append(f"### `{fqn}`")
        lines.append("")
        lines.append(f"File: `{file}` — {len(methods)} natives")
        lines.append("")
        for m in methods:
            lines.append(f"- `{m['signature']}`")
        lines.append("")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", type=Path, default=DOCS / "natives_inventory.json")
    ap.add_argument("--md", type=Path, default=DOCS / "natives_inventory.md")
    ap.add_argument("--sources", type=Path, default=SOURCES)
    ap.add_argument("--exe", type=Path, default=EXE)
    args = ap.parse_args()

    natives: list[dict] = []
    for java in sorted(args.sources.rglob("*.java")):
        natives.extend(parse_native_methods(java, args.sources))

    by_class: dict[str, list[dict]] = defaultdict(list)
    for n in natives:
        by_class[n["fqn"]].append(n)

    pe_info: dict = {"path": str(args.exe), "size": 0, "headers": {}, "strings": {}}
    if args.exe.is_file():
        data = args.exe.read_bytes()
        pe_info["size"] = len(data)
        pe_info["headers"] = pe_sections(data)
        pe_info["strings"] = pe_interesting(extract_pe_strings(data))

    inventory = {
        "native_count": len(natives),
        "class_count": len(by_class),
        "natives": natives,
        "natives_by_class": dict(by_class),
        "pe": pe_info,
    }

    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(inventory, indent=2), encoding="utf-8")
    write_markdown(inventory, args.md)
    print(f"Wrote {args.json} ({len(natives)} natives, {len(by_class)} classes)")
    print(f"Wrote {args.md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
