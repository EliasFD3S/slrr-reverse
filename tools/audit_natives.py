#!/usr/bin/env python3
"""Classify registered natives as empty / trivial / ok."""
import json
import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "engine"
OUT = Path(__file__).resolve().parent.parent / "docs" / "stub_audit.json"


from typing import Optional

def brace_body(text: str, start: int) -> Optional[str]:
    if start >= len(text) or text[start] != "{":
        return None
    depth = 0
    for j in range(start, min(len(text), start + 8000)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[start : j + 1]
    return None


def classify(body: str) -> str:
    raw = body
    b = re.sub(r"//.*?$|/\*.*?\*/", "", body, flags=re.S | re.M).strip()
    inner = b[1:-1].strip() if b.startswith("{") else b
    chunks = [c.strip() for c in inner.split(";") if c.strip()]
    if not chunks:
        return "empty"
    if all(re.match(r"\(void\)\w+", c) for c in chunks):
        return "empty"
    joined = " ".join(chunks)
    if re.fullmatch(r"(?:\(void\)\w+\s+)*return\s+0", joined):
        return "return0"
    if re.fullmatch(r"(?:\(void\)\w+\s+)*return\s+0\.f", joined):
        return "return0f"
    if re.fullmatch(r"(?:\(void\)\w+\s+)*return\s+nullptr", joined):
        return "nullptr"
    if re.search(r"\bTBD\b|\bTODO\b|not implemented|Full .* TBD", raw, re.I):
        return "marked"
    if len(inner) < 100 and inner.count(";") <= 2 and "return" in inner:
        return "trivial"
    return "ok"


def main() -> None:
    stubs_src = (ROOT / "Core" / "natives_stubs.cpp").read_text(
        encoding="utf-8", errors="replace"
    )
    entries = re.findall(
        r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(true|false)\s*,'
        r"\s*reinterpret_cast<void\*>\(\&([a-zA-Z0-9_]+)\)\s*\}",
        stubs_src,
    )

    bodies: dict[str, tuple[str, str]] = {}
    cpp_files = list((ROOT / "Runtime").rglob("*.cpp"))
    cpp_files += list((ROOT / "Core").rglob("*.cpp"))
    for f in cpp_files:
        if not f.exists():
            continue
        text = f.read_text(encoding="utf-8", errors="replace")
        rel = f.relative_to(ROOT).as_posix()
        for m in re.finditer(
            r"^(?:static\s+)?(?:void|int32_t|float|InvObject\*|int|bool|double)\s+"
            r"(java_[a-zA-Z0-9_]+)\s*\([^;{]*\)\s*\{",
            text,
            re.M,
        ):
            name = m.group(1)
            body = brace_body(text, m.end() - 1)
            if body:
                bodies[name] = (rel, body)

    by: dict[str, list] = defaultdict(list)
    for cls, meth, sig, static, fn in entries:
        if fn not in bodies:
            by["missing"].append(
                {
                    "class": cls,
                    "method": meth,
                    "sig": sig,
                    "fn": fn,
                    "static": static == "true",
                }
            )
            continue
        src, body = bodies[fn]
        cat = classify(body)
        by[cat].append(
            {
                "class": cls,
                "method": meth,
                "sig": sig,
                "fn": fn,
                "file": src,
                "bytes": len(body),
                "static": static == "true",
            }
        )

    weak_keys = ["empty", "return0", "return0f", "nullptr", "marked", "trivial", "missing"]
    summary = {k: len(by[k]) for k in list(by.keys())}
    summary["total"] = len(entries)
    summary["weak"] = sum(len(by[k]) for k in weak_keys)
    summary["ok"] = len(by["ok"])

    # Priority hints by class importance for gameplay
    hot_classes = {
        "java.lang.System",
        "java.util.resource.GameRef",
        "java.util.resource.RenderRef",
        "java.util.resource.ResourceRef",
        "java.util.resource.PhysicsRef",
        "java.game.parts.Part",
        "java.render.GfxEngine",
        "java.io.Input",
        "java.lang.Thread",
    }
    priority = []
    for k in weak_keys:
        for e in by[k]:
            score = 0
            if e["class"] in hot_classes:
                score += 3
            if k in ("empty", "return0", "nullptr", "missing"):
                score += 2
            if k == "marked":
                score += 1
            name = e["method"]
            # Declared leftovers: unused / debug empty natives (keep return-0).
            if name in (
                "netHost",
                "netJoin",
                "netLeave",
                "analyze",
                "gc",
                "runFinalization",
                "finalize",
                "enableDebugDump",
            ):
                score -= 5
            if name in (
                "wait",
                "notify",
                "start",
                "stop",
                "sleep",
            ):
                score += 1
            priority.append({**e, "cat": k, "score": score})
    priority.sort(key=lambda x: (-x["score"], x["class"], x["method"]))

    leftover_names = {
        "netHost",
        "netJoin",
        "netLeave",
        "analyze",
        "gc",
        "runFinalization",
        "finalize",
        "enableDebugDump",
    }
    leftovers = [
        {"class": cls, "method": meth, "sig": sig, "fn": fn}
        for cls, meth, sig, static, fn in entries
        if meth in leftover_names
    ]

    report = {
        "summary": summary,
        "by_category": {k: by[k] for k in weak_keys if by[k]},
        "ok_count": len(by["ok"]),
        "leftovers": leftovers,
        "priority": priority[:40],
        "by_file_weak": {},
    }
    file_counts: dict[str, int] = defaultdict(int)
    for k in weak_keys:
        for e in by[k]:
            file_counts[e.get("file", "?")] += 1
    report["by_file_weak"] = dict(sorted(file_counts.items(), key=lambda x: -x[1]))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(json.dumps(summary, indent=2))
    print("\n=== PRIORITY (top 25) ===")
    for e in priority[:25]:
        print(f"  [{e['cat']:8}] {e['class']}.{e['method']}  ({e.get('file','?')})")
    print(f"\nwrote {OUT}")


if __name__ == "__main__":
    main()
