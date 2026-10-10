[CmdletBinding()]
param(
    [ValidateSet('cpu', 'directml')][string]$Provider = 'directml',
    [ValidateRange(0, 31)][int]$DeviceId = 0,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$runtimeRoot = if (Test-Path -LiteralPath (Join-Path $root 'PACKAGE.json') -PathType Leaf) {
    Join-Path $root 'runtime'
} else { Join-Path $root 'NeuralPass/runtime' }
$testExecutable = Join-Path $runtimeRoot 'NeuralPassWorkerHealthTest.exe'
$workerExecutable = Join-Path $runtimeRoot 'NeuralPassWorker.exe'
$model = Get-ChildItem -LiteralPath (Join-Path $root 'models/downloads') -Filter '*.onnx' -File |
    Sort-Object Name | Select-Object -First 1
foreach ($path in @($testExecutable, $workerExecutable)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required worker-health executable is missing: $path"
    }
}
if (-not $model) { throw 'No packaged ONNX model was found.' }

function Get-PeArchitecture([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $reader = [IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'invalid DOS signature' }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadUInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw 'invalid PE signature' }
        switch ($reader.ReadUInt16()) {
            0x014C { return 'X86' }
            0x8664 { return 'X64' }
            default { return 'Unknown' }
        }
    } finally { $stream.Dispose() }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    if (Test-Path -LiteralPath (Join-Path $root 'PACKAGE.json') -PathType Leaf) {
        $localData = [Environment]::GetFolderPath(
            [Environment+SpecialFolder]::LocalApplicationData)
        if ([string]::IsNullOrWhiteSpace($localData)) {
            throw 'Could not resolve LocalAppData for the evidence report.'
        }
        $OutputDirectory = Join-Path $localData 'NeuralPass/Evidence'
    } else {
        $OutputDirectory = $root
    }
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[void](New-Item -ItemType Directory -Path $OutputDirectory -Force)

$started = [DateTime]::UtcNow
$ErrorActionPreference = 'Continue'
$output = @(& $testExecutable $workerExecutable $model.FullName $Provider $DeviceId 2>&1 |
    ForEach-Object { "$_" })
$exitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
$evidence = [ordered]@{
    format = 1
    generated_utc = [DateTime]::UtcNow.ToString('o')
    duration_ms = [int]([DateTime]::UtcNow - $started).TotalMilliseconds
    provider = $Provider
    directml_device_id = $DeviceId
    model = $model.Name
    client_architecture = Get-PeArchitecture $testExecutable
    worker_architecture = Get-PeArchitecture $workerExecutable
    passed = ($exitCode -eq 0)
    exit_code = $exitCode
    validates_forced_restart = $true
    output = $output
}
$report = Join-Path $OutputDirectory 'NeuralPass-worker-health.json'
$evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $report -Encoding UTF8
$output | ForEach-Object { Write-Host $_ }
Write-Host "Evidence report: $report"
if ($exitCode -ne 0) { exit $exitCode }
