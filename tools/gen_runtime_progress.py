#!/usr/bin/env python3
"""Generate Runtime/*/PROGRESS.md from native_registry.json + local .cpp symbols."""
from __future__ import annotations

import json
import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / "engine"
REGISTRY = ROOT / "docs" / "native_registry.json"

# Domain folder → java class prefixes (FQN startswith)
DOMAINS: dict[str, list[str]] = {
    "Runtime/System": [
        "java.lang.",
        "java.util.Config",
        "java.util.Vector",
        "java.util.VideoMode",
    ],
    "Runtime/IO": ["java.io."],
    "Runtime/Resources": ["java.util.resource."],
    "Runtime/Parts": ["java.game.parts."],
    "Runtime/Parts/Body": ["java.game.parts.bodypart."],
    "Runtime/Cars": [
        "java.game.Vehicle",
        "java.game.GameLogic",
        "java.game.Painter",
        "java.game.Navigator",
        "java.game.Player",
    ],
    "Runtime/Render": ["java.render."],
    "Runtime/Audio": ["java.sound."],
}

FN_RE = re.compile(
    r"^(?:void|int32_t|float|bool|InvObject\*)\s+(java_[\w]+)\(", re.M
)


def fqn_to_sym_prefix(fqn: str) -> str:
    return fqn.replace(".", "_")


def domain_for_class(cls: str) -> str | None:
    # Prefer most specific prefix (Body before Parts, etc.)
    best = None
    best_len = -1
    for folder, prefixes in DOMAINS.items():
        for p in prefixes:
            if cls.startswith(p) and len(p) > best_len:
                best = folder
                best_len = len(p)
    return best


def collect_symbols(folder: Path) -> set[str]:
    syms: set[str] = set()
    if not folder.exists():
        return syms
    for cpp in folder.rglob("*.cpp"):
        # Body is nested under Parts — skip nested domain folders when
        # scanning parent (Parts should not double-count Body).
        rel = cpp.relative_to(folder)
        if folder.name == "Parts" and rel.parts and rel.parts[0] == "Body":
            continue
        text = cpp.read_text(encoding="utf-8", errors="replace")
        syms.update(FN_RE.findall(text))
    return syms


def stub_like(cpp_path: Path, sym: str) -> bool | None:
    """Heuristic: body is stub if function is tiny / only (void)args / return 0."""
    text = cpp_path.read_text(encoding="utf-8", errors="replace")
    m = re.search(
        rf"^(?:void|int32_t|float|bool|InvObject\*)\s+{re.escape(sym)}\([^)]*\)\s*\{{",
        text,
        re.M,
    )
    if not m:
        return None
    start = m.end() - 1
    depth = 0
    i = start
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                body = text[start : i + 1]
                compact = re.sub(r"\s+", " ", body)
                if len(compact) < 80:
                    return True
                if re.search(r"\(void\)", compact) and len(compact) < 160:
                    return True
                return False
        i += 1
    return None


def find_sym_file(folder: Path, sym: str) -> Path | None:
    for cpp in folder.rglob("*.cpp"):
        if folder.name == "Parts" and "Body" in cpp.parts:
            # when searching Parts parent, ignore Body
            try:
                rel = cpp.relative_to(folder)
                if rel.parts and rel.parts[0] == "Body":
                    continue
            except ValueError:
                pass
        if sym in cpp.read_text(encoding="utf-8", errors="replace"):
            return cpp
    return None


def main() -> None:
    reg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    entries = reg["entries"]

    by_domain: dict[str, list[dict]] = defaultdict(list)
    unassigned: list[dict] = []
    for e in entries:
        d = domain_for_class(e["class"])
        if d:
            by_domain[d].append(e)
        else:
            unassigned.append(e)

    root_progress = []
    for folder, ents in sorted(by_domain.items()):
        path = ENGINE / folder
        syms = collect_symbols(path)
        # Parts/Body symbols are only in Body; Parts scan excludes Body.
        if folder == "Runtime/Parts":
            # Also count Body toward Parts rollup? Plan wants per-folder.
            # Keep separate: Parts folder only non-bodypart classes.
            ents = [
                e
                for e in ents
                if not e["class"].startswith("java.game.parts.bodypart.")
            ]

        by_class: dict[str, list[dict]] = defaultdict(list)
        for e in ents:
            by_class[e["class"]].append(e)

        lines = [
            f"# {folder}",
            "",
            f"Registry natives in this domain: **{len(ents)}**",
            "",
            "| Class | Registry | Host symbols | Notes |",
            "|---|---:|---:|---|",
        ]
        present = 0
        total = 0
        for cls, cl_ents in sorted(by_class.items()):
            total += len(cl_ents)
            prefix = fqn_to_sym_prefix(cls) + "_"
            found = [e for e in cl_ents if any(s.startswith(prefix) and s.endswith(e["name"].replace(".", "_")) or s == fqn_to_sym_prefix(cls) + "_" + e["name"] for s in syms)]
            # simpler match: java_game_parts_WheelRef_getPos
            hit = 0
            stub_n = 0
            body_n = 0
            for e in cl_ents:
                want = fqn_to_sym_prefix(cls) + "_" + e["name"]
                # handle overloads / _1 suffix loosely
                matches = [s for s in syms if s == want or s.startswith(want + "_")]
                if matches:
                    hit += 1
                    present += 1
                    fp = find_sym_file(path, matches[0])
                    if fp:
                        st = stub_like(fp, matches[0])
                        if st is True:
                            stub_n += 1
                        elif st is False:
                            body_n += 1
            note = []
            if stub_n:
                note.append(f"~{stub_n} thin")
            if body_n:
                note.append(f"~{body_n} body")
            lines.append(
                f"| `{cls}` | {len(cl_ents)} | {hit} | {', '.join(note) or '—'} |"
            )

        cpp_files = sorted(
            p.relative_to(ENGINE).as_posix()
            for p in path.rglob("*.cpp")
            if not (
                folder == "Runtime/Parts"
                and "Body" in p.relative_to(path).parts
            )
        )
        lines += [
            "",
            "## Sources",
            "",
        ]
        for f in cpp_files:
            lines.append(f"- `{f}`")
        lines += [
            "",
            f"Coverage (name match): **{present}/{total}**",
            "",
            "_Generated by `native/tools/gen_runtime_progress.py`._",
            "",
        ]
        out = path / "PROGRESS.md"
        out.write_text("\n".join(lines), encoding="utf-8")
        print(f"wrote {out.relative_to(ROOT)} ({present}/{total})")
        root_progress.append((folder, present, total))

    # Root Runtime PROGRESS
    rt = ENGINE / "Runtime" / "PROGRESS.md"
    lines = [
        "# Runtime progress",
        "",
        "Unity-like domain folders for the SLRR native rewrite.",
        "",
        "| Domain | Host hits | Registry |",
        "|---|---:|---:|",
    ]
    for folder, present, total in root_progress:
        lines.append(f"| `{folder}` | {present} | {total} |")
    if unassigned:
        lines += [
            "",
            f"Unassigned registry entries: {len(unassigned)}",
            "",
        ]
        for e in unassigned[:20]:
            lines.append(f"- `{e['class']}.{e['name']}`")
    lines += [
        "",
        "Regenerate: `python native/tools/gen_runtime_progress.py`",
        "",
    ]
    rt.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {rt.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
