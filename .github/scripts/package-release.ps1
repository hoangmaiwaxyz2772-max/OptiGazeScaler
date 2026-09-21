param(
    [Parameter(Mandatory = $true)][ValidatePattern('^v[0-9.]+-pre[0-9]+-dlss5-hotfix[0-9]+$')][string]$Tag,
    [string]$BuildDirectory = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path (Split-Path $PSScriptRoot)
Set-Location $repo
$package = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $repo 'x64/Release/a' }
$output = Join-Path $repo 'release-assets'
if (Test-Path $output) { throw 'Release output directory must be new' }
New-Item -ItemType Directory -Path $output | Out-Null

# Add only tracked documentation and bridge sources, never local experiments.
$extras = @(git ls-files -- README.md Config.md Features.md Changelog.md docs/DLSS5.md docs/GazeROI.md docs/GazeRoiExternalInput.md tools/gaze_roi_bridge tools/gaze_roi_shared_bridge)
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate package sources' }
foreach ($relative in $extras) {
    $destination = Join-Path $package $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo $relative) -Destination $destination
}
$required = @(
    'OptiScaler.dll', 'OptiScaler.ini', 'setup_windows.bat', 'setup_linux.sh',
    'LICENSE', 'CREDITS.md', 'README.md', 'Config.md', 'Features.md', 'Changelog.md',
    'docs/DLSS5.md', 'docs/GazeROI.md', 'docs/GazeRoiExternalInput.md',
    'Licenses/DirectX_LICENSE.txt', 'Licenses/FidelityFX_v1_LICENSE.md',
    'Licenses/FidelityFX_v2_LICENSE.md', 'Licenses/XeSS_LICENSE.txt',
    'Licenses/RenoDX_ATTRIBUTION.txt', 'Licenses/OptiScaler_DLSSNR_ATTRIBUTION.txt',
    'OptiScaler/D3D12_OptiScaler/D3D12Core.dll', 'OptiScaler/libxell.dll',
    'OptiScaler/libxess.dll', 'OptiScaler/libxess_dx11.dll', 'OptiScaler/libxess_fg.dll',
    'OptiScaler/amd_fidelityfx_vk.dll', 'OptiScaler/amd_fidelityfx_loader_dx12.dll',
    'OptiScaler/amd_fidelityfx_upscaler_dx12.dll', 'OptiScaler/amd_fidelityfx_framegeneration_dx12.dll',
    'tools/gaze_roi_bridge/bridge.mjs', 'tools/gaze_roi_bridge/package.json',
    'tools/gaze_roi_bridge/package-lock.json', 'tools/gaze_roi_shared_bridge/Program.cs',
    'tools/gaze_roi_shared_bridge/gaze_roi_shared_bridge.csproj',
    'tools/gaze_roi_shared_bridge/start-shared-bridge.bat'
)
foreach ($relative in $required) {
    $path = Join-Path $package $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
        throw "Missing or empty package file: $relative"
    }
}
$dll = Join-Path $package 'OptiScaler.dll'
$bytes = [IO.File]::ReadAllBytes($dll)
$pe = [BitConverter]::ToInt32($bytes, 0x3c)
if ($bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a -or [BitConverter]::ToUInt16($bytes, $pe+4) -ne 0x8664) {
    throw 'Expected an x64 PE DLL'
}
$bad = @(Get-ChildItem -LiteralPath $package -Recurse -File | Where-Object {
    $_.Name -match 'CpuDiag|^nvngx.*dlssnr.*\.dll$' -or $_.Extension -in '.pdb', '.log'
})
if ($bad.Count) { throw "Unexpected package files: $($bad.Name -join ', ')" }
$dllName = "OptiGazeScaler-$Tag.dll"
$zipName = "OptiGazeScaler-$Tag-win-x64.zip"
Copy-Item -LiteralPath $dll -Destination (Join-Path $output $dllName)
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($package, (Join-Path $output $zipName))

# Read every ZIP entry back and compare its digest with the staged file.
$archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $output $zipName))
try {
    $entries = @($archive.Entries | Where-Object { $_.Name })
    if ($entries.Count -ne @(Get-ChildItem $package -Recurse -File -Force).Count) { throw 'ZIP file count mismatch' }
    foreach ($entry in $entries) {
        $stream = $entry.Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
        finally { $stream.Dispose(); $sha.Dispose() }
        if ($hash -ne (Get-FileHash -LiteralPath (Join-Path $package $entry.FullName)).Hash) {
            throw "ZIP checksum mismatch: $($entry.FullName)"
        }
    }
} finally { $archive.Dispose() }
$checksums = foreach ($name in @($zipName, $dllName)) {
    '{0}  {1}' -f (Get-FileHash (Join-Path $output $name)).Hash.ToLowerInvariant(), $name
}
$checksums | Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ascii
Get-ChildItem $output | Select-Object Name, Length
$checksums
