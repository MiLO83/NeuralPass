[CmdletBinding()]
param([Parameter(Position = 0)][string]$TargetPath)

$ErrorActionPreference = 'Continue'
if ([string]::IsNullOrWhiteSpace($TargetPath)) {
    $TargetPath = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$target = (Get-Item -LiteralPath $TargetPath).FullName
$report = Join-Path $target 'NeuralPass-diagnostics.txt'
$lines = [Collections.Generic.List[string]]::new()
function Add-Line([string]$Value = '') { $lines.Add($Value) }

Add-Line 'NeuralPass diagnostics'
Add-Line "Generated UTC: $([DateTime]::UtcNow.ToString('o'))"
Add-Line "Target: $target"
Add-Line "Windows: $([Environment]::OSVersion.VersionString)"
Add-Line "Process architecture: $([Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture)"
Add-Line ''
Add-Line 'Package provenance:'
$packagePath = Join-Path $target 'NeuralPass/PACKAGE.json'
if (Test-Path -LiteralPath $packagePath -PathType Leaf) {
    try {
        $package = Get-Content -LiteralPath $packagePath -Raw | ConvertFrom-Json
        Add-Line "- version $($package.version); $($package.architecture); mode $($package.mode)"
        Add-Line "- revision $($package.source_revision); dirty $($package.source_dirty); signed $($package.signed)"
        Add-Line "- source timestamp $($package.created_utc)"
    } catch { Add-Line "! unreadable PACKAGE.json: $($_.Exception.Message)" }
} else { Add-Line '- PACKAGE.json not found' }
Add-Line ''
Add-Line 'Graphics API candidates (from proxy DLL names):'
$apiCandidates = [Collections.Generic.List[string]]::new()
if (Test-Path -LiteralPath (Join-Path $target 'd3d9.dll')) { $apiCandidates.Add('D3D9') }
if (Test-Path -LiteralPath (Join-Path $target 'd3d10.dll')) { $apiCandidates.Add('D3D10') }
if (Test-Path -LiteralPath (Join-Path $target 'd3d11.dll')) { $apiCandidates.Add('D3D11') }
if (Test-Path -LiteralPath (Join-Path $target 'd3d12.dll')) { $apiCandidates.Add('D3D12') }
if (Test-Path -LiteralPath (Join-Path $target 'dxgi.dll')) { $apiCandidates.Add('D3D10/D3D11/D3D12 via DXGI') }
if (Test-Path -LiteralPath (Join-Path $target 'opengl32.dll')) { $apiCandidates.Add('OpenGL') }
if ($apiCandidates.Count) { $apiCandidates | ForEach-Object { Add-Line "- $_" } }
else { Add-Line '- no recognized proxy at game root' }
Add-Line '- authoritative live API and HDR color space require ReShade runtime evidence'
Add-Line ''
Add-Line 'Display adapters:'
Get-CimInstance Win32_VideoController | ForEach-Object {
    Add-Line "- $($_.Name); driver $($_.DriverVersion); RAM $($_.AdapterRAM)"
}
Add-Line ''
Add-Line 'Relevant files:'
$names = @(
    'NeuralPass.addon64', 'NeuralPass.addon32', 'ReShade.ini', 'ReShade.log', 'dxgi.dll', 'd3d9.dll',
    'd3d10.dll', 'd3d11.dll', 'd3d12.dll', 'opengl32.dll', 'onnxruntime.dll',
    'onnxruntime_providers_shared.dll', 'DirectML.dll', 'NeuralPass.install.json',
    'NeuralPassWorker.exe', 'NeuralPassHardwareTest.exe', 'NeuralPass-hardware-0.json',
    'NeuralPass/PACKAGE.json', 'NeuralPass/SBOM.spdx.json', 'NeuralPass/SHA256SUMS.txt'
)
foreach ($name in $names) {
    $path = Join-Path $target $name
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $item = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        Add-Line "+ $name; $($item.Length) bytes; SHA256 $hash"
    } else {
        Add-Line "- $name"
    }
}
Add-Line ''
Add-Line 'Installed file integrity:'
$installManifestPath = Join-Path $target 'NeuralPass.install.json'
if (Test-Path -LiteralPath $installManifestPath -PathType Leaf) {
    try {
        $installManifest = Get-Content -LiteralPath $installManifestPath -Raw | ConvertFrom-Json
        $missingInstalled = 0
        $modifiedInstalled = 0
        foreach ($relative in $installManifest.files) {
            $installedPath = Join-Path $target ([string]$relative)
            if (-not (Test-Path -LiteralPath $installedPath -PathType Leaf)) {
                Add-Line "! missing: $relative"
                $missingInstalled++
                continue
            }
            $property = if ($installManifest.file_hashes) {
                $installManifest.file_hashes.PSObject.Properties[[string]$relative]
            } else { $null }
            if ($property) {
                $actualHash = (Get-FileHash -LiteralPath $installedPath -Algorithm SHA256).Hash
                if ($actualHash -cne $property.Value) {
                    Add-Line "! modified: $relative"
                    $modifiedInstalled++
                }
            }
        }
        if (-not $installManifest.file_hashes) { Add-Line '- legacy manifest has no installed hashes' }
        elseif ($missingInstalled -eq 0 -and $modifiedInstalled -eq 0) {
            Add-Line "PASS: $($installManifest.files.Count) manifest-owned files match"
        } else { Add-Line "FAIL: $missingInstalled missing; $modifiedInstalled modified" }
    } catch { Add-Line "! unreadable install manifest: $($_.Exception.Message)" }
} else { Add-Line '- not installed through the guided installer' }
Add-Line ''
Add-Line 'Latest hardware/provider evidence:'
$hardwareReport = Get-ChildItem -LiteralPath $target -Filter 'NeuralPass-hardware-*.json' -File |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if ($hardwareReport) {
    try {
        $hardware = Get-Content -LiteralPath $hardwareReport.FullName -Raw | ConvertFrom-Json
        Add-Line "- $($hardwareReport.Name): passed $($hardware.passed); DirectML device $($hardware.directml_device_id); model $($hardware.model)"
    } catch { Add-Line "! unreadable hardware report: $($_.Exception.Message)" }
} else { Add-Line '- no hardware validation report found' }
Add-Line ''
Add-Line 'Safety markers:'
$markers = @('EasyAntiCheat', 'EasyAntiCheat_EOS', 'BattlEye', 'BEDaisy.sys', 'vgk.sys', 'FACEIT', 'ESEA')
$anyMarker = $false
foreach ($marker in $markers) {
    if (Test-Path -LiteralPath (Join-Path $target $marker)) {
        Add-Line "! $marker"
        $anyMarker = $true
    }
}
if (-not $anyMarker) { Add-Line '- none found at the game root' }
Add-Line ''
Add-Line 'Recent ReShade log tail:'
$log = Join-Path $target 'ReShade.log'
if (Test-Path -LiteralPath $log) {
    $logTail = @(Get-Content -LiteralPath $log -Tail 120)
    $evidence = @($logTail | Where-Object { $_ -match 'NeuralPass|Direct3D|Vulkan|HDR|scRGB|color.?space|add-on' })
    Add-Line 'Detected runtime/API/HDR evidence:'
    if ($evidence.Count) { $evidence | Select-Object -Last 30 | ForEach-Object { Add-Line "> $_" } }
    else { Add-Line '- no matching runtime evidence in log tail' }
    Add-Line 'Raw tail:'
    $logTail | ForEach-Object { Add-Line $_ }
} else {
    Add-Line '- ReShade.log not found'
}
$lines | Set-Content -LiteralPath $report -Encoding UTF8
Write-Host "Diagnostic report written to: $report" -ForegroundColor Green
