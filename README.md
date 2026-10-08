# slrr-reverse

Reverse host for *Street Legal Racing: Redline* (Invictus `StreetLegal_Redline.exe`).

C++ rewrite that boots stock / typical-mod Java via a TREE + VA-backed native
table. 

## Layout

| Path | Purpose |
|------|---------|
| `engine/` | MSVC / CMake host (`core/`, `runtime/`, `include/`, `data/`) |
| `tools/` | Inventory / stub codegen / IDA registry apply helpers |

No game install dump, no IDA session docs, no Cursor rules in this repo.

### Engine splits (current)

Large `.cpp` bodies are split into `*_partN.inc` fragments included by a
thin shell (128 KB editor buffer cap). CMake still lists the `.cpp` only.

| Area | Files |
|------|--------|
| JVM | `jvm.cpp` + `jvm_part*.inc` / `jvm_vmthread` + parts / register / load |
| TREE | `tree_fields.cpp` / `tree_eval.cpp` + `tree_eval_part*.inc` |
| GameRef | `GameRef.cpp` + `GameRef_part*.inc` / core / collision |
| Ground / phys | `GroundRef_route` / `GroundRef_traffic` / `PhysicsRef` |
| System | `System.cpp` + `System_part*.inc` / `System_natives.cpp` |
| Platform | `render_d3d9.cpp` + `render_d3d9_part*.inc` |

## Build (MSVC Win32)

Needs Visual Studio 2022 with C++ desktop workload.

```powershell
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
  -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S engine -B engine/build -G "Visual Studio 17 2022" -A Win32
& $cmake --build engine/build --config Release
```

Run against a local SLRR install (not shipped here):

```powershell
cd <path-to-slrr-game>
..\slrr-reverse\engine\build\Release\slrr_engine.exe --game --no-wait
```

## License

MIT — see [LICENSE](LICENSE). Game assets and the stock executable remain
property of their respective owners; this repo does not redistribute them.
