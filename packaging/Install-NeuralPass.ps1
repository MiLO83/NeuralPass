[CmdletBinding()]
param([Parameter(Position = 0)][string]$TargetPath)

$ErrorActionPreference = 'Stop'
$package = Split-Path -Parent $MyInvocation.MyCommand.Path
$packageMetadata = Get-Content -LiteralPath (Join-Path $package 'PACKAGE.json') -Raw | ConvertFrom-Json
$addonName = switch ($packageMetadata.architecture) {
    'windows-x64' { 'NeuralPass.addon64' }
    'windows-x86' { 'NeuralPass.addon32' }
    default { throw "Unsupported package architecture: $($packageMetadata.architecture)" }
}

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
if ((Read-Host 'Type INSTALL to activate NeuralPass for this game') -cne 'INSTALL') {
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
foreach ($optional in @('NeuralPassHardwareTest.exe', 'Validate NeuralPass Hardware.cmd',
                         'Validate-NeuralPassHardware.ps1')) {
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
foreach ($entry in $entries) {
    $source = Join-Path $package $entry.Source
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
    $destination = Join-Path $target $entry.Destination
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
    $installed += $entry.Destination
}
$manifest = [ordered]@{
    format = 1
    installed_utc = [DateTime]::UtcNow.ToString('o')
    package = (Get-Content -LiteralPath (Join-Path $package 'PACKAGE.json') -Raw | ConvertFrom-Json)
    files = $installed
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host "Installed $($installed.Count) files." -ForegroundColor Green
Write-Host 'Enable "NeuralPass (keep last)" and keep it last in the ReShade technique order.'
Write-Host 'Run "Diagnose NeuralPass.cmd" in the game directory if startup fails.'
