# Env for ghidra-ai-bridge + Ghidra 12.1.2 (JDK 21 required).
# Dot-source from repo root:
#   . .\native\tools\ghidra_bridge_env.ps1

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$jdk21 = "C:\Program Files\Eclipse Adoptium\jdk-21.0.6.7-hotspot"
$ghidra = Join-Path $repo "ghidra_12.1.2_PUBLIC"

if (-not (Test-Path (Join-Path $jdk21 "bin\java.exe"))) {
  Write-Error "JDK 21 introuvable: $jdk21"
}
if (-not (Test-Path (Join-Path $ghidra "Ghidra"))) {
  Write-Error "Ghidra introuvable: $ghidra"
}

$env:JAVA_HOME = $jdk21
$env:GHIDRA_INSTALL_DIR = "$ghidra"
# Prefer JDK 21 on PATH for this session.
$env:Path = "$(Join-Path $jdk21 'bin');$env:Path"

Write-Host "JAVA_HOME=$env:JAVA_HOME"
Write-Host "GHIDRA_INSTALL_DIR=$env:GHIDRA_INSTALL_DIR"
& "$env:JAVA_HOME\bin\java.exe" -version
