# IDAPython — apply native_registry.json (+ ABI anchors) onto the open DB.
#
# From IDA GUI:  File → Script file… → this script
# Headless:      powershell -File native/tools/run_ida_apply_registry.ps1
#
# Expects StreetLegal_Redline.exe.i64 open (or passed to idat).

from __future__ import annotations

import json
import os
import re
import traceback
from collections import defaultdict
from pathlib import Path

import ida_auto
import ida_bytes
import ida_funcs
import ida_kernwin
import ida_name
import ida_pro
import idaapi
import idc

# Confirmed ABI anchors (abi_bridge.md / jvm_anchors.md)
ABI_ANCHORS = [
    (0x00416B00, "JVM_RegisterNative"),
    (0x0045D910, "JVM_UnboxArg"),
    (0x00487F20, "Natives_RegisterAll"),
    (0x004427E0, "Natives_Register_Partial"),
    (0x0046B530, "Natives_Register_PartDyno"),
    (0x00477CB0, "Natives_Register_Controller"),
    (0x004165A0, "JVM_compileSource"),
    (0x0040FE70, "JVM_bootstrap_GameRef"),
]

# Known VAs missing from registry JSON (abi_bridge.md)
FALLBACK_VAS = {
    ("java.io.Input", "checkHotkeys", "(Ljava.util.resource.GameRef;Ljava.render.Osd;)V"): 0x0047CD40,
}

SN_FLAGS = ida_name.SN_FORCE | ida_name.SN_NOCHECK | ida_name.SN_NODUMMY


def find_root() -> Path:
    env = os.environ.get("SLRR_ROOT")
    if env:
        return Path(env)

    # GUI Script file usually sets __file__
    try:
        return Path(__file__).resolve().parents[2]
    except NameError:
        pass

    # Headless: idb lives at <root>/game/*.i64
    idb = Path(idc.get_idb_path())
    if idb.parent.name.lower() == "game":
        return idb.parent.parent

    raise RuntimeError(
        "cannot locate repo root; set SLRR_ROOT or keep idb under <root>/game/"
    )


def paths():
    root = find_root()
    return (
        root,
        root / "native" / "docs" / "native_registry.json",
        root / "native" / "docs" / "ida_apply_registry_report.json",
        root / "native" / "docs" / "ida_apply_registry.log",
    )


def log(msg: str, log_path: Path | None = None) -> None:
    line = f"[ida_apply_registry] {msg}"
    print(line)
    try:
        ida_kernwin.msg(line + "\n")
    except Exception:
        pass
    if log_path is not None:
        with log_path.open("a", encoding="utf-8") as f:
            f.write(line + "\n")


def sanitize(s: str) -> str:
    s = s.replace(".", "_").replace("/", "_").replace("$", "_").replace(";", "")
    s = s.replace("[", "A").replace("(", "").replace(")", "_")
    s = re.sub(r"[^0-9A-Za-z_]", "_", s)
    s = re.sub(r"_+", "_", s).strip("_")
    if s and s[0].isdigit():
        s = "_" + s
    return s or "anon"


def make_name(class_fqn: str, method: str, signature: str, *, disambiguate: bool) -> str:
    base = f"{sanitize(class_fqn)}_{sanitize(method)}"
    if disambiguate:
        base = f"{base}__{sanitize(signature)}"
    return base[:250]


def parse_va(s) -> int | None:
    if s is None:
        return None
    if isinstance(s, str):
        s = s.strip()
        if not s:
            return None
        return int(s, 16)
    if isinstance(s, int):
        return s
    return None


def ensure_func(ea: int) -> bool:
    if ida_funcs.get_func(ea) is not None:
        return True
    return bool(ida_funcs.add_func(ea))


def set_name_at(ea: int, name: str) -> tuple[bool, str]:
    if not ida_bytes.is_loaded(ea):
        return False, "ea_not_loaded"
    ensure_func(ea)
    ok = ida_name.set_name(ea, name, SN_FLAGS)
    if not ok:
        alt = f"{name}_{ea:X}"
        ok = ida_name.set_name(ea, alt, SN_FLAGS)
        return (True, alt) if ok else (False, "set_name_failed")
    return True, name


def set_func_comment(ea: int, text: str) -> None:
    f = ida_funcs.get_func(ea)
    if f is None:
        ida_bytes.set_cmt(ea, text, 1)
        return
    ida_funcs.set_func_cmt(f, text, 1)


def apply_abi(report: dict, log_path: Path) -> None:
    for ea, name in ABI_ANCHORS:
        ok, final = set_name_at(ea, name)
        report["abi"].append({"ea": hex(ea), "wanted": name, "ok": ok, "final": final})
        if ok:
            set_func_comment(ea, f"ABI anchor from abi_bridge.md ({name})")
            log(f"ABI {hex(ea)} -> {final}", log_path)
        else:
            log(f"ABI FAIL {hex(ea)} {name}: {final}", log_path)


def apply_registry(report: dict, registry: Path, log_path: Path) -> None:
    data = json.loads(registry.read_text(encoding="utf-8"))
    entries = data.get("entries", [])
    report["registry_count"] = len(entries)

    key_counts: dict[tuple[str, str], int] = defaultdict(int)
    for e in entries:
        key_counts[(e["class"], e["name"])] += 1

    by_va: dict[int, list] = defaultdict(list)
    skipped = 0
    for e in entries:
        ea = parse_va(e.get("impl_va"))
        if ea is None:
            key = (e.get("class"), e.get("name"), e.get("signature"))
            ea = FALLBACK_VAS.get(key)
        if ea is None:
            skipped += 1
            report.setdefault("skipped", []).append(
                {
                    "class": e.get("class"),
                    "name": e.get("name"),
                    "signature": e.get("signature"),
                    "impl_symbol": e.get("impl_symbol"),
                    "reason": "null_impl_va",
                }
            )
            continue
        by_va[ea].append(e)

    renamed = 0
    failed = 0
    for ea, group in sorted(by_va.items()):
        primary = group[0]
        need_sig = key_counts[(primary["class"], primary["name"])] > 1 or len(group) > 1
        name = make_name(
            primary["class"],
            primary["name"],
            primary["signature"],
            disambiguate=need_sig or len(group) > 1,
        )

        ok, final = set_name_at(ea, name)
        bindings = [f"{e['class']}.{e['name']}{e['signature']}" for e in group]
        if ok:
            set_func_comment(ea, "native: " + " | ".join(bindings))
            renamed += 1
        else:
            failed += 1

        report["natives"].append(
            {
                "ea": hex(ea),
                "wanted": name,
                "ok": ok,
                "final": final,
                "bindings": bindings,
            }
        )

    report["renamed"] = renamed
    report["failed"] = failed
    report["skipped_count"] = skipped
    log(
        f"natives renamed={renamed} failed={failed} skipped={skipped} unique_va={len(by_va)}",
        log_path,
    )


def main() -> int:
    root, registry, report_path, log_path = paths()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text("", encoding="utf-8")

    log(f"root={root}", log_path)
    log(f"idb={idc.get_idb_path()}", log_path)
    log(f"input={idaapi.get_input_file_path()}", log_path)
    log(f"registry={registry}", log_path)
    log(f"batch_env={os.environ.get('IDA_APPLY_BATCH')!r}", log_path)

    if not registry.is_file():
        log(f"missing registry: {registry}", log_path)
        return 1

    report = {
        "database": idaapi.get_input_file_path(),
        "idb": idc.get_idb_path(),
        "registry": str(registry),
        "abi": [],
        "natives": [],
    }

    ida_auto.auto_wait()
    apply_abi(report, log_path)
    apply_registry(report, registry, log_path)

    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    log(f"report -> {report_path}", log_path)

    idc.save_database(idc.get_idb_path(), 0)
    log("database saved", log_path)
    return 0


def _run() -> None:
    rc = 1
    log_path = None
    try:
        try:
            _, _, _, log_path = paths()
        except Exception:
            log_path = Path(idc.get_idb_path()).with_suffix(".apply_registry.log")
        rc = main()
    except Exception:
        tb = traceback.format_exc()
        print(tb)
        try:
            ida_kernwin.msg(tb + "\n")
        except Exception:
            pass
        if log_path is not None:
            try:
                with log_path.open("a", encoding="utf-8") as f:
                    f.write(tb + "\n")
            except Exception:
                pass
        rc = 1
    finally:
        if os.environ.get("IDA_APPLY_BATCH") == "1":
            ida_pro.qexit(rc)


_run()
