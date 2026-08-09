[CmdletBinding()]
param(
    [ValidateSet("Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$Version,
    [switch]$SkipBuild,
    [switch]$RequireLicense
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$cmakeFile = Join-Path $repoRoot "CMakeLists.txt"
$buildDirectory = Join-Path $repoRoot "build"
$pluginFile = Join-Path $buildDirectory "bin\$Configuration\mojie.aux2"
$aliasFile = Join-Path $repoRoot "assets\Alias\mojieテキスト.object"
$distDirectory = Join-Path $repoRoot "dist"

$cmake = Get-Content -LiteralPath $cmakeFile -Raw
$match = [regex]::Match($cmake, '(?s)project\s*\(\s*mojie\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $match.Success) {
    throw "The project version could not be read from CMakeLists.txt."
}
$suffixMatch = [regex]::Match(
    $cmake,
    'set\s*\(\s*MOJIE_VERSION_SUFFIX\s+"([0-9A-Za-z.-]*)"\s*\)')
if (-not $suffixMatch.Success) {
    throw "The project version suffix could not be read from CMakeLists.txt."
}
$projectVersion = $match.Groups[1].Value + $suffixMatch.Groups[1].Value
if (-not $Version) {
    $Version = $projectVersion
}

if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:a[0-9]+)?$') {
    throw "Version must use X.Y.Z or X.Y.ZaN without a leading v: $Version"
}
if ($Version -ne $projectVersion) {
    throw "Package version $Version does not match CMake project version $projectVersion."
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration -NoDeploy
    if ($LASTEXITCODE -ne 0) {
        throw "Release build failed with exit code $LASTEXITCODE."
    }
}

foreach ($requiredFile in @(
    $pluginFile,
    $aliasFile,
    (Join-Path $repoRoot "README.md"),
    (Join-Path $repoRoot "THIRD_PARTY_NOTICES.md")
)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "A release file is missing: $requiredFile"
    }
}

$archivePath = Join-Path $distDirectory "mojie-v$Version.au2pkg.zip"
$checksumPath = "$archivePath.sha256"
$licenseFile = Join-Path $repoRoot "LICENSE"
$archiveEntries = @(
    [pscustomobject]@{ Source = $pluginFile; ArchivePath = "Plugin/mojie/mojie.aux2" },
    [pscustomobject]@{ Source = $aliasFile; ArchivePath = "Alias/mojieテキスト.object" },
    [pscustomobject]@{ Source = (Join-Path $repoRoot "README.md"); ArchivePath = "README.md" },
    [pscustomobject]@{ Source = (Join-Path $repoRoot "THIRD_PARTY_NOTICES.md"); ArchivePath = "THIRD_PARTY_NOTICES.md" }
)
if (Test-Path -LiteralPath $licenseFile -PathType Leaf) {
    $archiveEntries += [pscustomobject]@{ Source = $licenseFile; ArchivePath = "LICENSE" }
} elseif ($RequireLicense) {
    throw "LICENSE is required for a publishable release."
} else {
    Write-Warning "LICENSE is not present. Choose a project license before publishing the release."
}

New-Item -ItemType Directory -Path $distDirectory -Force | Out-Null
if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
    Remove-Item -LiteralPath $archivePath -Force
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$fixedTimestamp = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
$outputStream = [System.IO.File]::Open(
    $archivePath,
    [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::ReadWrite,
    [System.IO.FileShare]::None)
try {
    $archive = [System.IO.Compression.ZipArchive]::new(
        $outputStream,
        [System.IO.Compression.ZipArchiveMode]::Create,
        $false)
    try {
        foreach ($item in $archiveEntries) {
            $entry = $archive.CreateEntry(
                $item.ArchivePath,
                [System.IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $fixedTimestamp
            $sourceStream = [System.IO.File]::OpenRead($item.Source)
            $entryStream = $entry.Open()
            try {
                $sourceStream.CopyTo($entryStream)
            } finally {
                $entryStream.Dispose()
                $sourceStream.Dispose()
            }
        }
    } finally {
        $archive.Dispose()
    }
} finally {
    $outputStream.Dispose()
}

$expectedEntries = @($archiveEntries | ForEach-Object { $_.ArchivePath })
$archive = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    $entries = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    if ($entries.Count -ne $expectedEntries.Count) {
        throw "The release archive contains an unexpected number of entries."
    }
    for ($index = 0; $index -lt $expectedEntries.Count; ++$index) {
        if ($entries[$index] -cne $expectedEntries[$index]) {
            throw "Unexpected archive entry '$($entries[$index])'; expected '$($expectedEntries[$index])'."
        }
    }
} finally {
    $archive.Dispose()
}

$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath $checksumPath -Value "$hash  $(Split-Path -Leaf $archivePath)" -Encoding ascii

Write-Host "Package: $archivePath"
Write-Host "SHA-256: $hash"
