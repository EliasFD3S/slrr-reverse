#!/usr/bin/env python3
"""Measure reverse-host progress toward stock StreetLegal_Redline.exe.

Writes native/docs/progress_global.json and refreshes the Cursor canvas
SNAPSHOT block (slrr-stock-progress.canvas.tsx).

Layers (do not collapse into one undocumented %):
  1. JNI table — PE-commented vs non-thin body vs thin (376 kNativeTable)
  2. Boot smoke — splash → menu → garage/city harness
  3. Engine internals — PE clusters not in the JNI table

Stock index (headline, conservative):
  0.10 * boot + 0.70 * (pe / table) + 0.20 * (engine_done / engine_n)

This is NOT Runtime/*/PROGRESS.md name-match coverage.
"""
from __future__ import annotations

import json
import re
from collections import defaultdict
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / "native" / "engine"
STUBS = ENGINE / "Core" / "natives_stubs.cpp"
RUNTIME = ENGINE / "Runtime"
OUT_JSON = ROOT / "native" / "docs" / "progress_global.json"
CANVAS = (
    Path.home()
    / ".cursor"
    / "projects"
    / "c-Users-Elias-Desktop-re-engineering"
    / "canvases"
    / "slrr-stock-progress.canvas.tsx"
)

DOMAINS: dict[str, list[str]] = {
    "System": ["java.lang.", "java.util.Config", "java.util.Vector", "java.util.VideoMode"],
    "IO": ["java.io."],
    "Resources": ["java.util.resource."],
    "Parts": ["java.game.parts."],
    "Body": ["java.game.parts.bodypart."],
    "Cars": [
        "java.game.Vehicle",
        "java.game.GameLogic",
        "java.game.Painter",
        "java.game.Navigator",
        "java.game.Player",
    ],
    "Render": ["java.render."],
    "Audio": ["java.sound."],
}

ENTRY_RE = re.compile(
    r'\{\s*"(java\.[^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(true|false)\s*,'
    r"\s*reinterpret_cast<void\*>\(&([A-Za-z0-9_]+)\)",
)
PE_RE = re.compile(r"(?:PE\s*@\s*0x[0-9A-Fa-f]+|Stock\s+0x[0-9A-Fa-f]{6,8})", re.I)
FN_START_RE = re.compile(
    r"^(?:void|int32_t|int|float|double|bool|InvObject\*)\s+(java_[A-Za-z0-9_]+)\s*\(",
    re.M,
)
THIN_RETURN_RE = re.compile(
    r"^\s*(?:return(?:\s+(?:0|0\.f|0\.0f|nullptr|NULL|false|true))?|)\s*;\s*$"
)
STUB_HINT_RE = re.compile(r"\b(?:stub|no-?op|nyi|not implemented|TODO)\b", re.I)

# PE clusters outside the JNI table. Agents mark done=true when the host
# matches the stock routine (not merely a boot-compatible shim).
DEFAULT_ENGINE = [
    {
        "id": "setParent_lists",
        "title": "GameRef.setParent child lists +0x30/+0x38",
        "va": "0x0047E2D0",
        "done": False,
        "note": "sub_419860 out of scope",
    },
    {
        "id": "navigator_update",
        "title": "Navigator.updateNavigator body 0x6c4",
        "va": "0x00482D30",
        "done": False,
        "note": "map/car mode; viewport clamp",
    },
    {
        "id": "queueEvent_parse",
        "title": "queueEvent parser sub_458C00",
        "va": "0x0047DA30",
        "done": False,
        "note": "wakeup/sethorn/start/stop/reset",
    },
    {
        "id": "fog_vtable",
        "title": "Camera.setFog → sub_5447D0",
        "va": "0x00486570",
        "done": False,
        "note": "TREE *10 ported; camera vtable not",
    },
    {
        "id": "sfx_3d_cull",
        "title": "SfxRef.nplay 3D cull",
        "va": "0x00480D40",
        "done": False,
        "note": "null/id0 → -1 ported",
    },
    {
        "id": "force_rendering_7",
        "title": "GfxEngine.forceRendering 7 pumps",
        "va": "0x0047C1D0",
        "done": False,
        "note": "host = 1 Present",
    },
    {
        "id": "render_4arg",
        "title": "RenderRef.setMatrix 4-arg parent-link",
        "va": "0x004810B0",
        "done": False,
        "note": "no bindBone; 0 stock Java call sites",
    },
    {
        "id": "isempty_type8",
        "title": "GameRef.isEmpty RESTYPE_GAME=8",
        "va": "0x00486D10",
        "done": False,
        "note": "host = empty flag",
    },
    {
        "id": "getscript_split",
        "title": "getScriptInstance type 1 / 8",
        "va": "0x00486F30",
        "done": False,
        "note": "sub_404E20 not ported",
    },
    {
        "id": "mainloop_1to1",
        "title": "Engine_MainLoop tick/render 1:1",
        "va": "0x00428960",
        "done": False,
        "note": "GameInit + loop; JVM inside the loop",
    },
]

W_BOOT = 0.10
W_PE = 0.70
W_ENGINE = 0.20


def domain_for_class(cls: str) -> str:
    best = "Other"
    best_len = -1
    for folder, prefixes in DOMAINS.items():
        for p in prefixes:
            if cls.startswith(p) and len(p) > best_len:
                best = folder
                best_len = len(p)
    return best


def extract_body(src: str, start: int) -> str:
    brace = src.find("{", start)
    if brace < 0:
        return ""
    depth = 0
    i = brace
    while i < len(src):
        c = src[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return src[brace : i + 1]
        i += 1
    return src[brace:]


def strip_comments(body: str) -> str:
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    body = re.sub(r"//.*?$", " ", body, flags=re.M)
    return body


def is_thin(body: str) -> bool:
    code = strip_comments(body)
    inner = code.strip()
    if inner.startswith("{") and inner.endswith("}"):
        inner = inner[1:-1]
    lines = [ln.strip() for ln in inner.splitlines() if ln.strip()]
    if not lines:
        return True
    if STUB_HINT_RE.search(body) and len(lines) <= 6:
        return True
    meaningful = 0
    for ln in lines:
        if ln in ("{", "}") or ln.startswith("if (!self") or ln.startswith("if (!self)"):
            continue
        if THIN_RETURN_RE.match(ln) or ln in ("(void)self;", "(void)self;"):
            continue
        if ln.startswith("using ") or ln.startswith("auto&"):
            continue
        meaningful += 1
    return meaningful <= 2


def index_runtime_fns() -> dict[str, dict]:
    out: dict[str, dict] = {}
    for cpp in RUNTIME.rglob("*.cpp"):
        src = cpp.read_text(encoding="utf-8", errors="replace")
        for m in FN_START_RE.finditer(src):
            name = m.group(1)
            body = extract_body(src, m.start())
            out[name] = {
                "file": str(cpp.relative_to(ENGINE)).replace("\\", "/"),
                "nloc": body.count("\n"),
                "pe": bool(PE_RE.search(body)),
                "thin": is_thin(body),
            }
    return out


def parse_table() -> list[dict]:
    text = STUBS.read_text(encoding="utf-8", errors="replace")
    rows = []
    for m in ENTRY_RE.finditer(text):
        rows.append(
            {
                "cls": m.group(1),
                "name": m.group(2),
                "sig": m.group(3),
                "static": m.group(4) == "true",
                "fn": m.group(5),
            }
        )
    return rows


def read_text_auto(path: Path) -> str:
    raw = path.read_bytes()
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        return raw.decode("utf-16")
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw.decode("utf-8-sig")
    return raw.decode("utf-8", errors="replace")


def latest_smoke() -> dict:
    build_dir = ENGINE / "build"
    smokes = sorted(build_dir.glob("game_smoke_race*.txt"), key=lambda p: p.stat().st_mtime)
    if not smokes:
        return {"file": None, "boot_pct": None, "exit_ok": None, "build": None}
    path = smokes[-1]
    text = read_text_auto(path)
    boot_pct = None
    m = re.search(r"boot progress ~(\d+)%", text)
    if m:
        boot_pct = int(m.group(1))
    build = None
    m = re.search(r"build=(\d+)", text)
    if m:
        build = int(m.group(1))
    exit_ok = "EXIT=0" in text or "boot progress ~100%" in text
    return {
        "file": path.name,
        "boot_pct": boot_pct,
        "exit_ok": exit_ok,
        "build": build,
    }


def load_prev() -> dict:
    if OUT_JSON.exists():
        try:
            return json.loads(OUT_JSON.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return {}
    return {}


def merge_engine(prev: dict) -> list[dict]:
    prev_list = prev.get("engine_clusters") or (prev.get("engine") or {}).get("clusters") or []
    prev_map = {e["id"]: e for e in prev_list if isinstance(e, dict) and e.get("id")}
    out = []
    for default in DEFAULT_ENGINE:
        old = prev_map.get(default["id"], {})
        item = dict(default)
        if "done" in old:
            item["done"] = bool(old["done"])
        if old.get("note") and not item.get("note"):
            item["note"] = old["note"]
        out.append(item)
    return out


def pct(num: int, den: int) -> float:
    if den <= 0:
        return 0.0
    return round(100.0 * num / den, 1)


def classify(rows: list[dict], fns: dict[str, dict]) -> list[dict]:
    classified = []
    for r in rows:
        info = fns.get(r["fn"], {})
        pe = bool(info.get("pe"))
        thin = bool(info.get("thin", True)) if info else True
        if pe:
            kind = "pe"
            thin = False
        elif not info:
            kind = "missing"
            thin = True
        elif thin:
            kind = "thin"
        else:
            kind = "body"
        classified.append(
            {
                **r,
                "domain": domain_for_class(r["cls"]),
                "kind": kind,
                "file": info.get("file"),
                "nloc": info.get("nloc", 0),
            }
        )
    return classified


def build_snapshot(classified: list[dict], engine: list[dict], smoke: dict) -> dict:
    n = len(classified)
    pe = sum(1 for x in classified if x["kind"] == "pe")
    body = sum(1 for x in classified if x["kind"] == "body")
    thin = sum(1 for x in classified if x["kind"] in ("thin", "missing"))
    missing = sum(1 for x in classified if x["kind"] == "missing")
    nonthin = pe + body
    eng_n = len(engine)
    eng_done = sum(1 for e in engine if e.get("done"))
    boot = float(smoke.get("boot_pct") or 0)
    pe_pct = pct(pe, n)
    body_pct = pct(nonthin, n)
    eng_pct = pct(eng_done, eng_n)
    stock = round(W_BOOT * boot + W_PE * pe_pct + W_ENGINE * eng_pct, 1)

    by_domain: dict[str, dict[str, int]] = defaultdict(lambda: {"pe": 0, "body": 0, "thin": 0, "total": 0})
    for x in classified:
        d = by_domain[x["domain"]]
        d["total"] += 1
        bucket = "thin" if x["kind"] in ("thin", "missing") else x["kind"]
        d[bucket] += 1

    domain_rows = []
    for name in ["Resources", "Parts", "Body", "Render", "System", "IO", "Audio", "Cars", "Other"]:
        d = by_domain.get(name)
        if not d:
            continue
        domain_rows.append(
            {
                "domain": name,
                "pe": d["pe"],
                "body": d["body"],
                "thin": d["thin"],
                "total": d["total"],
                "pe_pct": pct(d["pe"], d["total"]),
                "nonthin_pct": pct(d["pe"] + d["body"], d["total"]),
            }
        )

    return {
        "updated": date.today().isoformat(),
        "formula": f"{W_BOOT}*boot + {W_PE}*(pe/table) + {W_ENGINE}*(engine_done/n)",
        "stock_index_pct": stock,
        "jni": {
            "table": n,
            "pe": pe,
            "body": body,
            "thin": thin,
            "missing": missing,
            "nonthin": nonthin,
            "pe_pct": pe_pct,
            "nonthin_pct": body_pct,
        },
        "boot": {
            "pct": boot,
            "smoke": smoke.get("file"),
            "build": smoke.get("build"),
            "exit_ok": smoke.get("exit_ok"),
        },
        "engine": {
            "done": eng_done,
            "total": eng_n,
            "pct": eng_pct,
            "clusters": engine,
        },
        "domains": domain_rows,
        "weights": {"boot": W_BOOT, "pe": W_PE, "engine": W_ENGINE},
        "caveat": (
            "JNI is not the whole PE. The index weights 70% the 1:1 native "
            "table, 20% non-JNI engine clusters, 10% boot smoke. A 'body' "
            "native without a VA does not count toward pe_pct."
        ),
    }


def append_history(prev: dict, snap: dict) -> list[dict]:
    hist = list(prev.get("history") or [])
    entry = {
        "date": snap["updated"],
        "stock_index_pct": snap["stock_index_pct"],
        "pe": snap["jni"]["pe"],
        "body": snap["jni"]["body"],
        "thin": snap["jni"]["thin"],
        "boot_pct": snap["boot"]["pct"],
        "engine_done": snap["engine"]["done"],
        "smoke": snap["boot"]["smoke"],
        "build": snap["boot"]["build"],
    }
    if hist and hist[-1].get("date") == entry["date"]:
        hist[-1] = entry
        return hist
    hist.append(entry)
    return hist[-24:]


SNAPSHOT_BEGIN = "// <snapshot>"
SNAPSHOT_END = "// </snapshot>"


def write_canvas_snapshot(snap: dict) -> None:
    if not CANVAS.exists():
        return
    text = CANVAS.read_text(encoding="utf-8")
    blob = (
        SNAPSHOT_BEGIN
        + "\nconst SNAPSHOT = "
        + json.dumps(snap, indent=2, ensure_ascii=False)
        + " as const;\n"
        + SNAPSHOT_END
    )
    pattern = re.compile(
        re.escape(SNAPSHOT_BEGIN) + r".*?" + re.escape(SNAPSHOT_END),
        re.S,
    )
    if not pattern.search(text):
        raise SystemExit(f"canvas missing snapshot markers: {CANVAS}")
    CANVAS.write_text(pattern.sub(blob, text), encoding="utf-8")


def main() -> None:
    prev = load_prev()
    rows = parse_table()
    fns = index_runtime_fns()
    classified = classify(rows, fns)
    engine = merge_engine(prev)
    smoke = latest_smoke()
    snap = build_snapshot(classified, engine, smoke)
    snap["history"] = append_history(prev, snap)
    OUT_JSON.write_text(json.dumps(snap, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    write_canvas_snapshot(snap)
    j = snap["jni"]
    print(
        f"stock={snap['stock_index_pct']}%  "
        f"pe={j['pe']}/{j['table']} ({j['pe_pct']}%)  "
        f"nonthin={j['nonthin']} ({j['nonthin_pct']}%)  "
        f"thin={j['thin']}  "
        f"boot={snap['boot']['pct']}%  "
        f"engine={snap['engine']['done']}/{snap['engine']['total']}  "
        f"smoke={snap['boot']['smoke']}"
    )


if __name__ == "__main__":
    main()
