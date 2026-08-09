[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Debug",
    [string]$CMakeGenerator = "Visual Studio 16 2019",
    [switch]$NoDeploy
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $repoRoot "build"
$sdkDirectory = Join-Path $repoRoot ".deps\aviutl2_sdk"
$runtimeDirectory = Join-Path $repoRoot "sandbox\AviUtl2"
$runtimeExecutable = Join-Path $runtimeDirectory "aviutl2.exe"
$pluginDirectory = Join-Path $runtimeDirectory "data\Plugin\mojie"
$pluginFile = Join-Path $buildDirectory "bin\$Configuration\mojie.aux2"
$aliasDirectory = Join-Path $runtimeDirectory "data\Alias"
$aliasSource = Join-Path $repoRoot "assets\Alias\mojieテキスト.object"

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory)]
        [string]$Command,
        [Parameter(Mandatory)]
        [string[]]$Arguments
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Command $Arguments"
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $sdkDirectory "include\aviutl2_sdk\plugin2.h") -PathType Leaf)) {
    throw "AviUtl2 SDK is missing. Run .\scripts\setup.ps1 first."
}

if (-not $NoDeploy -and (Test-Path -LiteralPath $runtimeExecutable -PathType Leaf)) {
    $runningRuntime = Get-Process aviutl2 -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $runtimeExecutable } |
        Select-Object -First 1
    if ($runningRuntime) {
        throw "The isolated AviUtl2 is running (PID $($runningRuntime.Id)). Close it before building and deploying the plugin."
    }
}

Invoke-CheckedCommand -Command cmake -Arguments @(
    "-S", $repoRoot,
    "-B", $buildDirectory,
    "-G", $CMakeGenerator,
    "-A", "x64",
    "-DMOJIE_AVIUTL2_SDK_DIR=$sdkDirectory"
)
Invoke-CheckedCommand -Command cmake -Arguments @("--build", $buildDirectory, "--config", $Configuration)

if (-not (Test-Path -LiteralPath $pluginFile -PathType Leaf)) {
    throw "Build completed but the plugin was not found: $pluginFile"
}

if (-not $NoDeploy) {
    if (-not (Test-Path -LiteralPath $runtimeExecutable -PathType Leaf)) {
        throw "The test runtime is missing. Run .\scripts\setup.ps1 first."
    }

    New-Item -ItemType Directory -Path $pluginDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $aliasDirectory -Force | Out-Null
    Copy-Item -LiteralPath $pluginFile -Destination (Join-Path $pluginDirectory "mojie.aux2") -Force
    Copy-Item -LiteralPath $aliasSource -Destination (Join-Path $aliasDirectory "mojieテキスト.object") -Force
    Write-Host "Deployed: $pluginDirectory\mojie.aux2"
    Write-Host "Deployed: $aliasDirectory\mojieテキスト.object"
}

Write-Host "Built: $pluginFile"
