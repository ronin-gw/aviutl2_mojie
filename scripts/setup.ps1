[CmdletBinding()]
param(
    [string]$AviUtl2Source = "C:\Program Files\AviUtl2",
    [switch]$SkipToolchainCheck
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$dependencyRoot = Join-Path $repoRoot ".deps"
$sdkDirectory = Join-Path $dependencyRoot "aviutl2_sdk"
$runtimeDirectory = Join-Path $repoRoot "sandbox\AviUtl2"
$pluginDirectory = Join-Path $runtimeDirectory "data\Plugin\mojie"
$fontDirectory = Join-Path $runtimeDirectory "data\Font\mojie"
$sdkRepository = "https://github.com/aviutl2/aviutl2_sdk_mirror.git"
$sdkRevision = "5753e971f831ccc27d06e89920914001650d0224"

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

if (-not (Test-Path -LiteralPath $AviUtl2Source -PathType Container)) {
    throw "AviUtl2 source directory was not found: $AviUtl2Source"
}

$aviUtl2Executable = Join-Path $AviUtl2Source "aviutl2.exe"
if (-not (Test-Path -LiteralPath $aviUtl2Executable -PathType Leaf)) {
    throw "aviutl2.exe was not found in: $AviUtl2Source"
}

New-Item -ItemType Directory -Path $dependencyRoot -Force | Out-Null

if (-not (Test-Path -LiteralPath (Join-Path $sdkDirectory ".git") -PathType Container)) {
    Invoke-CheckedCommand -Command git -Arguments @("clone", "--filter=blob:none", "--no-checkout", $sdkRepository, $sdkDirectory)
}

Push-Location $sdkDirectory
try {
    Invoke-CheckedCommand -Command git -Arguments @("fetch", "--depth=1", "origin", $sdkRevision)
    Invoke-CheckedCommand -Command git -Arguments @("checkout", "--detach", $sdkRevision)
} finally {
    Pop-Location
}

New-Item -ItemType Directory -Path $runtimeDirectory -Force | Out-Null
Get-ChildItem -LiteralPath $AviUtl2Source -File |
    Where-Object { $_.Name -ne "aviutl2_setup.exe" } |
    Copy-Item -Destination $runtimeDirectory -Force

New-Item -ItemType Directory -Path $pluginDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $fontDirectory -Force | Out-Null

Write-Host "AviUtl2 SDK: $sdkDirectory"
Write-Host "Test runtime: $runtimeDirectory"
Write-Host "Emoji images: $fontDirectory"

if ($SkipToolchainCheck) {
    return
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake was not found. Install CMake 3.16 or newer."
}

$vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vsWhere -PathType Leaf)) {
    throw "Visual Studio Installer (vswhere.exe) was not found."
}

$visualStudio = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) {
    $visualStudio = & $vsWhere -latest -products * -property installationPath
}

$msvcCompiler = $null
if ($visualStudio) {
    $msvcCompiler = Get-ChildItem -LiteralPath (Join-Path $visualStudio "VC\Tools\MSVC") -Filter cl.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "Hostx64\\x64\\cl\.exe$" } |
        Select-Object -First 1
}
if (-not $msvcCompiler) {
    throw "MSVC x64 tools were not found. Install the Desktop development with C++ workload."
}

$windowsKitRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include"
$windowsHeader = Get-ChildItem -LiteralPath $windowsKitRoot -Filter windows.h -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $windowsHeader) {
    throw "Windows 10 SDK headers were not found. Add Windows 10 SDK (10.0.19041.0 or newer) from Visual Studio Installer."
}

Write-Host "Visual Studio: $visualStudio"
Write-Host "MSVC compiler: $($msvcCompiler.FullName)"
Write-Host "Windows SDK: $($windowsHeader.Directory.Parent.FullName)"
Write-Host "Setup completed. Run .\scripts\build.ps1 next."
