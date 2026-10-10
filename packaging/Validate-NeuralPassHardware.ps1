[CmdletBinding()]
param([ValidateRange(0, 31)][int]$DeviceId = 0)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$executable = Join-Path $root 'NeuralPassHardwareTest.exe'
$model = Get-ChildItem -LiteralPath (Join-Path $root 'models/downloads') -Filter '*.onnx' -File |
    Sort-Object Name | Select-Object -First 1
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw 'NeuralPassHardwareTest.exe is missing. Use the x64 DirectML package.'
}
if (-not $model) { throw 'No packaged ONNX model was found.' }

$adapters = @(Get-CimInstance Win32_VideoController | ForEach-Object {
    [ordered]@{
        name = $_.Name
        pnp_device_id = $_.PNPDeviceID
        driver_version = $_.DriverVersion
        adapter_ram = [uint64]$_.AdapterRAM
    }
})
$started = [DateTime]::UtcNow
$ErrorActionPreference = 'Continue' # ONNX Runtime writes benign graph warnings to stderr.
$output = @(& $executable $model.FullName directml $DeviceId 2>&1 | ForEach-Object { "$_" })
$exitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
$evidence = [ordered]@{
    format = 1
    generated_utc = [DateTime]::UtcNow.ToString('o')
    duration_ms = [int]([DateTime]::UtcNow - $started).TotalMilliseconds
    directml_device_id = $DeviceId
    model = $model.Name
    passed = ($exitCode -eq 0)
    exit_code = $exitCode
    windows_display_adapters_unordered = $adapters
    output = $output
}
$report = Join-Path $root "NeuralPass-hardware-$DeviceId.json"
$evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $report -Encoding UTF8
$output | ForEach-Object { Write-Host $_ }
Write-Host "Evidence report: $report"
if ($exitCode -ne 0) { exit $exitCode }
