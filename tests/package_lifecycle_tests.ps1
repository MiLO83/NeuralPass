[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Package)

$ErrorActionPreference = 'Stop'
$packageRoot = (Get-Item -LiteralPath $Package).FullName
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("NeuralPass-lifecycle-" + [Guid]::NewGuid())
$game = Join-Path $testRoot 'game'
try {
    New-Item -ItemType Directory -Path $game | Out-Null
    Set-Content -LiteralPath (Join-Path $game 'dxgi.dll') -Value 'test proxy marker'

    & (Join-Path $packageRoot 'Install-NeuralPass.ps1') `
        -TargetPath $game -ActivationPhrase 'INSTALL'
    $installManifestPath = Join-Path $game 'NeuralPass.install.json'
    if (-not (Test-Path -LiteralPath $installManifestPath -PathType Leaf)) {
        throw 'installer did not create its ownership manifest'
    }
    $manifest = Get-Content -LiteralPath $installManifestPath -Raw | ConvertFrom-Json
    if (-not $manifest.file_hashes -or $manifest.files.Count -lt 10) {
        throw 'install manifest does not contain per-file integrity evidence'
    }
    if ((Test-Path -LiteralPath (Join-Path $game 'NeuralPassWorker.exe')) -or
        -not (Test-Path -LiteralPath (
            Join-Path $game 'NeuralPass/runtime/NeuralPassWorker.exe') -PathType Leaf)) {
        throw 'worker executable was not isolated from game-root graphics proxies'
    }

    $modelManager = Join-Path $game 'Manage-NeuralPassModels.ps1'
    & $modelManager -Action Verify -Model candy
    $lastModelRefused = $false
    try {
        & $modelManager -Action Remove -Model candy
    } catch {
        $lastModelRefused = $_.Exception.Message -match 'last verified model'
    }
    if (-not $lastModelRefused) {
        throw 'model manager did not protect the last verified model'
    }
    $immutableRefused = $false
    try {
        & (Join-Path $packageRoot 'Manage-NeuralPassModels.ps1') -Action Install -Model mosaic
    } catch {
        $immutableRefused = $_.Exception.Message -match 'assembled package is immutable'
    }
    if (-not $immutableRefused) {
        throw 'model manager did not protect the assembled package from mutation'
    }

    $vulkanValidator = Join-Path $game 'Validate-NeuralPassVulkan.ps1'
    $vulkanProcess = Start-Process -FilePath 'powershell.exe' -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$vulkanValidator`"") `
        -Wait -PassThru -NoNewWindow
    if ($vulkanProcess.ExitCode -notin @(0, 77)) {
        throw "installed Vulkan validator failed with exit code $($vulkanProcess.ExitCode)"
    }
    $vulkanReportPath = Join-Path $game 'NeuralPass-vulkan-runtime.json'
    if (-not (Test-Path -LiteralPath $vulkanReportPath -PathType Leaf)) {
        throw 'installed Vulkan validator did not write evidence beside the game'
    }
    $vulkanReport = Get-Content -LiteralPath $vulkanReportPath -Raw | ConvertFrom-Json
    if (($vulkanProcess.ExitCode -eq 0 -and -not $vulkanReport.passed) -or
        ($vulkanProcess.ExitCode -eq 77 -and -not $vulkanReport.skipped)) {
        throw 'installed Vulkan evidence does not classify its exit status correctly'
    }

    $workerValidator = Join-Path $game 'Validate-NeuralPassWorker.ps1'
    $workerProcess = Start-Process -FilePath 'powershell.exe' -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$workerValidator`"",
        '-Provider', 'cpu') -Wait -PassThru -NoNewWindow
    if ($workerProcess.ExitCode -ne 0) {
        throw "installed worker-health validator failed with exit code $($workerProcess.ExitCode)"
    }
    $workerReportPath = Join-Path $game 'NeuralPass-worker-health.json'
    if (-not (Test-Path -LiteralPath $workerReportPath -PathType Leaf)) {
        throw 'installed worker-health validator did not write evidence beside the game'
    }
    $workerReport = Get-Content -LiteralPath $workerReportPath -Raw | ConvertFrom-Json
    $expectedClientArchitecture = if ($manifest.package.architecture -eq 'windows-x86') {
        'X86'
    } else { 'X64' }
    if (-not $workerReport.passed -or $workerReport.provider -ne 'cpu' -or
        -not $workerReport.validates_forced_restart -or
        $workerReport.client_architecture -ne $expectedClientArchitecture -or
        $workerReport.worker_architecture -ne 'X64') {
        throw 'installed worker-health evidence has invalid architecture or recovery data'
    }

    & (Join-Path $packageRoot 'Diagnose-NeuralPass.ps1') -TargetPath $game
    $diagnostics = Get-Content -LiteralPath (Join-Path $game 'NeuralPass-diagnostics.txt') -Raw
    if ($diagnostics -notmatch 'Package provenance:' -or
        $diagnostics -notmatch 'Installed file integrity:[\s\S]*PASS:' -or
        $diagnostics -notmatch 'Managed models:[\s\S]*candy: verified' -or
        $diagnostics -notmatch 'Vulkan runtime: passed' -or
        $diagnostics -notmatch 'Worker health: passed True') {
        throw 'diagnostics did not verify package provenance and installed files'
    }

    $tampered = Join-Path $game 'NeuralPass/README.md'
    Add-Content -LiteralPath $tampered -Value 'tamper test'
    $refused = $false
    try {
        & (Join-Path $packageRoot 'Uninstall-NeuralPass.ps1') `
            -TargetPath $game -Confirmation 'REMOVE'
    } catch {
        $refused = $_.Exception.Message -match 'modified; refusing to delete'
    }
    if (-not $refused -or -not (Test-Path -LiteralPath $installManifestPath)) {
        throw 'uninstaller did not safely refuse a modified installed file'
    }

    & (Join-Path $packageRoot 'Uninstall-NeuralPass.ps1') `
        -TargetPath $game -Confirmation 'REMOVE' -ForceModified
    if ((Test-Path -LiteralPath $installManifestPath) -or
        (Test-Path -LiteralPath (Join-Path $game 'NeuralPass.addon64')) -or
        (Test-Path -LiteralPath (Join-Path $game 'NeuralPass.addon32'))) {
        throw 'uninstaller left manifest-owned runtime files behind'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $game 'dxgi.dll'))) {
        throw 'uninstaller removed an unowned game file'
    }
    Write-Host 'Package install/diagnose/tamper/uninstall lifecycle passed.'
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
