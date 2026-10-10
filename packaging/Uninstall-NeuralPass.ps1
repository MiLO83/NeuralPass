[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$TargetPath,
    [switch]$RemoveCache,
    [switch]$ForceModified,
    [string]$Confirmation
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
$answer = if ([string]::IsNullOrWhiteSpace($Confirmation)) {
    Read-Host 'Type REMOVE to uninstall the manifest-listed NeuralPass files'
} else { $Confirmation }
if ($answer -cne 'REMOVE') {
    throw 'Uninstall cancelled.'
}
$targetPrefix = $target.TrimEnd([IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$resolved = @()
foreach ($relative in $manifest.files) {
    $candidate = [IO.Path]::GetFullPath((Join-Path $target ([string]$relative)))
    if (-not $candidate.StartsWith($targetPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe manifest path: $relative"
    }
    $resolved += [pscustomobject]@{ Relative = [string]$relative; Path = $candidate }
}
if ($manifest.file_hashes -and -not $ForceModified) {
    foreach ($entry in $resolved) {
        $expected = $manifest.file_hashes.PSObject.Properties[$entry.Relative].Value
        if ((Test-Path -LiteralPath $entry.Path -PathType Leaf) -and $expected) {
            $actual = (Get-FileHash -LiteralPath $entry.Path -Algorithm SHA256).Hash
            if ($actual -cne $expected) {
                throw "Installed file was modified; refusing to delete it without -ForceModified: $($entry.Relative)"
            }
        }
    }
}
foreach ($entry in $resolved) {
    Remove-Item -LiteralPath $entry.Path -Force -ErrorAction SilentlyContinue
}
Remove-Item -LiteralPath $manifestPath -Force
if ($RemoveCache) {
    Remove-Item -LiteralPath (Join-Path $target 'NeuralPassCache') -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host 'NeuralPass files listed by the install manifest were removed.' -ForegroundColor Green
