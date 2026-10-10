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
    'NeuralPassHardwareTest.exe', 'NeuralPass-hardware-0.json',
    'NeuralPass/PACKAGE.json', 'NeuralPass/SHA256SUMS.txt'
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
    Get-Content -LiteralPath $log -Tail 120 | ForEach-Object { Add-Line $_ }
} else {
    Add-Line '- ReShade.log not found'
}
$lines | Set-Content -LiteralPath $report -Encoding UTF8
Write-Host "Diagnostic report written to: $report" -ForegroundColor Green
