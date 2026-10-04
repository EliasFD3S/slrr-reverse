#!/usr/bin/env python3
"""Measure reverse-host progress toward stock StreetLegal_Redline.exe.

Writes native/docs/progress_global.json and refreshes the Cursor canvas
SNAPSHOT block (slrr-stock-progress.canvas.tsx).

Journey index (headline) — fidelity to game start, not JNI fill:
  0.10 * boot_smoke
  0.30 * gameloop     (MainLoop stages: hosted=1, partial=0.5)
  0.25 * engine       (non-JNI PE clusters, incl. mainloop_1to1)
  0.15 * physics      (done=1, partial=0.5)
  0.10 * render       (done=1, partial=0.5)
  0.10 * jni_pe       (RegisterNative PE comments — hygiene, not the goal)

OOS blockers stay tracked separately (gate closes; not in index).
JNI table fill alone must not read as “almost done”.

This is NOT runtime/*/PROGRESS.md name-match coverage.
"""
from __future__ import annotations

import json
import re
from collections import defaultdict
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / "native" / "engine"
STUBS = ENGINE / "core" / "natives_stubs.cpp"
RUNTIME = ENGINE / "runtime"
OUT_JSON = ROOT / "native" / "docs" / "progress_global.json"
CANVAS = (
    Path.home()
    / ".cursor"
    / "projects"
    / "c-Users-Elias-Desktop-re-engineering"
    / "canvases"
    / "slrr-stock-progress.canvas.tsx"
)
FORMULA_ID = "journey_v2"

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
# group: graph | render | audio | script | boot | nav
DEFAULT_ENGINE = [
    {
        "id": "setParent_lists",
        "title": "GameRef.setParent child lists +0x30/+0x38",
        "va": "0x0047E2D0",
        "group": "graph",
        "done": True,
        "blocker": None,
        "note": "type1 HostAttachBlock+setParent_inner; dllist 2/3; Type53 lists",
    },
    {
        "id": "navigator_update",
        "title": "Navigator.updateNavigator body 0x6c4",
        "va": "0x00482D30",
        "group": "nav",
        "done": True,
        "blocker": None,
        "note": "PE@ body+findCam+osd Bind+camApply soft; look/Text RText2 OOS residual",
    },
    {
        "id": "queueEvent_parse",
        "title": "queueEvent parser sub_458C00",
        "va": "0x0047DA30",
        "group": "script",
        "done": True,
        "blocker": None,
        "note": "PE@ lists+LGI+ctor/THRD+mid+0x10+PrepareLod stand-in; vtbl* residual",
    },
    {
        "id": "fog_vtable",
        "title": "Camera.setFog → ResHandle_getPayload dual hop",
        "va": "0x00486570",
        "group": "render",
        "done": True,
        "blocker": None,
        "note": "Native.ptr + getPayload 0xA0000000 → fog+0x1C..+0x28",
    },
    {
        "id": "sfx_3d_cull",
        "title": "SfxRef.nplay 3D cull",
        "va": "0x00480D40",
        "group": "audio",
        "done": True,
        "blocker": None,
        "note": "null/id0 → -1; listener via MainLoop",
    },
    {
        "id": "force_rendering_7",
        "title": "GfxEngine.forceRendering 7 pumps",
        "va": "0x0047C1D0",
        "group": "render",
        "done": True,
        "blocker": None,
        "note": "7× Pump+AltPresentGate+GCSweep(1)+DrainGpuCaches; AltPresent Draw OOS",
    },
    {
        "id": "render_4arg",
        "title": "RenderRef.setMatrix 4-arg parent-link",
        "va": "0x004810B0",
        "group": "render",
        "done": True,
        "blocker": None,
        "note": "LinkOrUnlink mid0x17C+findBone+Rebind+hook VA-backed",
    },
    {
        "id": "isempty_type8",
        "title": "GameRef.isEmpty RESTYPE_GAME=8",
        "va": "0x00486D10",
        "group": "script",
        "done": True,
        "blocker": None,
        "note": "host = empty flag",
    },
    {
        "id": "getscript_split",
        "title": "getScriptInstance type 1 / 8",
        "va": "0x00486F30",
        "group": "script",
        "done": True,
        "blocker": None,
        "note": "type1→script +0x50; type8→Class_boxObject java.lang.Class shell",
    },
    {
        "id": "mainloop_1to1",
        "title": "Engine_MainLoop tick/render 1:1",
        "va": "0x00428960",
        "group": "boot",
        "done": False,
        "blocker": "VMThread_run opcode loop @ 0x4210D4 size 0x2979",
        "note": "frame pumps+FilePool+PackFile API; bytecode@4210D4 residual",
    },
]

# Engine_MainLoop @ 0x00428960 — ordered frame stages (not weighted in index).
# status: hosted | partial | oos
DEFAULT_GAMELOOP = [
    {
        "id": "input_tick",
        "title": "Input_tick device list",
        "va": "0x0054DDA0",
        "order": 1,
        "status": "partial",
        "note": "Input_tick+deviceList Push/tickAxes soft; Player embed/DI table/MainLoop OOS",
    },
    {
        "id": "simulate_frame",
        "title": "SimulateFrame TickSim+Phys+Timers",
        "va": "0x00428450",
        "order": 2,
        "status": "partial",
        "note": "Physics_Step+CONTROL dllist+CallNamedMethod; DrainEvent empty",
    },
    {
        "id": "jvm_pump_frame",
        "title": "Jvm_PumpFrame + GcSlice",
        "va": "0x00418D10",
        "order": 3,
        "status": "partial",
        "note": "++Jvm+RunThreads+EMA+0x24 soft; GcSlice/mark-sweep→bytecode OOS",
    },
    {
        "id": "sfx_listener",
        "title": "Sfx_ListenerSetPose + UpdateVoices",
        "va": "0x005508F0",
        "order": 4,
        "status": "hosted",
        "note": "camera eye → listener; UpdateVoices @ 0x550980",
    },
    {
        "id": "pump_load",
        "title": "ResourceEngine_PumpLoadQueue",
        "va": "0x005378D0",
        "order": 5,
        "status": "partial",
        "note": "LoadLod+TouchResNode+tryUnload soft; GT Update/dtor OOS",
    },
    {
        "id": "pump_unload",
        "title": "ResourceEngine_PumpUnloadQueue",
        "va": "0x00537B40",
        "order": 6,
        "status": "partial",
        "note": "wantUnload+Touch+tryUnload+GCSweep; GT Update/dtor OOS",
    },
    {
        "id": "present",
        "title": "GfxEngine Present / flush",
        "va": "0x00428CFE",
        "order": 7,
        "status": "hosted",
        "note": "render_d3d9_flush; AltPresentGate OOS",
    },
    {
        "id": "asyncload",
        "title": "AsyncLoad HasWork→PumpOne→FinishSlice",
        "va": "0x00505CD0",
        "order": 8,
        "status": "partial",
        "note": "PumpOne+UV slots+PostLoop soft; PostLoop D3D VB/IB/JPEG OOS",
    },
    {
        "id": "sleep_ld",
        "title": "Engine_SleepMs when ldWorkScale≤0",
        "va": "0x00551730",
        "order": 9,
        "status": "hosted",
        "note": "Win32 Sleep(10) on LD_HIGH; NORM 0.1 skips",
    },
    {
        "id": "poll_quit",
        "title": "Engine_PollWindowQuit",
        "va": "0x005522A0",
        "order": 10,
        "status": "hosted",
        "note": "render_d3d9_pump + quit flag",
    },
    {
        "id": "endframe_fileasync",
        "title": "MainLoop_EndFrame FileAsync ring+Worker",
        "va": "0x00554DC0",
        "order": 11,
        "status": "partial",
        "note": "HandleCache LRU soft+FilePool -2; FlushHandles/DrainEvent dllists OOS",
    },
]

# Host physics / vehicle dynamics (mostly non-JNI; Body/Parts JNI deepen separate).
# status: done | partial | blocked
DEFAULT_PHYSICS = [
    {
        "id": "arcade_body",
        "title": "PhysicsRef createBox/sphere + vel/angVel",
        "status": "partial",
        "area": "arcade",
        "blocker": None,
        "note": "ResState body; integrate + ground_y; not stock phys solver",
    },
    {
        "id": "arcade_drive",
        "title": "Controller axes → accel/brake/handbrake/nitro",
        "status": "partial",
        "area": "arcade",
        "blocker": None,
        "note": "valocity_simulate drive; gear/asleep/collide gates",
    },
    {
        "id": "road_network",
        "title": "GroundRef road project / nearest cross / spawn",
        "status": "partial",
        "area": "world",
        "blocker": None,
        "note": "tickPhys+pathnode+timer heap soft; list ticks/queueEvent OOS",
    },
    {
        "id": "chassis_forceUpdate",
        "title": "Chassis.forceUpdate / ingestPart / cloneHdr",
        "status": "partial",
        "area": "stock_phys",
        "note": "phys_blob+camBlob_baseCtor soft; allocCamBlob@44A250 body OOS",
        "blocker": "Chassis_allocCamBlob@44A250 body 0x16c2",
    },
    {
        "id": "wheel_phys_table",
        "title": "Wheel phys table stride 0x2B4",
        "status": "partial",
        "area": "stock_phys",
        "note": "setBrake/HBrake +0xD8/+0xDC; phys78 side-map; no *[veh+0x13E4]",
        "blocker": "*[veh+0x13E4] phys slots",
    },
    {
        "id": "aabb_mesh",
        "title": "Chassis.getMin/getMax mesh AABB",
        "status": "partial",
        "area": "stock_phys",
        "note": "FINAL-node mesh walk; pad 0.1; no raw mat@+0x1C",
        "blocker": "Veh_ensureSceneBound / mesh blob +0x5C",
    },
    {
        "id": "dyno_calc",
        "title": "DynoData.calcDyno torque curve",
        "status": "partial",
        "area": "powertrain",
        "blocker": None,
        "note": "turbo gate / T_loss / mixture; phys Native.ptr walks open",
    },
    {
        "id": "native_ptr_graph",
        "title": "Native.ptr + ResHandle_getPayload host",
        "status": "partial",
        "area": "bridge",
        "note": "Camera fog dual hop done; setParent type1 getPayload; vtbl+0x20 OOS",
        "blocker": "ResourceEngine vtbl attach",
    },
]

# Host render engine (D3D9 + scene graph). Not in stock index — tracks frame path
# fidelity beyond the few PE engine clusters (fog / forceRendering / setMatrix4).
# area: device | camera | scene | mesh | texture | osd | frame
# status: done | partial | blocked
DEFAULT_RENDER = [
    {
        "id": "d3d9_device",
        "title": "D3D9 device Clear/Present + window",
        "status": "partial",
        "area": "device",
        "va": "0x0047C1D0",
        "blocker": None,
        "note": "render_d3d9; --window 800×600; headless default",
    },
    {
        "id": "viewport_activate",
        "title": "Viewport activate + SetViewport",
        "status": "partial",
        "area": "device",
        "va": None,
        "blocker": None,
        "note": "normalized rect + CLEARDEPTH/TARGET queue on flush",
    },
    {
        "id": "camera_frustum",
        "title": "Camera create / activate / projection",
        "status": "partial",
        "area": "camera",
        "va": "0x004861E0",
        "blocker": None,
        "note": "half-AOV + aspect; RenderRef camera under parent",
    },
    {
        "id": "fog_dual_hop",
        "title": "Camera.setFog ResHandle_getPayload dual hop",
        "status": "done",
        "area": "camera",
        "va": "0x00486570",
        "blocker": None,
        "note": "Native.ptr + tag 0xA0000000 → fog+0x1C..+0x28",
    },
    {
        "id": "texture_dds",
        "title": "makeTexture / DDS + RPAK sourcefile upload",
        "status": "partial",
        "area": "texture",
        "va": "0x0047FFE0",
        "blocker": None,
        "note": "DXT1/3/5 + A8R8G8B8 MANAGED; envmap key store",
    },
    {
        "id": "scx_invo_mesh",
        "title": "SCX INVO mesh parse + flush draw",
        "status": "partial",
        "area": "mesh",
        "va": None,
        "blocker": None,
        "note": "v4 chunks mat/meta/verts/idx; font INVO v3 TODO",
    },
    {
        "id": "mesh_world_pose",
        "title": "RenderRef.setMatrix pose + parent hierarchy",
        "status": "partial",
        "area": "scene",
        "va": "0x004810B0",
        "blocker": None,
        "note": "Scale*Ry*Rx*Rz*T; World=Local*ParentWorld",
    },
    {
        "id": "bone_parent_link",
        "title": "setMatrix 4-arg bone / LinkOrUnlinkBone",
        "status": "done",
        "area": "scene",
        "va": "0x004810B0",
        "blocker": None,
        "note": "mid 0x17C own-list + findBone HEAD + Rebind + hook 0x1C",
    },
    {
        "id": "osd_blit_fonts",
        "title": "OSD Rectangle/Text XYZRHW + font atlas",
        "status": "partial",
        "area": "osd",
        "va": None,
        "blocker": None,
        "note": "pri-sorted strips; RID→simple20/slii24; greyscale TGA",
    },
    {
        "id": "light_flare",
        "title": "RenderRef setLight / setFlare / project",
        "status": "partial",
        "area": "scene",
        "va": None,
        "blocker": None,
        "note": "directional+ambient; world→OSD NDC project",
    },
    {
        "id": "force_rendering_7",
        "title": "GfxEngine.forceRendering 7 Present pumps",
        "status": "done",
        "area": "frame",
        "va": "0x0047C1D0",
        "blocker": None,
        "note": "7× Pump+AltPresentGate+GCSweep(1)+DrainGpuCaches; AltPresent Draw OOS",
    },
    {
        "id": "fmv_open_video",
        "title": "GfxEngine.openVideo / close / isPlaying",
        "status": "partial",
        "area": "frame",
        "va": "0x0047C330",
        "blocker": None,
        "note": "FMV open+nonExclusive+UpdateQuad soft; AltPresent VB/boot@55C470 OOS",
    },
    {
        "id": "endframe_present",
        "title": "MainLoop EndFrame Simulate + Present",
        "status": "done",
        "area": "frame",
        "va": "0x00554DC0",
        "blocker": None,
        "note": "Physics_Step + Present + FileAsync + TickTimers/empty lists",
    },
]

# Hard OOS — blocks engine/physics closes; not closed by go-all fillers.
# status: open | partial | unlocked
DEFAULT_OOS = [
    {
        "id": "vtbl_attach_type1",
        "title": "setParent type1 vtbl+0x20 + type53",
        "status": "unlocked",
        "blocks": ["setParent_lists"],
        "va": "0x00419860",
        "note": "Type53 A/C+spatial+linkNearby; promote@4B3AC0 OOS",
    },
    {
        "id": "link_or_unlink_bone",
        "title": "LinkOrUnlinkBone / getPayload mismatch",
        "status": "unlocked",
        "blocks": ["render_4arg", "bone_parent_link"],
        "va": "0x0048BE10",
        "note": "PumpLoad68+GCSweep evict/Lo+mid_Unload; tryUnload/GT Update OOS",
    },
    {
        "id": "queueevent_0x38fe",
        "title": "queueEvent full parser sub_458C00",
        "status": "unlocked",
        "blocks": ["queueEvent_parse"],
        "va": "0x00458C00",
        "note": "LGI+PrepareLod stand-in+ctor/THRD; vtbl*+timer residual",
    },
    {
        "id": "mainloop_endframe",
        "title": "MainLoop EndFrame FileAsync + Simulate/Present",
        "status": "unlocked",
        "blocks": ["mainloop_1to1", "endframe_present"],
        "va": "0x00554DC0",
        "note": "PackFile API+FilePool Worker; SetRoot 0 xref / HandleCache OOS",
    },
    {
        "id": "chassis_1fbc_blob",
        "title": "Chassis +0x1FBC cloneHdr / phys graph",
        "status": "unlocked",
        "blocks": ["chassis_forceUpdate", "wheel_phys_table"],
        "va": "0x00448430",
        "note": "spatial insert+linkNearby malloc20; promote@4B3AC0 OOS",
    },
    {
        "id": "nav_scene_poke",
        "title": "Navigator scene poke *(handle+0xC)",
        "status": "unlocked",
        "blocks": ["navigator_update"],
        "va": "0x00482D30",
        "note": "poke+paint+GII_DIR getInfo; paint tiles + Bind0x12 soft",
    },
]

# journey_v2 — demote JNI; weight frame path + dynamics that gate world entry.
W_BOOT = 0.10
W_GAMELOOP = 0.30
W_ENGINE = 0.25
W_PHYSICS = 0.15
W_RENDER = 0.10
W_JNI = 0.10


def _merge_catalog(defaults: list[dict], prev_list: list, key: str = "id") -> list[dict]:
    """Refresh titles/notes/blockers/status from defaults; keep done from prev
    unless default marks done=True (catalog unlock)."""
    prev_map = {e[key]: e for e in (prev_list or []) if isinstance(e, dict) and e.get(key)}
    out = []
    for default in defaults:
        old = prev_map.get(default[key], {})
        item = dict(default)
        if "done" in old:
            item["done"] = bool(old["done"])
        if default.get("done"):
            item["done"] = True
        out.append(item)
    return out


def merge_engine(prev: dict) -> list[dict]:
    prev_list = prev.get("engine_clusters") or (prev.get("engine") or {}).get("clusters") or []
    return _merge_catalog(DEFAULT_ENGINE, prev_list)


def merge_physics(prev: dict) -> list[dict]:
    prev_list = (prev.get("physics") or {}).get("items") or prev.get("physics_items") or []
    return _merge_catalog(DEFAULT_PHYSICS, prev_list)


def merge_render(prev: dict) -> list[dict]:
    prev_list = (prev.get("render") or {}).get("items") or prev.get("render_items") or []
    return _merge_catalog(DEFAULT_RENDER, prev_list)


def merge_oos(prev: dict) -> list[dict]:
    prev_list = (prev.get("oos") or {}).get("items") or prev.get("oos_items") or []
    return _merge_catalog(DEFAULT_OOS, prev_list)


def merge_gameloop(prev: dict) -> list[dict]:
    # Prefer DEFAULT titles/notes/status; keep custom order from defaults.
    prev_list = (prev.get("gameloop") or {}).get("stages") or []
    return _merge_catalog(DEFAULT_GAMELOOP, prev_list)


def _gameloop_stats(stages: list[dict]) -> dict:
    hosted = sum(1 for s in stages if s.get("status") == "hosted")
    partial = sum(1 for s in stages if s.get("status") == "partial")
    oos = sum(1 for s in stages if s.get("status") == "oos")
    n = len(stages)
    # hosted=1, partial=0.5, oos=0
    readiness = pct(hosted * 2 + partial, n * 2) if n else 0.0
    return {
        "hosted": hosted,
        "partial": partial,
        "oos": oos,
        "total": n,
        "pct": readiness,
        "va": "0x00428960",
        "title": "Engine_MainLoop frame path",
        "blocker": "VMThread_run opcode loop @ 0x4210D4 size 0x2979",
        "stages": sorted(stages, key=lambda s: int(s.get("order") or 0)),
    }


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
    # Forward decls (`void foo(...);`) must not steal the next function body.
    if ";" in src[start:brace]:
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


# Soft-only leftovers: declared in System.java + host stubs, ABSENT from
# stock Natives_RegisterAll / IDA (0 string hits). Exclude from pe/table.
SOFT_ONLY_JNI = frozenset(
    {
        "java_lang_System_netHost",
        "java_lang_System_netJoin",
        "java_lang_System_netLeave",
    }
)


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
            if not body:
                continue  # forward declaration — keep prior real body
            info = {
                "file": str(cpp.relative_to(ENGINE)).replace("\\", "/"),
                "nloc": body.count("\n"),
                "pe": bool(PE_RE.search(body)),
                "thin": is_thin(body),
            }
            prev = out.get(name)
            # Prefer PE-tagged / larger body when duplicates exist.
            if prev and (prev["pe"] and not info["pe"]):
                continue
            if prev and prev["nloc"] > info["nloc"] and prev["pe"] == info["pe"]:
                continue
            out[name] = info
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


def pct(num: int, den: int) -> float:
    if den <= 0:
        return 0.0
    return round(100.0 * num / den, 1)


def classify(rows: list[dict], fns: dict[str, dict]) -> list[dict]:
    classified = []
    for r in rows:
        if r["fn"] in SOFT_ONLY_JNI:
            continue  # not stock PE RegisterNative — out of pe/table
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


def _readiness_stats(items: list[dict], area_order: tuple[str, ...]) -> dict:
    counts = {"done": 0, "partial": 0, "blocked": 0}
    by_area: dict[str, dict[str, int]] = defaultdict(
        lambda: {"done": 0, "partial": 0, "blocked": 0, "total": 0}
    )
    for p in items:
        st = str(p.get("status") or "partial")
        if st not in counts:
            st = "partial"
        counts[st] += 1
        area = str(p.get("area") or "other")
        by_area[area][st] += 1
        by_area[area]["total"] += 1
    n = len(items)
    score = counts["done"] * 1.0 + counts["partial"] * 0.5
    return {
        "done": counts["done"],
        "partial": counts["partial"],
        "blocked": counts["blocked"],
        "total": n,
        "pct": pct(int(score * 2), n * 2) if n else 0.0,
        "by_area": [
            {"area": a, **by_area[a]} for a in area_order if a in by_area
        ],
        "items": items,
    }


def _physics_stats(physics: list[dict]) -> dict:
    return _readiness_stats(
        physics, ("arcade", "world", "stock_phys", "powertrain", "bridge", "other")
    )


def _render_stats(render: list[dict]) -> dict:
    return _readiness_stats(
        render, ("device", "camera", "texture", "mesh", "scene", "osd", "frame", "other")
    )


def _oos_stats(oos: list[dict]) -> dict:
    counts = {"unlocked": 0, "partial": 0, "open": 0}
    for o in oos:
        st = str(o.get("status") or "open")
        if st not in counts:
            st = "open"
        counts[st] += 1
    n = len(oos)
    closed = counts["unlocked"]  # fully unlocked only
    return {
        "unlocked": counts["unlocked"],
        "partial": counts["partial"],
        "open": counts["open"],
        "total": n,
        "pct": pct(closed, n),
        "items": oos,
    }


def _engine_by_group(engine: list[dict]) -> list[dict]:
    groups: dict[str, dict[str, int]] = defaultdict(lambda: {"done": 0, "open": 0, "total": 0})
    for e in engine:
        g = str(e.get("group") or "other")
        groups[g]["total"] += 1
        if e.get("done"):
            groups[g]["done"] += 1
        else:
            groups[g]["open"] += 1
    order = ("graph", "nav", "script", "render", "audio", "boot", "other")
    return [{"group": g, **groups[g]} for g in order if g in groups]


def build_snapshot(
    classified: list[dict],
    engine: list[dict],
    physics: list[dict],
    render: list[dict],
    oos: list[dict],
    smoke: dict,
    gameloop: list[dict] | None = None,
) -> dict:
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

    phys = _physics_stats(physics)
    rend = _render_stats(render)
    oos_s = _oos_stats(oos)
    gl = _gameloop_stats(gameloop or DEFAULT_GAMELOOP)

    layers = [
        {"id": "boot", "label": "Boot smoke", "weight": W_BOOT, "pct": boot},
        {
            "id": "gameloop",
            "label": "MainLoop frame",
            "weight": W_GAMELOOP,
            "pct": float(gl["pct"]),
        },
        {
            "id": "engine",
            "label": "PE clusters",
            "weight": W_ENGINE,
            "pct": eng_pct,
        },
        {
            "id": "physics",
            "label": "Physics",
            "weight": W_PHYSICS,
            "pct": float(phys["pct"]),
        },
        {
            "id": "render",
            "label": "Render",
            "weight": W_RENDER,
            "pct": float(rend["pct"]),
        },
        {"id": "jni", "label": "JNI PE table", "weight": W_JNI, "pct": pe_pct},
    ]
    for layer in layers:
        layer["pts"] = round(layer["weight"] * layer["pct"], 2)
    stock = round(sum(layer["pts"] for layer in layers), 1)

    open_blockers = []
    for e in engine:
        if not e.get("done"):
            open_blockers.append(
                {
                    "layer": "engine",
                    "id": e["id"],
                    "title": e.get("title"),
                    "va": e.get("va"),
                    "blocker": e.get("blocker"),
                }
            )
    for s in gl.get("stages") or []:
        if s.get("status") != "hosted":
            open_blockers.append(
                {
                    "layer": "gameloop",
                    "id": s["id"],
                    "title": s.get("title"),
                    "va": s.get("va"),
                    "blocker": s.get("note") if s.get("status") == "oos" else None,
                    "status": s.get("status"),
                }
            )
    for item in phys.get("items") or []:
        if item.get("status") == "blocked" or item.get("blocker"):
            if item.get("status") != "done":
                open_blockers.append(
                    {
                        "layer": "physics",
                        "id": item["id"],
                        "title": item.get("title"),
                        "va": item.get("va"),
                        "blocker": item.get("blocker"),
                        "status": item.get("status"),
                    }
                )
    for item in oos_s.get("items") or []:
        if item.get("status") == "open":
            open_blockers.append(
                {
                    "layer": "oos",
                    "id": item["id"],
                    "title": item.get("title"),
                    "va": item.get("va"),
                    "blocker": item.get("note"),
                    "status": item.get("status"),
                }
            )

    return {
        "updated": date.today().isoformat(),
        "formula_id": FORMULA_ID,
        "formula": (
            f"{W_BOOT}*boot + {W_GAMELOOP}*gameloop + {W_ENGINE}*engine + "
            f"{W_PHYSICS}*physics + {W_RENDER}*render + {W_JNI}*jni_pe"
        ),
        "stock_index_pct": stock,
        "layers": layers,
        "open_blockers": open_blockers,
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
            "open": eng_n - eng_done,
            "by_group": _engine_by_group(engine),
            "clusters": engine,
        },
        "gameloop": gl,
        "physics": phys,
        "render": rend,
        "oos": oos_s,
        "domains": domain_rows,
        "weights": {
            "boot": W_BOOT,
            "gameloop": W_GAMELOOP,
            "engine": W_ENGINE,
            "physics": W_PHYSICS,
            "render": W_RENDER,
            "jni": W_JNI,
        },
        "caveat": (
            "journey_v2: headline tracks splash→world fidelity. MainLoop + "
            "PE clusters + physics/render readiness dominate; JNI PE table "
            "is only 10% (hygiene once filled). Soft smoke EXIT=0 is not PE "
            "MainLoop 1:1. OOS items gate closes but are not in the %. "
            "runtime/*/PROGRESS.md is name-match only."
        ),
    }


def append_history(prev: dict, snap: dict) -> list[dict]:
    hist = list(prev.get("history") or [])
    entry = {
        "date": snap["updated"],
        "formula_id": snap.get("formula_id"),
        "stock_index_pct": snap["stock_index_pct"],
        "pe": snap["jni"]["pe"],
        "body": snap["jni"]["body"],
        "thin": snap["jni"]["thin"],
        "boot_pct": snap["boot"]["pct"],
        "engine_done": snap["engine"]["done"],
        "gameloop_pct": snap["gameloop"]["pct"],
        "physics_pct": snap["physics"]["pct"],
        "render_pct": snap["render"]["pct"],
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
    physics = merge_physics(prev)
    render = merge_render(prev)
    oos = merge_oos(prev)
    gameloop = merge_gameloop(prev)
    smoke = latest_smoke()
    snap = build_snapshot(classified, engine, physics, render, oos, smoke, gameloop)
    snap["history"] = append_history(prev, snap)
    OUT_JSON.write_text(json.dumps(snap, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    write_canvas_snapshot(snap)
    j = snap["jni"]
    p = snap["physics"]
    r = snap["render"]
    o = snap["oos"]
    print(
        f"journey={snap['stock_index_pct']}% ({snap.get('formula_id')})  "
        f"boot={snap['boot']['pct']}%  "
        f"gameloop={snap['gameloop']['pct']}%  "
        f"engine={snap['engine']['done']}/{snap['engine']['total']}  "
        f"phys={p['pct']}%  render={r['pct']}%  "
        f"jni={j['pe']}/{j['table']}  "
        f"oos_open={o['open']}  "
        f"smoke={snap['boot']['smoke']}"
    )


if __name__ == "__main__":
    main()
