# Apply native_registry.json names onto game/StreetLegal_Redline.exe.i64 via IDA headless.
# Close the DB in the IDA GUI first (exclusive lock on .id0).

param(
    [string]$Idb = "",
    [string]$IdaRoot = "C:\Program Files\IDA Professional 9.4"
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $Idb) {
    $Idb = Join-Path $Root "game\StreetLegal_Redline.exe.i64"
}
$Script = Join-Path $PSScriptRoot "ida_apply_registry.py"
$Idat = Join-Path $IdaRoot "idat.exe"
$Log = Join-Path $Root "native\docs\ida_apply_registry.log"

if (-not (Test-Path -LiteralPath $Idat)) { throw "idat.exe not found: $Idat" }
if (-not (Test-Path -LiteralPath $Idb)) { throw "IDB not found: $Idb" }
if (-not (Test-Path -LiteralPath $Script)) { throw "Script not found: $Script" }

$id0 = [IO.Path]::ChangeExtension($Idb, ".id0")
if (Test-Path -LiteralPath $id0) {
    try {
        $fs = [IO.File]::Open($id0, "Open", "ReadWrite", "None")
        $fs.Close()
    } catch {
        throw @"
IDB is locked (IDA GUI probably has it open):
  $Idb

Either:
  1) In IDA: File -> Script file -> native\tools\ida_apply_registry.py
  2) Close the database / IDA, then re-run this script.
"@
    }
}

$env:IDA_APPLY_BATCH = "1"
$env:SLRR_ROOT = $Root

Write-Host "idat:   $Idat"
Write-Host "idb:    $Idb"
Write-Host "script: $Script"
Write-Host "root:   $Root"

# IDA wants -S immediately followed by the script path (no separate argv quoting tricks).
# Use a short wrapper in TEMP if the path has spaces (safer for -S parser).
$scriptForIda = $Script
if ($Script -match "\s") {
    $wrapper = Join-Path $env:TEMP "ida_apply_registry_run.py"
    $rootEsc = $Root.Replace("\", "\\")
    $scriptEsc = $Script.Replace("\", "\\")
    @"
import os, runpy
os.environ['IDA_APPLY_BATCH'] = '1'
os.environ['SLRR_ROOT'] = '$rootEsc'
runpy.run_path(r'$scriptEsc', run_name='__main__')
"@ | Set-Content -Encoding utf8 $wrapper
    $scriptForIda = $wrapper
    Write-Host "wrapper:$wrapper"
}

$argS = "-S$scriptForIda"
Write-Host "cmdline: $Idat -A $argS `"$Idb`""

$p = Start-Process -FilePath $Idat -ArgumentList @("-A", $argS, $Idb) -Wait -PassThru -NoNewWindow
$rc = $p.ExitCode
Write-Host "idat exit: $rc"

if (Test-Path -LiteralPath $Log) {
    Write-Host "----- log -----"
    Get-Content -LiteralPath $Log
    Write-Host "---------------"
} else {
    Write-Host "no log at $Log (script likely never started)"
}

$report = Join-Path $Root "native\docs\ida_apply_registry_report.json"
if (Test-Path -LiteralPath $report) {
    Write-Host "report: $report"
}
exit $rc
