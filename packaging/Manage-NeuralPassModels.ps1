[CmdletBinding()]
param(
    [ValidateSet('List', 'Verify', 'Install', 'Remove')]
    [string]$Action = 'List',
    [Parameter(Position = 0)]
    [string[]]$Model,
    [switch]$Repair
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$manifestPath = Join-Path $root 'models/manifest.json'
$downloads = Join-Path $root 'models/downloads'
$installManifestPath = Join-Path $root 'NeuralPass.install.json'
$packagePath = Join-Path $root 'PACKAGE.json'

if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw 'models/manifest.json is missing; model operations are unavailable.'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema_version -ne 1 -or -not $manifest.models) {
    throw 'The model manifest has an unsupported format.'
}

$entries = [Collections.Generic.Dictionary[string,object]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $manifest.models) {
    $id = [string]$entry.id
    $filename = [string]$entry.filename
    $hash = [string]$entry.sha256
    $uri = $null
    if ([string]::IsNullOrWhiteSpace($id) -or $entries.ContainsKey($id) -or
        $filename -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*\.onnx$' -or
        [IO.Path]::GetFileName($filename) -cne $filename -or
        $hash -notmatch '^[0-9a-fA-F]{64}$' -or
        -not [Uri]::TryCreate([string]$entry.url, [UriKind]::Absolute, [ref]$uri) -or
        $uri.Scheme -cne 'https' -or $uri.Host -notin @('github.com', 'raw.githubusercontent.com')) {
        throw "Unsafe or invalid model manifest entry: $id"
    }
    $entries.Add($id, $entry)
}

function Get-ModelState($Entry) {
    $path = Join-Path $downloads ([string]$Entry.filename)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return 'missing' }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($actual -ieq [string]$Entry.sha256) { return 'verified' }
    return 'corrupt'
}

function Resolve-Selection([switch]$InstalledOnly) {
    if ($Model -and $Model.Count) {
        $unknown = @($Model | Where-Object { -not $entries.ContainsKey($_) })
        if ($unknown.Count) { throw "Unknown model ID(s): $($unknown -join ', ')" }
        return @($Model | Select-Object -Unique | ForEach-Object { $entries[$_] })
    }
    $all = @($manifest.models)
    if ($InstalledOnly) {
        $all = @($all | Where-Object { (Get-ModelState $_) -ne 'missing' })
        if (-not $all.Count) { throw 'No installed model files were found.' }
    }
    return $all
}

function Assert-InstalledLayout {
    if (-not (Test-Path -LiteralPath $installManifestPath -PathType Leaf)) {
        if (Test-Path -LiteralPath $packagePath -PathType Leaf) {
            throw 'The assembled package is immutable. Install NeuralPass into a game before changing models.'
        }
        throw 'NeuralPass.install.json is missing; refusing to manage files without installer ownership.'
    }
}

function Read-InstallManifest {
    $value = Get-Content -LiteralPath $installManifestPath -Raw | ConvertFrom-Json
    if ($value.format -ne 1 -or -not $value.files -or -not $value.file_hashes) {
        throw 'NeuralPass.install.json does not contain supported file ownership data.'
    }
    return $value
}

function Write-InstallManifest($Value) {
    $temporary = "$installManifestPath.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporary -Encoding UTF8
        Move-Item -LiteralPath $temporary -Destination $installManifestPath -Force
    } finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

if ($Action -eq 'List') {
    foreach ($entry in $manifest.models) {
        $state = Get-ModelState $entry
        Write-Host ("{0,-16} {1,-9} {2} ({3})" -f $entry.id, $state,
            $entry.display_name, $entry.license)
    }
    return
}

if ($Action -eq 'Verify') {
    $selected = @(Resolve-Selection -InstalledOnly:(-not $Model))
    $failed = @()
    foreach ($entry in $selected) {
        $state = Get-ModelState $entry
        Write-Host "$($entry.id): $state"
        if ($state -ne 'verified') { $failed += [string]$entry.id }
    }
    if ($failed.Count) { throw "Model verification failed: $($failed -join ', ')" }
    Write-Host "Verified $($selected.Count) model(s)." -ForegroundColor Green
    return
}

Assert-InstalledLayout
$selected = @(Resolve-Selection)
if (-not $Model -or -not $Model.Count) {
    throw "Action '$Action' requires at least one model ID."
}

if ($Action -eq 'Install') {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    New-Item -ItemType Directory -Path $downloads -Force | Out-Null
    $ownership = Read-InstallManifest
    foreach ($entry in $selected) {
        $state = Get-ModelState $entry
        if ($state -eq 'verified') {
            Write-Host "$($entry.id): already verified"
            continue
        }
        if ($state -eq 'corrupt' -and -not $Repair) {
            throw "$($entry.id) is corrupt; rerun with -Repair to replace it."
        }
        $target = Join-Path $downloads ([string]$entry.filename)
        $partial = "$target.$([Guid]::NewGuid().ToString('N')).partial"
        try {
            Write-Host "Downloading $($entry.display_name) from its pinned manifest URL..."
            Invoke-WebRequest -Uri ([string]$entry.url) -OutFile $partial -UseBasicParsing
            $item = Get-Item -LiteralPath $partial
            if ($item.Length -le 0 -or $item.Length -gt 1GB) {
                throw "Downloaded model has an invalid size: $($item.Length) bytes"
            }
            $actual = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash
            if ($actual -ine [string]$entry.sha256) {
                throw "Downloaded model checksum mismatch for $($entry.id): $actual"
            }
            Move-Item -LiteralPath $partial -Destination $target -Force
        } finally {
            Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
        }
        $relative = "models/downloads/$($entry.filename)"
        if ($relative -notin @($ownership.files)) {
            $ownership.files = @($ownership.files) + $relative
        }
        $ownership.file_hashes | Add-Member -NotePropertyName $relative `
            -NotePropertyValue ([string]$entry.sha256).ToUpperInvariant() -Force
        Write-Host "$($entry.id): installed and verified" -ForegroundColor Green
    }
    Write-InstallManifest $ownership
    return
}

if ($Action -eq 'Remove') {
    $removing = @($selected | ForEach-Object { [string]$_.id })
    $remaining = @($manifest.models | Where-Object {
        ([string]$_.id -notin $removing) -and (Get-ModelState $_) -eq 'verified'
    })
    if (-not $remaining.Count) {
        throw 'Refusing to remove the last verified model.'
    }
    $ownership = Read-InstallManifest
    foreach ($entry in $selected) {
        $relative = "models/downloads/$($entry.filename)"
        $path = Join-Path $root $relative
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        $ownership.files = @($ownership.files | Where-Object { [string]$_ -ine $relative })
        $ownership.file_hashes.PSObject.Properties.Remove($relative)
        Write-Host "$($entry.id): removed"
    }
    Write-InstallManifest $ownership
}
