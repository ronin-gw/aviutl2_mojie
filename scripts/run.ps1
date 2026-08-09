[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Debug",
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$runtimeDirectory = Join-Path $repoRoot "sandbox\AviUtl2"
$executable = Join-Path $runtimeDirectory "aviutl2.exe"

if (Test-Path -LiteralPath $executable -PathType Leaf) {
    $runningRuntime = Get-Process aviutl2 -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $executable } |
        Select-Object -First 1
    if ($runningRuntime) {
        throw "The isolated AviUtl2 is already running (PID $($runningRuntime.Id))."
    }
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }
}

if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "The test runtime is missing. Run .\scripts\setup.ps1 first."
}

Start-Process -FilePath $executable -WorkingDirectory $runtimeDirectory
Write-Host "Started isolated AviUtl2: $executable"
