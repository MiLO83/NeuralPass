[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$TargetPath,
    [string]$PackagePath,
    [switch]$ForceModified,
    [string]$Confirmation
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($PackagePath)) { $PackagePath = $scriptRoot }
$package = (Get-Item -LiteralPath $PackagePath -ErrorAction Stop).FullName

function Test-PackageIntegrity {
    $checksumPath = Join-Path $package 'SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $checksumPath -PathType Leaf)) {
        throw 'The update package has no SHA256SUMS.txt.'
    }
    $expected = [Collections.Generic.Dictionary[string,string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $packagePrefix = $package.TrimEnd([IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    foreach ($line in Get-Content -LiteralPath $checksumPath) {
        if ($line -notmatch '^([0-9a-fA-F]{64}) \*(.+)$') {
            throw "Invalid checksum manifest line: $line"
        }
        $relative = $Matches[2].Replace('/', [IO.Path]::DirectorySeparatorChar)
        $candidate = [IO.Path]::GetFullPath((Join-Path $package $relative))
        if (-not $candidate.StartsWith($packagePrefix,
                [StringComparison]::OrdinalIgnoreCase) -or
            $expected.ContainsKey($relative)) {
            throw "Unsafe or duplicate checksum path: $relative"
        }
        $expected.Add($relative, $Matches[1].ToUpperInvariant())
    }
    $actual = @(Get-ChildItem -LiteralPath $package -File -Recurse | Where-Object {
        $_.FullName -ne $checksumPath
    })
    if ($actual.Count -ne $expected.Count) {
        throw 'Update package file count does not match SHA256SUMS.txt.'
    }
    foreach ($item in $actual) {
        $relative = $item.FullName.Substring($package.Length + 1)
        if (-not $expected.ContainsKey($relative)) {
            throw "Update package file is absent from SHA256SUMS.txt: $relative"
        }
        $actualHash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
        if ($actualHash -cne $expected[$relative]) {
            throw "Update package checksum mismatch: $relative"
        }
    }
}

Test-PackageIntegrity
$packageMetadataPath = Join-Path $package 'PACKAGE.json'
$installerPath = Join-Path $package 'Install-NeuralPass.ps1'
if (-not (Test-Path -LiteralPath $packageMetadataPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw 'The update package is missing PACKAGE.json or Install-NeuralPass.ps1.'
}
$packageMetadata = Get-Content -LiteralPath $packageMetadataPath -Raw | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($TargetPath)) {
    $TargetPath = Read-Host 'Installed game directory'
}
$target = (Get-Item -LiteralPath $TargetPath -ErrorAction Stop).FullName
$installManifestPath = Join-Path $target 'NeuralPass.install.json'
if (-not (Test-Path -LiteralPath $installManifestPath -PathType Leaf)) {
    throw 'NeuralPass.install.json is missing; use the installer for a new installation.'
}
$oldManifest = Get-Content -LiteralPath $installManifestPath -Raw | ConvertFrom-Json
if ($oldManifest.format -ne 1 -or -not $oldManifest.files -or
    -not $oldManifest.file_hashes -or -not $oldManifest.package) {
    throw 'The existing installation manifest is invalid or too old to update safely.'
}
if ([string]$oldManifest.package.architecture -cne [string]$packageMetadata.architecture) {
    throw "Architecture mismatch: installed $($oldManifest.package.architecture), package $($packageMetadata.architecture)."
}

$targetPrefix = $target.TrimEnd([IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$oldFiles = [Collections.Generic.Dictionary[string,string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($relativeValue in $oldManifest.files) {
    $relative = [string]$relativeValue
    $candidate = [IO.Path]::GetFullPath((Join-Path $target $relative))
    if (-not $candidate.StartsWith($targetPrefix,
            [StringComparison]::OrdinalIgnoreCase) -or $oldFiles.ContainsKey($relative)) {
        throw "Unsafe or duplicate installed path: $relative"
    }
    $oldFiles.Add($relative, $candidate)
    $hashProperty = $oldManifest.file_hashes.PSObject.Properties[$relative]
    $isMissing = -not (Test-Path -LiteralPath $candidate -PathType Leaf)
    $isModified = $false
    if (-not $isMissing -and $hashProperty) {
        $isModified = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash `
            -cne [string]$hashProperty.Value
    }
    if (($isMissing -or -not $hashProperty -or $isModified) -and -not $ForceModified) {
        $reason = if ($isMissing) { 'missing' } elseif (-not $hashProperty) {
            'unhashed'
        } else { 'modified' }
        throw "Installed file is $reason; refusing update without -ForceModified: $relative"
    }
}

$blockedMarkers = @(
    'EasyAntiCheat', 'EasyAntiCheat_EOS', 'BattlEye', 'BEDaisy.sys',
    'vgk.sys', 'Riot Vanguard', 'FACEIT', 'ESEA'
)
$foundBlocked = @($blockedMarkers | Where-Object {
    Test-Path -LiteralPath (Join-Path $target $_)
})
if ($foundBlocked.Count) {
    throw "Refusing update: protected/anti-cheat marker(s) found: $($foundBlocked -join ', ')"
}

Write-Host ''
Write-Host 'NeuralPass is an unsigned research preview.' -ForegroundColor Yellow
Write-Host "Installed: $($oldManifest.package.version) $($oldManifest.package.source_revision)"
Write-Host "Package:   $($packageMetadata.version) $($packageMetadata.source_revision)"
Write-Host "Target:    $target"
$answer = if ([string]::IsNullOrWhiteSpace($Confirmation)) {
    Read-Host 'Type UPDATE to replace this manifest-owned NeuralPass installation'
} else { $Confirmation }
if ($answer -cne 'UPDATE') { throw 'Update cancelled.' }

# Preserve optional models only when the new package manifest still recognizes
# their filename and exact content hash. They are folded into the new ownership
# manifest after the package installer replaces the base payload.
$preservedModels = @()
$newModelManifestPath = Join-Path $package 'models/manifest.json'
if (Test-Path -LiteralPath $newModelManifestPath -PathType Leaf) {
    $newModelManifest = Get-Content -LiteralPath $newModelManifestPath -Raw | ConvertFrom-Json
    foreach ($model in $newModelManifest.models) {
        $relative = "models/downloads/$($model.filename)"
        $candidate = Join-Path $target $relative
        if ($oldFiles.ContainsKey($relative) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf) -and
            (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash -ieq
                [string]$model.sha256) {
            $preservedModels += [pscustomobject]@{
                Relative = $relative
                Hash = ([string]$model.sha256).ToUpperInvariant()
            }
        }
    }
}

$backup = Join-Path ([IO.Path]::GetTempPath()) ("NeuralPass-update-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $backup | Out-Null
$backupManifest = Join-Path $backup 'NeuralPass.install.json'
Copy-Item -LiteralPath $installManifestPath -Destination $backupManifest
foreach ($entry in $oldFiles.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) { continue }
    $destination = Join-Path $backup $entry.Key
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $entry.Value -Destination $destination
}

try {
    try {
        & $installerPath -TargetPath $target -ActivationPhrase 'INSTALL'

        $newManifest = Get-Content -LiteralPath $installManifestPath -Raw | ConvertFrom-Json
        if ($newManifest.format -ne 1 -or -not $newManifest.files -or
            -not $newManifest.file_hashes) {
            throw 'The updated installer produced an invalid ownership manifest.'
        }
        foreach ($model in $preservedModels) {
            if ($model.Relative -notin @($newManifest.files)) {
                $newManifest.files = @($newManifest.files) + $model.Relative
            }
            $newManifest.file_hashes | Add-Member -NotePropertyName $model.Relative `
                -NotePropertyValue $model.Hash -Force
        }
        $manifestTemporary = "$installManifestPath.$([Guid]::NewGuid().ToString('N')).tmp"
        try {
            $newManifest | ConvertTo-Json -Depth 12 |
                Set-Content -LiteralPath $manifestTemporary -Encoding UTF8
            Move-Item -LiteralPath $manifestTemporary -Destination $installManifestPath -Force
        } finally {
            Remove-Item -LiteralPath $manifestTemporary -Force -ErrorAction SilentlyContinue
        }

        $newOwned = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::OrdinalIgnoreCase)
        foreach ($relative in $newManifest.files) { [void]$newOwned.Add([string]$relative) }
        foreach ($entry in $oldFiles.GetEnumerator()) {
            if (-not $newOwned.Contains($entry.Key)) {
                Remove-Item -LiteralPath $entry.Value -Force -ErrorAction Stop
            }
        }
    } catch {
        $failure = $_
        $possiblyNew = @()
        if (Test-Path -LiteralPath $installManifestPath -PathType Leaf) {
            try {
                $partialManifest = Get-Content -LiteralPath $installManifestPath -Raw |
                    ConvertFrom-Json
                $possiblyNew = @($partialManifest.files)
            } catch { $possiblyNew = @() }
        }
        foreach ($relativeValue in $possiblyNew) {
            $relative = [string]$relativeValue
            if ($oldFiles.ContainsKey($relative)) { continue }
            $candidate = [IO.Path]::GetFullPath((Join-Path $target $relative))
            if ($candidate.StartsWith($targetPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                Remove-Item -LiteralPath $candidate -Force -ErrorAction SilentlyContinue
            }
        }
        foreach ($entry in $oldFiles.GetEnumerator()) {
            $source = Join-Path $backup $entry.Key
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                New-Item -ItemType Directory -Path (Split-Path -Parent $entry.Value) `
                    -Force | Out-Null
                Copy-Item -LiteralPath $source -Destination $entry.Value -Force
            } else {
                Remove-Item -LiteralPath $entry.Value -Force -ErrorAction SilentlyContinue
            }
        }
        Copy-Item -LiteralPath $backupManifest -Destination $installManifestPath -Force
        throw $failure
    }
} finally {
    Remove-Item -LiteralPath $backup -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'NeuralPass update completed; package integrity and ownership were refreshed.' `
    -ForegroundColor Green
