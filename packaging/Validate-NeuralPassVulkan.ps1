[CmdletBinding()]
param([string]$OutputDirectory)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$executable = Join-Path $root 'NeuralPassVulkanTest.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw 'NeuralPassVulkanTest.exe is missing.'
}

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

$adapters = @(Get-CimInstance Win32_VideoController | ForEach-Object {
    [ordered]@{
        name = $_.Name
        pnp_device_id = $_.PNPDeviceID
        driver_version = $_.DriverVersion
        adapter_ram = [uint64]$_.AdapterRAM
    }
})
$started = [DateTime]::UtcNow
$ErrorActionPreference = 'Continue'
$output = @(& $executable 2>&1 | ForEach-Object { "$_" })
$exitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
$evidence = [ordered]@{
    format = 1
    generated_utc = [DateTime]::UtcNow.ToString('o')
    duration_ms = [int]([DateTime]::UtcNow - $started).TotalMilliseconds
    executable_architecture = Get-PeArchitecture $executable
    runner_process_architecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
    passed = ($exitCode -eq 0)
    skipped = ($exitCode -eq 77)
    exit_code = $exitCode
    windows_display_adapters_unordered = $adapters
    output = $output
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
        # Installed validators live beside the game and diagnostics consume the
        # report from there. An assembled package stays immutable after validation.
        $OutputDirectory = $root
    }
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[void](New-Item -ItemType Directory -Path $OutputDirectory -Force)
$report = Join-Path $OutputDirectory 'NeuralPass-vulkan-runtime.json'
$evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $report -Encoding UTF8
$output | ForEach-Object { Write-Host $_ }
Write-Host "Evidence report: $report"
if ($exitCode -ne 0) { exit $exitCode }
