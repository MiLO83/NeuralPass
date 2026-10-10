[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$TargetPath,
    [string]$ActivationPhrase
)

$ErrorActionPreference = 'Stop'
$package = Split-Path -Parent $MyInvocation.MyCommand.Path
$packageMetadata = Get-Content -LiteralPath (Join-Path $package 'PACKAGE.json') -Raw | ConvertFrom-Json
$addonName = switch ($packageMetadata.architecture) {
    'windows-x64' { 'NeuralPass.addon64' }
    'windows-x86' { 'NeuralPass.addon32' }
    default { throw "Unsupported package architecture: $($packageMetadata.architecture)" }
}

function Test-PackageIntegrity {
    $checksumPath = Join-Path $package 'SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $checksumPath -PathType Leaf)) {
        throw 'SHA256SUMS.txt is missing; refusing an unverifiable package.'
    }
    $expected = [Collections.Generic.Dictionary[string,string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($line in Get-Content -LiteralPath $checksumPath) {
        if ($line -notmatch '^([0-9a-fA-F]{64}) \*(.+)$') {
            throw "Invalid checksum manifest line: $line"
        }
        $relative = $Matches[2].Replace('/', [IO.Path]::DirectorySeparatorChar)
        $candidate = [IO.Path]::GetFullPath((Join-Path $package $relative))
        $prefix = $package.TrimEnd([IO.Path]::DirectorySeparatorChar,
            [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
        if (-not $candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
            $expected.ContainsKey($relative)) {
            throw "Unsafe or duplicate checksum path: $relative"
        }
        $expected.Add($relative, $Matches[1].ToUpperInvariant())
    }
    $actual = @(Get-ChildItem -LiteralPath $package -File -Recurse | Where-Object {
        $_.FullName -ne $checksumPath
    })
    if ($actual.Count -ne $expected.Count) {
        throw 'Package file count does not match SHA256SUMS.txt.'
    }
    foreach ($item in $actual) {
        $relative = $item.FullName.Substring($package.Length + 1)
        if (-not $expected.ContainsKey($relative)) {
            throw "Package file is absent from SHA256SUMS.txt: $relative"
        }
        $actualHash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
        if ($actualHash -cne $expected[$relative]) {
            throw "Package checksum mismatch: $relative"
        }
    }
}

Test-PackageIntegrity

function Resolve-GameDirectory([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) {
        $Value = Read-Host 'Game executable or installation directory'
    }
    $item = Get-Item -LiteralPath $Value -ErrorAction Stop
    if (-not $item.PSIsContainer) { return $item.Directory.FullName }
    return $item.FullName
}

$target = Resolve-GameDirectory $TargetPath
$blockedMarkers = @(
    'EasyAntiCheat', 'EasyAntiCheat_EOS', 'BattlEye', 'BEDaisy.sys',
    'vgk.sys', 'Riot Vanguard', 'FACEIT', 'ESEA'
)
$foundBlocked = foreach ($marker in $blockedMarkers) {
    if (Test-Path -LiteralPath (Join-Path $target $marker)) { $marker }
}
if ($foundBlocked) {
    throw "Refusing installation: protected/anti-cheat marker(s) found: $($foundBlocked -join ', ')"
}

$proxyNames = @('dxgi.dll', 'd3d9.dll', 'd3d10.dll', 'd3d11.dll', 'd3d12.dll', 'opengl32.dll')
$hasProxy = $false
foreach ($name in $proxyNames) {
    if (Test-Path -LiteralPath (Join-Path $target $name)) { $hasProxy = $true; break }
}
if (-not $hasProxy) {
    throw 'No graphics proxy DLL was found. Install ReShade with full add-on support first.'
}

Write-Host ''
Write-Host 'NeuralPass is an unsigned research preview.' -ForegroundColor Yellow
Write-Host 'Use it only with an offline game. Do not use it with protected multiplayer software.'
Write-Host "Target: $target"
$confirmation = if ([string]::IsNullOrWhiteSpace($ActivationPhrase)) {
    Read-Host 'Type INSTALL to activate NeuralPass for this game'
} else { $ActivationPhrase }
if ($confirmation -cne 'INSTALL') {
    throw 'Installation cancelled.'
}

$entries = @(
    @{ Source = $addonName; Destination = $addonName },
    @{ Source = 'reshade-shaders/Shaders/NeuralPass.fx'; Destination = 'reshade-shaders/Shaders/NeuralPass.fx' },
    @{ Source = 'README.md'; Destination = 'NeuralPass/README.md' },
    @{ Source = 'LICENSE.txt'; Destination = 'NeuralPass/LICENSE.txt' },
    @{ Source = 'ARCHITECTURE.md'; Destination = 'NeuralPass/ARCHITECTURE.md' },
    @{ Source = 'COMPATIBILITY.md'; Destination = 'NeuralPass/COMPATIBILITY.md' },
    @{ Source = 'THIRD_PARTY_NOTICES.md'; Destination = 'NeuralPass/THIRD_PARTY_NOTICES.md' },
    @{ Source = 'PACKAGE.json'; Destination = 'NeuralPass/PACKAGE.json' },
    @{ Source = 'SBOM.spdx.json'; Destination = 'NeuralPass/SBOM.spdx.json' },
    @{ Source = 'SHA256SUMS.txt'; Destination = 'NeuralPass/SHA256SUMS.txt' },
    @{ Source = 'Install NeuralPass.cmd'; Destination = 'Install NeuralPass.cmd' },
    @{ Source = 'Uninstall NeuralPass.cmd'; Destination = 'Uninstall NeuralPass.cmd' },
    @{ Source = 'Diagnose NeuralPass.cmd'; Destination = 'Diagnose NeuralPass.cmd' },
    @{ Source = 'Install-NeuralPass.ps1'; Destination = 'Install-NeuralPass.ps1' },
    @{ Source = 'Uninstall-NeuralPass.ps1'; Destination = 'Uninstall-NeuralPass.ps1' },
    @{ Source = 'Diagnose-NeuralPass.ps1'; Destination = 'Diagnose-NeuralPass.ps1' }
)
foreach ($optional in @('onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'DirectML.dll')) {
    if (Test-Path -LiteralPath (Join-Path $package $optional)) {
        $entries += @{ Source = $optional; Destination = $optional }
    }
}
foreach ($runtimeFile in @('onnxruntime.dll', 'onnxruntime_providers_shared.dll',
                            'DirectML.dll', 'NeuralPassWorker.exe',
                            'NeuralPassHardwareTest.exe', 'NeuralPassWorkerHealthTest.exe')) {
    $source = "runtime/$runtimeFile"
    if (Test-Path -LiteralPath (Join-Path $package $source)) {
        $entries += @{ Source = $source; Destination = "NeuralPass/runtime/$runtimeFile" }
    }
}
foreach ($optional in @('Validate NeuralPass Hardware.cmd',
                         'Validate-NeuralPassHardware.ps1', 'NeuralPassVulkanTest.exe',
                         'Validate NeuralPass Vulkan.cmd', 'Validate-NeuralPassVulkan.ps1',
                         'Validate NeuralPass Worker.cmd', 'Validate-NeuralPassWorker.ps1')) {
    if (Test-Path -LiteralPath (Join-Path $package $optional)) {
        $entries += @{ Source = $optional; Destination = $optional }
    }
}
if (Test-Path -LiteralPath (Join-Path $package 'models')) {
    Get-ChildItem -LiteralPath (Join-Path $package 'models') -File -Recurse | ForEach-Object {
        $relative = $_.FullName.Substring($package.Length + 1).Replace('\', '/')
        $entries += @{ Source = $relative; Destination = $relative }
    }
}
if (Test-Path -LiteralPath (Join-Path $package 'third-party')) {
    Get-ChildItem -LiteralPath (Join-Path $package 'third-party') -File -Recurse | ForEach-Object {
        $relative = $_.FullName.Substring($package.Length + 1).Replace('\', '/')
        $entries += @{ Source = $relative; Destination = "NeuralPass/$relative" }
    }
}

$manifestPath = Join-Path $target 'NeuralPass.install.json'
$owned = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
    $previous = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($previous.format -ne 1 -or -not $previous.files) {
        throw 'An invalid NeuralPass.install.json exists; refusing to overwrite files.'
    }
    foreach ($relative in $previous.files) { [void]$owned.Add([string]$relative) }
}
foreach ($entry in $entries) {
    $destination = Join-Path $target $entry.Destination
    if ((Test-Path -LiteralPath $destination) -and -not $owned.Contains($entry.Destination)) {
        throw "Refusing to overwrite a file not owned by NeuralPass: $($entry.Destination)"
    }
}

$installed = @()
$installedHashes = [ordered]@{}
foreach ($entry in $entries) {
    $source = Join-Path $package $entry.Source
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
    $destination = Join-Path $target $entry.Destination
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
    $installed += $entry.Destination
    $installedHashes[$entry.Destination] =
        (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
}
$manifest = [ordered]@{
    format = 1
    installed_utc = [DateTime]::UtcNow.ToString('o')
    package = (Get-Content -LiteralPath (Join-Path $package 'PACKAGE.json') -Raw | ConvertFrom-Json)
    package_manifest_sha256 = (Get-FileHash -LiteralPath (Join-Path $package 'SHA256SUMS.txt') -Algorithm SHA256).Hash
    files = $installed
    file_hashes = $installedHashes
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host "Installed $($installed.Count) files." -ForegroundColor Green
Write-Host 'Enable "NeuralPass (keep last)" and keep it last in the ReShade technique order.'
Write-Host 'Run "Diagnose NeuralPass.cmd" in the game directory if startup fails.'
