"""Export a few native VAs into ghidra-ai-bridge JSON layout (fast vs full dump).

Usage (from repo root, after ghidra_bridge_env.ps1):
  py -3.11 native/tools/ghidra_export_natives.py
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

# Camera + nearby render natives we care about for Phase 2.
TARGETS = {
    0x004861E0: "java.render.Camera.create",
    0x004864C0: "java.render.Camera.destroy",
    0x00486570: "java.render.Camera.setFog",
    0x0047C2D0: "java.render.GfxEngine.flush",
    0x0047C1D0: "java.render.GfxEngine.forceRendering",
    0x0047C440: "java.render.GfxEngine.numDisplayModes",
    0x0047FFE0: "java.util.resource.ResourceRef.makeTexture",
    0x0047C220: "java.render.GfxEngine.setGlobalEnvmap",
}


def main() -> int:
    os.environ.setdefault(
        "JAVA_HOME",
        r"C:\Program Files\Eclipse Adoptium\jdk-21.0.6.7-hotspot",
    )
    os.environ.setdefault(
        "GHIDRA_INSTALL_DIR",
        str(REPO / "ghidra_12.1.2_PUBLIC"),
    )

    import pyghidra

    out_dir = REPO / "native" / "ghidra" / "bridge-exports"
    out_dir.mkdir(parents=True, exist_ok=True)

    project_dir = str(REPO / "native" / "ghidra" / "project")
    project_name = "slrr_native"
    program_name = "StreetLegal_Redline.exe"

    print(f"Opening {project_dir}/{project_name} :: {program_name}")
    pyghidra.start(verbose=False)

    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor

    index = {}
    if (out_dir / "_index.json").exists():
        index = json.loads((out_dir / "_index.json").read_text(encoding="utf-8"))

    with pyghidra.open_program(
        None,
        project_location=project_dir,
        project_name=project_name,
        program_name=program_name,
        analyze=False,
        nested_project_location=False,
    ) as flat_api:
        program = flat_api.getCurrentProgram()
        fm = program.getFunctionManager()
        ref_mgr = program.getReferenceManager()
        listing = program.getListing()
        af = program.getAddressFactory()

        decomp = DecompInterface()
        decomp.openProgram(program)
        monitor = ConsoleTaskMonitor()

        for va, label in TARGETS.items():
            addr = af.getDefaultAddressSpace().getAddress(va)
            func = fm.getFunctionAt(addr)
            if func is None:
                func = fm.getFunctionContaining(addr)
            if func is None:
                try:
                    flat_api.disassemble(addr)
                    func = flat_api.createFunction(addr, label.replace(".", "_"))
                except Exception as e:
                    print(f"MISS create failed 0x{va:08X} ({label}): {e}")
                    continue
            if func is None:
                print(f"MISS no function at 0x{va:08X} ({label})")
                continue

            entry = str(func.getEntryPoint())
            name = func.getName()
            try:
                result = decomp.decompileFunction(func, 120, monitor)
                if result.decompileCompleted():
                    c_code = result.getDecompiledFunction().getC()
                else:
                    c_code = "// Decompilation failed"
            except Exception as e:
                c_code = f"// Error: {e}"

            callers = []
            for ref in ref_mgr.getReferencesTo(func.getEntryPoint()):
                caller = fm.getFunctionContaining(ref.getFromAddress())
                if caller and caller != func:
                    callers.append(
                        {
                            "addr": str(caller.getEntryPoint()),
                            "name": caller.getName(),
                            "ref_type": str(ref.getReferenceType()),
                        }
                    )

            assembly = []
            try:
                assembly = [
                    f"{ins.getAddress()}  {ins}"
                    for ins in listing.getInstructions(func.getBody(), True)
                ]
            except Exception:
                pass

            payload = {
                "address": entry,
                "name": name,
                "label_hint": label,
                "signature": str(func.getSignature()),
                "calling_convention": str(func.getCallingConventionName()),
                "decompiled": c_code,
                "callers": callers,
                "callees": [],
                "assembly": assembly,
            }

            # Bridge lookup uses 8-char hex filenames.
            safe = f"{va:08x}"
            path = out_dir / f"{safe}.json"
            path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
            index[entry] = {
                "name": name,
                "address": entry,
                "label_hint": label,
            }
            # Also write under Ghidra-style key if different.
            ghidra_safe = entry.replace(":", "_")
            if ghidra_safe != safe:
                (out_dir / f"{ghidra_safe}.json").write_text(
                    json.dumps(payload, indent=2), encoding="utf-8"
                )
            print(f"OK 0x{va:08X} -> {path.name} ({name}) [{label}]")

        (out_dir / "_index.json").write_text(
            json.dumps(index, indent=2), encoding="utf-8"
        )

    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
