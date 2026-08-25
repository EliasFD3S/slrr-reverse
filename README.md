# slrr-reverse

Reverse host for *Street Legal Racing: Redline* (Invictus `StreetLegal_Redline.exe`).

C++ rewrite that boots stock / typical-mod Java via a TREE + VA-backed native table.

## Layout

| Path | Purpose |
|------|---------|
| `engine/` | MSVC / CMake host (`Core/`, `Runtime/`, `include/`, `data/`) |
| `tools/` | Inventory / stub codegen / IDA registry apply helpers |

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

MIT — see [LICENSE](LICENSE). Game assets and the stock executable remain property of their respective owners; this repo does not redistribute them.
