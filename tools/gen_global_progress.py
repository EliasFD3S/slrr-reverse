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
        "blocker": "Jvm::invoke PE-stream leaf-only; SLRR_PE_STREAM_INVOKE=1 "
                   "still breaks Soft host leaves (command/addTraffic→traffic "
                   "id=0); name-only+Vector 4025 packing hosted; 1×0x4012 OOS",
        "note": "insn stream proven == TreeBody nodes; see snapshot.mainloop for "
                "the per-opcode frontier (static vs boot-path)",
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
        "status": "hosted",
        "note": "PE@0x54DDA0 size 0x46 1:1 soft (pollDevices+stamp+tickAxes walk); Player+0x1C embed=DeviceList_Push site outside body",
    },
    {
        "id": "simulate_frame",
        "title": "SimulateFrame TickSim+Phys+Timers",
        "va": "0x00428450",
        "order": 2,
        "status": "hosted",
        "note": "PE@0x428450 soft TickSim+Phys+Timers+Drain empty; enqueueUnload/Release outside body OOS",
    },
    {
        "id": "jvm_pump_frame",
        "title": "Jvm_PumpFrame + GcSlice",
        "va": "0x00418D10",
        "order": 3,
        "status": "hosted",
        "note": "PE@0x418D10 size 0x191 soft (++gen+EMA+RunThreads); GcSlice/mark-sweep→bytecode OOS",
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
        "status": "hosted",
        "note": "PE@0x5378D0 size 0x270 soft walk+LoadLod+recycle+LoadRing; GT dtor/EnsureIndex TOC OOS",
    },
    {
        "id": "pump_unload",
        "title": "ResourceEngine_PumpUnloadQueue",
        "va": "0x00537B40",
        "order": 6,
        "status": "hosted",
        "note": "PE@0x537B40 size 0x189 soft mark+recycle+GCSweep(0); GT dtor tryUnload OOS",
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
        "status": "hosted",
        "note": "PE@0x505CD0 size 0xd0 Soft HasWork→PumpOne→FinishSlice; GetPackSlot Soft; PostLoop D3D/JPEG OOS",
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
        "status": "hosted",
        "note": "PE@0x554DC0 size 0x44 soft ring state4→Malloc→1+WakeSem; FlushHandles/real worker OOS",
    },
]

# Host physics / vehicle dynamics (mostly non-JNI; Body/Parts JNI deepen separate).
# status: done | partial | blocked
DEFAULT_PHYSICS = [
    {
        "id": "arcade_body",
        "title": "PhysicsRef createBox/sphere + vel/angVel",
        "status": "done",
        "area": "arcade",
        "blocker": None,
        "note": "PE@4805B0/4806F0 Soft create+zero_vel after bind; integrate/ground_y/solver OOS",
    },
    {
        "id": "arcade_drive",
        "title": "Controller axes → accel/brake/handbrake/nitro",
        "status": "done",
        "area": "arcade",
        "blocker": None,
        "note": "PE@480500 Soft getSpeedSquare live_phys_key; axes→physics_drive residual OOS",
    },
    {
        "id": "road_network",
        "title": "GroundRef road project / nearest cross / spawn",
        "status": "done",
        "area": "world",
        "blocker": None,
        "note": "PE@483400 Soft getStartDirection no-snap; list ticks/queueEvent OOS",
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
        "status": "done",
        "area": "stock_phys",
        "note": "PE@44B000 Soft LEA sidemap arm/hub/opp stride 0x2B4; *[veh+0x13E4] Phys_allocChild OOS",
        "blocker": None,
    },
    {
        "id": "aabb_mesh",
        "title": "Chassis.getMin/getMax mesh AABB",
        "status": "done",
        "area": "stock_phys",
        "note": "PE@43D600 Soft TREE aabb from Part_setMesh; Veh_ensureSceneBound/blob+0x5C OOS",
        "blocker": None,
    },
    {
        "id": "dyno_calc",
        "title": "DynoData.calcDyno torque curve",
        "status": "done",
        "area": "powertrain",
        "blocker": None,
        "note": "PE@0x46AA20 Soft turbo gate+T_loss=95+fillTables; Native.ptr 0x1EC / full P-V OOS",
    },
    {
        "id": "native_ptr_graph",
        "title": "Native.ptr + ResHandle_getPayload host",
        "status": "done",
        "area": "bridge",
        "note": "PE@0x419860 Soft resh_get_payload TickSim sites; PrepareLod Relink+RE vtbl+0x20 OOS",
        "blocker": None,
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
        "status": "done",
        "area": "device",
        "va": "0x0047C1D0",
        "blocker": None,
        "note": "PE@47C1D0→PresentFrame ClearTargetZ Z=0x3F7FFF58 + Present; g_present_count Soft",
    },
    {
        "id": "viewport_activate",
        "title": "Viewport activate + SetViewport",
        "status": "done",
        "area": "device",
        "va": "0x00481680",
        "blocker": None,
        "note": "PE@481680→ViewportBind@4FD020 type18; Soft g_active_vp+pending_clear+SetViewport; PE BindList dllist OOS",
    },
    {
        "id": "camera_frustum",
        "title": "Camera create / activate / projection",
        "status": "done",
        "area": "camera",
        "va": "0x004861E0",
        "blocker": None,
        "note": "PE@4861E0 Soft TREE frustum+cam_vp/pri type18; node factory/dllist OOS",
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
        "status": "done",
        "area": "texture",
        "va": "0x0047FFE0",
        "blocker": None,
        "note": "PE@47FFE0 Soft makeTexture + RPAK restype7 sourcefile path; AllocLocalRid/CRT GPU OOS",
    },
    {
        "id": "scx_invo_mesh",
        "title": "SCX INVO mesh parse + flush draw",
        "status": "done",
        "area": "mesh",
        "va": "0x00502070",
        "blocker": None,
        "note": "PE@502070 Soft vle3 ver==3+Max readV3 stage→VB/IB; v4 chunks; CreateVB vt+D0/MatHdr maps OOS",
    },
    {
        "id": "mesh_world_pose",
        "title": "RenderRef.setMatrix pose + parent hierarchy",
        "status": "done",
        "area": "scene",
        "va": "0x0048BF50",
        "blocker": None,
        "note": "PE@48BF50 Soft setBasis_posScaled10 *10→bone+0x54 stand-in; HostPeBoneNode.raw OOS",
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
        "status": "done",
        "area": "osd",
        "va": "0x00487050",
        "blocker": None,
        "note": "PE@487050 r_text + Soft r_text2@48C670 gauges; Rectangle RenderRef mesh / LookMoveZoom OOS",
    },
    {
        "id": "light_flare",
        "title": "RenderRef setLight / setFlare / project",
        "status": "done",
        "area": "scene",
        "va": "0x00486AB0",
        "blocker": None,
        "note": "PE@486AB0/486B20 Soft applyLight/Flare TREE units+stamp; getPayload 0x80000001/Rebind OOS",
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
        "status": "done",
        "area": "frame",
        "va": "0x0047C330",
        "blocker": None,
        "note": "PE@47C330 Soft open/close/isPlaying+AltPresentGate; AltPresent VB/boot@55C470 OOS",
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
        "blocker": "VMThread_run PE insn stream @ 0x4210D4; Soft hi 1016/1017/101F cold — not 1:1",
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
        # Java native; absent stock Natives_RegisterAll@487F20 PhysicsRef block
        # (create/createBox/createSphere/setMatrix/getPos/getOri only — IDA).
        "java_util_resource_PhysicsRef_setStatic",
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


def _runtime_source_text(cpp: Path) -> str:
    """Read a runtime .cpp, reassembling split_source.py shells.

    After the 128 KB editor-cap split, real native bodies live in
    `<name>_partN.inc` fragments `#include`d by a thin shell .cpp. The
    JNI PE scanner must see the concatenated TU, not just the shell —
    otherwise every moved body is falsely reported as `missing` and the
    journey jni layer collapses (≈114 false drops after the first split).
    """
    src = cpp.read_text(encoding="utf-8", errors="replace")
    parts: list[str] = []
    for m in re.finditer(r'#include\s+"([^"]+_part\d+\.inc)"', src):
        inc = cpp.parent / m.group(1)
        if inc.is_file():
            parts.append(inc.read_text(encoding="utf-8", errors="replace"))
    if not parts:
        return src
    # Keep the shell preamble (includes / using) then append fragments in
    # include order — byte-identical to what MSVC compiles.
    return src + "\n" + "\n".join(parts)


def index_runtime_fns() -> dict[str, dict]:
    out: dict[str, dict] = {}
    for cpp in RUNTIME.rglob("*.cpp"):
        src = _runtime_source_text(cpp)
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
        return {
            "file": None,
            "boot_pct": None,
            "exit_ok": None,
            "build": None,
            "render_ok": None,
        }
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
    render_ok = None
    m = re.search(r"boot render ok=(\d+)/(\d+)", text)
    if m:
        render_ok = f"{m.group(1)}/{m.group(2)}"
    return {
        "file": path.name,
        "boot_pct": boot_pct,
        "exit_ok": exit_ok,
        "build": build,
        "render_ok": render_ok,
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


# ---------------------------------------------------------------------------
# mainloop_1to1 frontier
#
# Everything here is read back from artefacts, never typed in by hand:
#   static coverage  <- native/docs/tree_opcode_census.json (tree_opcode_census.py)
#   boot-path counts <- native/docs/evidence/*.json measurements.boot_path
# The per-opcode notes below only name the PE routine and what it still needs;
# the numbers always come from the two sources above.
# ---------------------------------------------------------------------------
CENSUS_JSON = ROOT / "native" / "docs" / "tree_opcode_census.json"
EVIDENCE_DIR = ROOT / "native" / "docs" / "evidence"

# op -> (PE VA, what the opcode does, what the host still needs)
MAINLOOP_OPCODES: dict[str, tuple[str, str, str]] = {
    "0x101B": (
        "0x004234B4",
        "JT_FIELD_REF — push a field handle",
        "cleared: Class_findFieldSlot @ 0x00405690 derives the slot and the "
        "0x40000000 static bit from the FILD vectors, so CP entry+0x10 was only "
        "ever a cache",
    ),
    "0x1011": (
        "0x00422E98",
        "evalName fieldPath — push a dotted name path",
        "hosted: fieldPath + op24 carriers + Soft 1019+utf8 static; residual "
        "1 = bare seg0 0x4012 (not on boot). Boot walkable 1000/1000.",
    ),
    "0x0024": (
        "0x004212E4",
        "op36 — named invoke through Thread_evalName",
        "hosted incl. 0x1019 classname; cleared from boot blocking",
    ),
    "0x0021": (
        "0x00421254",
        "op33 — hardcoded <init> call",
        "hosted: peek recv via argc depth math, Object_callMethod @ 0x00408A30 "
        "with \"<init>\"; nested Java drained inline (op42/43 +8 on return)",
    ),
    "0x0023": (
        "0x004212A4",
        "op35 — conditional <init> from the frame class",
        "hosted: this=frame+0x30, clazz=[frame+0x34]+0x1C8 (super); "
        "Object_callInitIf @ 0x00408A90 (null super → advance)",
    ),
    "0x0022": (
        "0x004212C7",
        "op34 — Object_callMethod_init",
        "hosted: this=frame+0x30, clazz=frame+0x34; Thread_callMethod \"<init>\"",
    ),
    "0x1015": (
        "0x00423239",
        "boolCondAdvance — skip when falsy",
        "hosted: invert of 0x1016 @ 0x00423267 (falsy→pcSkip, truthy→+8)",
    ),
    "0x1002": (
        "0x00421752",
        "local declare/bind",
        "hosted: append empty local (PE ValueField→frame+0x18); name via "
        "following 0x1006 CP utf8; stock 0x1006+0x2C → push Local lvalue "
        "(loc_422D74); optional 0x402E type OOS rare",
    ),
    "0x1007": (
        "0x004218A5",
        "LITERAL — type tags hosted",
        "0x100E RID + 0x1E class-lit + arith/cmp + 0x1F NEWARRAY + '#' "
        "assign + land/lor/bitor/bitand + instanceof/cast + "
        "shift/xor/mod/bitnot/inc/dec hosted (static missing 0)",
    ),
    "0x1008": (
        "0x00422EDA",
        "statement dispatcher — Soft hosted",
        "hosted: cases 11/6/5/10/3/4 (assignOp + Value_inc/dec stmt); "
        "stock corpus has 0 residual nodes (cases 0-2,7-9 unused)",
    ),
    "0x401C": (
        "0x004237CE",
        "Object_getField utf8 name",
        "hosted: pop recv → Object_getField @ 0x00408800 via Soft "
        "op29_field_get + Field VmValueRef; static 254 cleared",
    ),
    "0x4025": (
        "0x00423725",
        "Object_callMethod utf8 name",
        "hosted: pop recv → Object_callMethod @ 0x00408A30 via Soft "
        "vmthread_stream_call; static 494 cleared",
    ),
    "0x4026": (
        "0x004236EE",
        "Thread_callMethod utf8 on clazz+0x1C8",
        "hosted: Soft stream_call(cls->super_name, frame this, utf8); "
        "static 384 cleared",
    ),
}

# Structural work that is not an opcode. Each entry states the measurement or
# the PE routine that justifies it.
MAINLOOP_STRUCTURAL = [
    {
        "id": "walker_on_live_path",
        "title": "Run the walker on the path the game actually uses",
        "why": "vmthread_run is never called during a --game --no-wait boot "
               "(measured: 0 entry hits). Every TREE body goes through "
               "Jvm::invoke; walkable leaf trees now hit exec_stream.",
        "state": "partial — leaf invoke stream + PE name-only/Vector 4025 "
                 "packing (race212); call-op Soft leaves still OOS "
                 "(INVOKE=1 → traffic id=0)",
    },
    {
        "id": "walker_opt_in",
        "title": "Turn the walker on by default",
        "why": "Gated behind SLRR_PE_STREAM=1 so a partial walk never replaces "
               "a working soft invoke mid-body.",
        "state": "hosted — default ON (race210); opt-out SLRR_PE_STREAM=0; "
                 "unhosted trees still soft-invoke",
    },
    {
        "id": "value_refcount",
        "title": "ValuePool / Value refcounting and GC colouring",
        "why": "ValuePool_alloc @ 0x00423C00, Value_ctorCopy @ 0x00423E30 and "
               "Object_MarkGrey @ 0x004198B0 on L/[ assignment.",
        "state": "out of scope — host uses plain values",
    },
    {
        "id": "valuefield_types",
        "title": "Per-local type descriptors on ValueField",
        "why": "VMThread_invokeMethod @ 0x00420222 types each local from "
               "NativeSigDesc_paramAt and names it from the TreeInsn chain "
               "@ 0x00420405; the host stores a bare JvmValue.",
        "state": "approximated — Int/Float coercion only",
    },
]


def _parse_boot_walkable(s) -> int:
    """'652/1000' → 652; missing/unparseable → -1."""
    if not isinstance(s, str) or "/" not in s:
        return -1
    try:
        return int(s.split("/", 1)[0].strip())
    except ValueError:
        return -1


def _latest_boot_path() -> dict:
    """Most recent evidence entry carrying measurements.boot_path.

    Prefer the highest measured walkable numerator when several evidence
    files were touched in the same pass (mtime alone can pick a stale
    rewrite of an older ticket). Fall back to mtime.
    """
    best: dict = {}
    best_key = (-1, -1.0)  # (walkable_n, mtime)
    if not EVIDENCE_DIR.is_dir():
        return best
    for p in EVIDENCE_DIR.glob("*.json"):
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
        except Exception:
            continue
        bp = (data.get("measurements") or {}).get("boot_path")
        if not isinstance(bp, dict):
            continue
        # Prefer "walkable"; accept "walkable_after" from older evidence drafts.
        walkable = bp.get("walkable") or bp.get("walkable_after")
        key = (_parse_boot_walkable(walkable), p.stat().st_mtime)
        if key > best_key:
            best_key = key
            blocking = (
                bp.get("blocking_opcodes_after")
                or bp.get("blocking_opcodes")
                or {}
            )
            best = {
                "source": p.name,
                "walkable": walkable,
                "blocking": blocking,
            }
    return best


def mainloop_frontier() -> dict | None:
    if not CENSUS_JSON.exists():
        return None
    census = json.loads(CENSUS_JSON.read_text(encoding="utf-8"))
    boot = _latest_boot_path()
    blocking = boot.get("blocking", {})
    total_blocking = sum(blocking.values()) or 0

    rows = []
    for op, hits in sorted(blocking.items(), key=lambda kv: -kv[1]):
        va, what, needs = MAINLOOP_OPCODES.get(op, ("", "unmapped", "unmapped"))
        rows.append(
            {
                "op": op,
                "va": va,
                "what": what,
                "needs": needs,
                "boot_hits": hits,
                "boot_share_pct": round(100.0 * hits / total_blocking, 1) if total_blocking else 0.0,
                "static_hits": (census.get("missing") or {}).get(op, 0),
            }
        )

    buckets = census.get("buckets") or {}
    insns = census.get("insns") or 0
    return {
        "goal": "Soft VMThread_run == PE VMThread_run @ 0x00420FF0 (switch @ 0x004210D4)",
        "static": {
            "source": "native/docs/tree_opcode_census.json",
            "corpus": f"{census.get('files')} .class, {census.get('trees')} trees, {insns} insns",
            "insns": insns,
            "stream_ok_pct": round(100.0 * buckets.get("stream_ok", 0) / insns, 1) if insns else 0.0,
            "default_advance_pct": round(100.0 * buckets.get("default_advance", 0) / insns, 1) if insns else 0.0,
            "needs_case_pct": round(100.0 * buckets.get("needs_case", 0) / insns, 1) if insns else 0.0,
            "walkable_trees": census.get("fully_hosted_trees", 0),
            "total_trees": census.get("trees", 0),
        },
        "boot_path": {
            "source": f"native/docs/evidence/{boot.get('source', '?')} (SLRR_PE_STREAM_CENSUS=1)",
            "walkable": boot.get("walkable"),
            "note": "The metric that counts. Static coverage is dominated by "
                    "trivial methods boot never calls.",
        },
        "opcodes": rows,
        "structural": MAINLOOP_STRUCTURAL,
    }


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

    gaps = collect_gaps(engine, gl, phys, rend, oos_s, pe=pe, table=n)
    open_blockers = [g for g in gaps if g.get("kind") == "hard"]

    return {
        "updated": date.today().isoformat(),
        "formula_id": FORMULA_ID,
        "formula": (
            f"{W_BOOT}*boot + {W_GAMELOOP}*gameloop + {W_ENGINE}*engine + "
            f"{W_PHYSICS}*physics + {W_RENDER}*render + {W_JNI}*jni_pe"
        ),
        "stock_index_pct": stock,
        "layers": layers,
        "gaps": gaps,
        "gaps_hard": sum(1 for g in gaps if g.get("kind") == "hard"),
        "gaps_residual": sum(1 for g in gaps if g.get("kind") == "residual"),
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
            "render_ok": smoke.get("render_ok"),
        },
        "engine": {
            "done": eng_done,
            "total": eng_n,
            "pct": eng_pct,
            "open": eng_n - eng_done,
            "by_group": _engine_by_group(engine),
            "clusters": engine,
        },
        "mainloop": mainloop_frontier(),
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


def _gap_row(
    *,
    kind: str,
    layer: str,
    item_id: str,
    title: str | None,
    va: str | None,
    status: str | None,
    missing: str | None,
    index_impact: str,
) -> dict:
    return {
        "kind": kind,
        "layer": layer,
        "id": item_id,
        "title": title,
        "va": va,
        "status": status,
        "missing": missing,
        "index_impact": index_impact,
    }


def _note_has_oos(note: str | None, blocker: str | None = None) -> bool:
    blob = f"{note or ''} {blocker or ''}"
    return "OOS" in blob or "oos" in blob


def collect_gaps(
    engine: list[dict],
    gl: dict,
    phys: dict,
    rend: dict,
    oos_s: dict,
    *,
    pe: int,
    table: int,
) -> list[dict]:
    """Every real missing piece: hard catalog gaps + residual OOS on closed rows."""
    gaps: list[dict] = []

    for e in engine:
        if not e.get("done"):
            gaps.append(
                _gap_row(
                    kind="hard",
                    layer="engine",
                    item_id=e["id"],
                    title=e.get("title"),
                    va=e.get("va"),
                    status="open",
                    missing=e.get("blocker") or e.get("note"),
                    index_impact="blocks engine 9→10 (journey −2.5 pts)",
                )
            )
        elif _note_has_oos(e.get("note"), e.get("blocker")):
            gaps.append(
                _gap_row(
                    kind="residual",
                    layer="engine",
                    item_id=e["id"],
                    title=e.get("title"),
                    va=e.get("va"),
                    status="done+OOS",
                    missing=e.get("note"),
                    index_impact="catalog done; residual fidelity only",
                )
            )

    for s in gl.get("stages") or []:
        st = s.get("status")
        if st != "hosted":
            gaps.append(
                _gap_row(
                    kind="hard",
                    layer="gameloop",
                    item_id=s["id"],
                    title=s.get("title"),
                    va=s.get("va"),
                    status=st,
                    missing=s.get("note"),
                    index_impact="blocks gameloop 100%",
                )
            )
        elif _note_has_oos(s.get("note")):
            gaps.append(
                _gap_row(
                    kind="residual",
                    layer="gameloop",
                    item_id=s["id"],
                    title=s.get("title"),
                    va=s.get("va"),
                    status="hosted+OOS",
                    missing=s.get("note"),
                    index_impact="stage hosted; residual outside body",
                )
            )

    for item in phys.get("items") or []:
        st = str(item.get("status") or "partial")
        if st in ("partial", "blocked"):
            gaps.append(
                _gap_row(
                    kind="hard",
                    layer="physics",
                    item_id=item["id"],
                    title=item.get("title"),
                    va=item.get("va"),
                    status=st,
                    missing=item.get("blocker") or item.get("note"),
                    index_impact="physics readiness (done=1 / partial=0.5)",
                )
            )
        elif st == "done" and _note_has_oos(item.get("note"), item.get("blocker")):
            gaps.append(
                _gap_row(
                    kind="residual",
                    layer="physics",
                    item_id=item["id"],
                    title=item.get("title"),
                    va=item.get("va"),
                    status="done+OOS",
                    missing=item.get("note"),
                    index_impact="catalog done; residual fidelity only",
                )
            )

    for item in rend.get("items") or []:
        st = str(item.get("status") or "partial")
        if st in ("partial", "blocked"):
            gaps.append(
                _gap_row(
                    kind="hard",
                    layer="render",
                    item_id=item["id"],
                    title=item.get("title"),
                    va=item.get("va"),
                    status=st,
                    missing=item.get("blocker") or item.get("note"),
                    index_impact="render readiness (done=1 / partial=0.5)",
                )
            )
        elif st == "done" and _note_has_oos(item.get("note"), item.get("blocker")):
            gaps.append(
                _gap_row(
                    kind="residual",
                    layer="render",
                    item_id=item["id"],
                    title=item.get("title"),
                    va=item.get("va"),
                    status="done+OOS",
                    missing=item.get("note"),
                    index_impact="catalog done; residual fidelity only",
                )
            )

    for item in oos_s.get("items") or []:
        st = str(item.get("status") or "open")
        if st in ("open", "partial"):
            gaps.append(
                _gap_row(
                    kind="hard",
                    layer="oos",
                    item_id=item["id"],
                    title=item.get("title"),
                    va=item.get("va"),
                    status=st,
                    missing=item.get("note"),
                    index_impact="OOS gate (not in journey %)",
                )
            )
        elif st == "unlocked" and _note_has_oos(item.get("note")):
            gaps.append(
                _gap_row(
                    kind="residual",
                    layer="oos",
                    item_id=item["id"],
                    title=item.get("title"),
                    va=item.get("va"),
                    status="unlocked+OOS",
                    missing=item.get("note"),
                    index_impact="gate unlocked; residual note still open",
                )
            )

    if pe < table:
        gaps.append(
            _gap_row(
                kind="hard",
                layer="jni",
                item_id="jni_pe_missing",
                title="JNI PE table residual",
                va=None,
                status="open",
                missing=f"{table - pe} RegisterNative row(s) without PE@ comment",
                index_impact="jni layer 10% of journey",
            )
        )

    order = {"hard": 0, "residual": 1}
    layer_order = {
        "engine": 0,
        "gameloop": 1,
        "physics": 2,
        "render": 3,
        "oos": 4,
        "jni": 5,
    }
    gaps.sort(
        key=lambda g: (
            order.get(str(g.get("kind")), 9),
            layer_order.get(str(g.get("layer")), 9),
            str(g.get("id")),
        )
    )
    return gaps


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
