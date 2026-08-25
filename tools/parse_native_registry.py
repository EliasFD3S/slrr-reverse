#!/usr/bin/env python3
"""Parse Ghidra decompile of Natives_Register* into a JSON registry.

Resolves string symbols like s_java_lang_System_00613bc0 / DAT_00613bb8
by reading the PE image at VA (ImageBase 0x00400000).

Usage:
  python native/tools/parse_native_registry.py
"""
from __future__ import annotations

import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / "Street Legal Racing - Redline" / "StreetLegal_Redline.exe"
DECOMP = ROOT / "native" / "ghidra" / "exports" / "decompile"
OUT_JSON = ROOT / "native" / "docs" / "native_registry.json"
OUT_MD = ROOT / "native" / "docs" / "native_registry.md"

IMAGE_BASE = 0x00400000
CALL_RE = re.compile(
    r"FUN_00416b00\s*\(\s*"
    r"(?P<a>[^,]+)\s*,\s*"
    r"(?P<b>[^,]+)\s*,\s*"
    r"(?P<c>[^,]+)\s*,\s*"
    r"(?P<d>[^)]+)\)",
    re.MULTILINE,
)
SYM_RE = re.compile(
    r"(?:s_[A-Za-z0-9_<>]+_|DAT_|LAB_|FUN_)(?P<va>[0-9A-Fa-f]{6,8})"
)


def load_pe_va_map(data: bytes) -> dict[int, int]:
    """Map VA -> file offset for readable sections."""
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    optsize = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    numsec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    sec_off = e_lfanew + 24 + optsize
    mapping = {}
    for i in range(numsec):
        off = sec_off + i * 40
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        size = max(vsize, rsize)
        for rel in range(0, size, 1):
            # don't store per-byte; just keep section ranges
            pass
        mapping[va] = (raw, rsize, vsize)
    return mapping


def va_to_off(sections: dict[int, tuple[int, int, int]], va: int) -> int | None:
    rva = va - IMAGE_BASE
    for sec_va, (raw, rsize, vsize) in sections.items():
        if sec_va <= rva < sec_va + max(vsize, rsize):
            return raw + (rva - sec_va)
    return None


def read_cstr(data: bytes, sections: dict, va: int, maxlen: int = 256) -> str | None:
    off = va_to_off(sections, va)
    if off is None or off < 0 or off >= len(data):
        return None
    end = data.find(b"\0", off, off + maxlen)
    if end < 0:
        end = off + maxlen
    try:
        return data[off:end].decode("ascii", "replace")
    except Exception:
        return None


def sym_va(token: str) -> int | None:
    token = token.strip().lstrip("&")
    m = SYM_RE.search(token)
    if not m:
        return None
    return int(m.group("va"), 16)


def parse_file(text: str, data: bytes, sections: dict) -> list[dict]:
    # flatten continued lines lightly
    flat = re.sub(r",\s*\n\s*", ", ", text)
    flat = re.sub(r"\n\s*", " ", flat)
    out: list[dict] = []
    for m in CALL_RE.finditer(flat):
        parts = [m.group(x).strip() for x in ("a", "b", "c", "d")]
        vas = [sym_va(p) for p in parts]
        cls = read_cstr(data, sections, vas[0]) if vas[0] else None
        name = read_cstr(data, sections, vas[1]) if vas[1] else None
        sig = read_cstr(data, sections, vas[2]) if vas[2] else None
        impl = parts[3].lstrip("&")
        impl_va = vas[3]
        out.append(
            {
                "class": cls,
                "name": name,
                "signature": sig,
                "impl_symbol": impl,
                "impl_va": f"0x{impl_va:08X}" if impl_va else None,
                "class_va": f"0x{vas[0]:08X}" if vas[0] else None,
                "name_va": f"0x{vas[1]:08X}" if vas[1] else None,
                "sig_va": f"0x{vas[2]:08X}" if vas[2] else None,
            }
        )
    return out


def main() -> int:
    data = EXE.read_bytes()
    sections = load_pe_va_map(data)
    entries: list[dict] = []
    sources = []
    for path in sorted(DECOMP.glob("Natives_Register*.c")):
        chunk = parse_file(path.read_text(encoding="utf-8", errors="replace"), data, sections)
        for e in chunk:
            e["register_fn"] = path.stem
        entries.extend(chunk)
        sources.append(path.name)

    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "register_helper": "FUN_00416b00",
        "register_helper_va": "0x00416B00",
        "sources": sources,
        "count": len(entries),
        "entries": entries,
    }
    OUT_JSON.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    lines = [
        "# Native registry (from Ghidra decompile)",
        "",
        f"Helper: `FUN_00416b00(class, name, signature, fn*)` @ `0x00416B00`",
        f"Entries parsed: **{len(entries)}** from `{', '.join(sources)}`",
        "",
        "| Class | Method | Signature | Impl VA |",
        "|---|---|---|---|",
    ]
    for e in entries:
        lines.append(
            f"| `{e.get('class')}` | `{e.get('name')}` | `{e.get('signature')}` | `{e.get('impl_va')}` |"
        )
    lines.append("")
    OUT_MD.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {OUT_JSON} ({len(entries)} entries)")
    print(f"Wrote {OUT_MD}")
    # quick sanity
    missing = sum(1 for e in entries if not e.get("class") or not e.get("name"))
    print(f"unresolved class/name: {missing}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
