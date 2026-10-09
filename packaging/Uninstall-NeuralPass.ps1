[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$TargetPath,
    [switch]$RemoveCache
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($TargetPath)) {
    $TargetPath = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$target = (Get-Item -LiteralPath $TargetPath).FullName
$manifestPath = Join-Path $target 'NeuralPass.install.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw 'NeuralPass.install.json is missing; refusing to guess which files to remove.'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.format -ne 1 -or -not $manifest.files) {
    throw 'The install manifest is invalid; no files were removed.'
}
Write-Host "Target: $target"
if ((Read-Host 'Type REMOVE to uninstall the manifest-listed NeuralPass files') -cne 'REMOVE') {
    throw 'Uninstall cancelled.'
}
$targetPrefix = $target.TrimEnd([IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
foreach ($relative in $manifest.files) {
    $candidate = [IO.Path]::GetFullPath((Join-Path $target ([string]$relative)))
    if (-not $candidate.StartsWith($targetPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe manifest path: $relative"
    }
    Remove-Item -LiteralPath $candidate -Force -ErrorAction SilentlyContinue
}
Remove-Item -LiteralPath $manifestPath -Force
if ($RemoveCache) {
    Remove-Item -LiteralPath (Join-Path $target 'NeuralPassCache') -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host 'NeuralPass files listed by the install manifest were removed.' -ForegroundColor Green
