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

    # Simulate a file owned by an older release, prove modified installs are
    # rejected before mutation, then prove a verified update removes stale
    # ownership while replacing the package atomically.
    $staleRelative = 'NeuralPass/obsolete-preview.txt'
    $stalePath = Join-Path $game $staleRelative
    Set-Content -LiteralPath $stalePath -Value 'old release payload'
    $manifest = Get-Content -LiteralPath $installManifestPath -Raw | ConvertFrom-Json
    $manifest.files = @($manifest.files) + $staleRelative
    $manifest.file_hashes | Add-Member -NotePropertyName $staleRelative `
        -NotePropertyValue (Get-FileHash -LiteralPath $stalePath -Algorithm SHA256).Hash
    $manifest | ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $installManifestPath -Encoding UTF8

    $installedReadme = Join-Path $game 'NeuralPass/README.md'
    Add-Content -LiteralPath $installedReadme -Value 'update refusal probe'
    $modifiedRefused = $false
    try {
        & (Join-Path $packageRoot 'Update-NeuralPass.ps1') `
            -TargetPath $game -Confirmation 'UPDATE'
    } catch {
        $modifiedRefused = $_.Exception.Message -match 'modified; refusing update'
    }
    if (-not $modifiedRefused -or -not (Test-Path -LiteralPath $stalePath)) {
        throw 'updater did not refuse a modified install before changing files'
    }
    Copy-Item -LiteralPath (Join-Path $packageRoot 'README.md') `
        -Destination $installedReadme -Force
    & (Join-Path $packageRoot 'Update-NeuralPass.ps1') `
        -TargetPath $game -Confirmation 'UPDATE'
    $manifest = Get-Content -LiteralPath $installManifestPath -Raw | ConvertFrom-Json
    $packageMetadata = Get-Content -LiteralPath (Join-Path $packageRoot 'PACKAGE.json') `
        -Raw | ConvertFrom-Json
    if ((Test-Path -LiteralPath $stalePath) -or
        $staleRelative -in @($manifest.files) -or
        $manifest.package.source_revision -cne $packageMetadata.source_revision) {
        throw 'updater did not remove stale ownership or refresh package provenance'
    }

    # Force the nested installer to fail after overwriting earlier entries: make
    # the installed update launcher appear unowned, while deliberately modifying
    # README under -ForceModified. Rollback must recover that exact README and the
    # pre-update manifest rather than leaving a half-updated installation.
    $collisionRelative = 'Update NeuralPass.cmd'
    $manifest.files = @($manifest.files | Where-Object {
        [string]$_ -ine $collisionRelative
    })
    $manifest.file_hashes.PSObject.Properties.Remove($collisionRelative)
    $manifest | ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $installManifestPath -Encoding UTF8
    $manifestBeforeFailure = [IO.File]::ReadAllBytes($installManifestPath)
    Add-Content -LiteralPath $installedReadme -Value 'rollback byte sentinel'
    $rollbackTriggered = $false
    try {
        & (Join-Path $packageRoot 'Update-NeuralPass.ps1') `
            -TargetPath $game -Confirmation 'UPDATE' -ForceModified
    } catch {
        $rollbackTriggered = $_.Exception.Message -match 'not owned by NeuralPass'
    }
    $manifestAfterFailure = [IO.File]::ReadAllBytes($installManifestPath)
    $readmeAfterFailure = Get-Content -LiteralPath $installedReadme -Raw
    if (-not $rollbackTriggered -or
        [Convert]::ToBase64String($manifestBeforeFailure) -cne
            [Convert]::ToBase64String($manifestAfterFailure) -or
        $readmeAfterFailure -notmatch 'rollback byte sentinel') {
        throw 'updater did not restore exact owned files and manifest after replacement failure'
    }
    Copy-Item -LiteralPath (Join-Path $packageRoot 'README.md') `
        -Destination $installedReadme -Force
    $manifest = Get-Content -LiteralPath $installManifestPath -Raw | ConvertFrom-Json
    $manifest.files = @($manifest.files) + $collisionRelative
    $collisionPath = Join-Path $game $collisionRelative
    $manifest.file_hashes | Add-Member -NotePropertyName $collisionRelative `
        -NotePropertyValue (Get-FileHash -LiteralPath $collisionPath -Algorithm SHA256).Hash
    $manifest | ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $installManifestPath -Encoding UTF8

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

    # Diagnostics must prefer structured evidence emitted by the live add-on over
    # proxy-name guesses. The add-on writer itself is compiled in both architectures;
    # this fixture verifies the packaged consumer and schema contract.
    [ordered]@{
        schema_version = 2
        generated_utc = '2026-01-02T03:04:05.006Z'
        process_architecture = $expectedClientArchitecture
        graphics_api = 'd3d12'
        graphics_api_value = 49152
        width = 3840
        height = 2160
        backbuffer_format = 'r10g10b10a2_unorm'
        backbuffer_format_value = 24
        swapchain_color_space = 'hdr10_pq'
        swapchain_color_space_value = 3
        display_capture_supported = $true
        display_encoding = 'hdr10_pq'
        hdr_path = $true
        classification = 'compatible_format_color_space'
        effect_frames = 900
        draws_seen = 800
        uv_draws_seen = 700
        material_draws_seen = 600
        surface_frames_captured = 500
        surface_pixels_captured = 400
        inference_submitted = 300
        inference_completed = 200
        inference_dropped = 100
    } | ConvertTo-Json | Set-Content -LiteralPath (
        Join-Path $game 'NeuralPass-runtime-evidence.json') -Encoding UTF8

    & (Join-Path $packageRoot 'Diagnose-NeuralPass.ps1') -TargetPath $game
    $diagnostics = Get-Content -LiteralPath (Join-Path $game 'NeuralPass-diagnostics.txt') -Raw
    if ($diagnostics -notmatch 'Package provenance:' -or
        $diagnostics -notmatch 'Installed file integrity:[\s\S]*PASS:' -or
        $diagnostics -notmatch 'Managed models:[\s\S]*candy: verified' -or
        $diagnostics -notmatch 'Authoritative live runtime evidence:[\s\S]*API d3d12' -or
        $diagnostics -notmatch 'encoding hdr10_pq; HDR path True' -or
        $diagnostics -notmatch 'live counters: effects 900; draws 800; UV draws 700; material draws 600' -or
        $diagnostics -notmatch 'capture: frames 500; supported pixels 400; inference submitted/completed/dropped 300/200/100' -or
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
