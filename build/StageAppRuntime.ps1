[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Staging,
    [string]$VisualStudio,
    [string]$RedistVersion
)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($VisualStudio)) {
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $locator -PathType Leaf)) { throw 'Specify -VisualStudio or install the Visual Studio locator.' }
    $VisualStudio = & $locator -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($VisualStudio)) { throw 'Visual Studio C++ runtime source not found.' }
    $VisualStudio = $VisualStudio.Trim()
}
function Read-DefaultVersion([string]$Name) {
    $path = Join-Path $VisualStudio ('VC\Auxiliary\Build\' + $Name)
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Visual Studio toolset version file not found: $path" }
    $value = (Get-Content -LiteralPath $path -Raw).Trim()
    if ($value -notmatch '^\d+\.\d+\.\d+$') { throw "Unrecognized version in ${path}: $value" }
    $value
}
# App-local redistribution from the licensed Visual Studio Redist directory.
# Windows 11 supplies UCRT. The redist set is the installed toolset's default
# (the one MSBuild uses), never older than the default compiler toolset. Inspect
# both ordinary and delay imports before changing the runtime set.
if ([string]::IsNullOrWhiteSpace($RedistVersion)) { $RedistVersion = Read-DefaultVersion 'Microsoft.VCRedistVersion.default.txt' }
$toolsVersion = Read-DefaultVersion 'Microsoft.VCToolsVersion.default.txt'
if ([version]$RedistVersion -lt [version]$toolsVersion) { throw "VC++ redist $RedistVersion is older than compiler toolset $toolsVersion; update the Visual Studio C++ redistributable component." }
$redist = Join-Path $VisualStudio ('VC\Redist\MSVC\' + $RedistVersion + '\x64')
$crt = Get-ChildItem -LiteralPath $redist -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue
$mfc = Get-ChildItem -LiteralPath $redist -Directory -Filter 'Microsoft.VC*.MFC' -ErrorAction SilentlyContinue
if (@($crt).Count -ne 1 -or @($mfc).Count -ne 1) { throw "Expected exactly one x64 CRT and one MFC redist directory under $redist. Install the Visual Studio C++ MFC component for this toolset." }
$files = @(
    @{ Name='msvcp140.dll'; Directory=$crt.FullName },
    @{ Name='vcruntime140.dll'; Directory=$crt.FullName },
    @{ Name='vcruntime140_1.dll'; Directory=$crt.FullName },
    @{ Name='mfc140u.dll'; Directory=$mfc.FullName }
)
$destination = (Resolve-Path -LiteralPath $Staging).Path
foreach ($entry in $files) {
    $source = Join-Path $entry.Directory $entry.Name
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Runtime not found: $source" }
    $signature = Get-AuthenticodeSignature -LiteralPath $source
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation(?:,|$)') { throw "Runtime signature did not verify: $source" }
    $bytes = [IO.File]::ReadAllBytes($source)
    $header = [BitConverter]::ToInt32($bytes, 60)
    if ($header -lt 64 -or $header + 6 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes,$header) -ne 0x4550 -or [BitConverter]::ToUInt16($bytes,$header+4) -ne 0x8664) { throw "Runtime is not an AMD64 PE file: $source" }
    # Hash the verified bytes; the staged copy must match them exactly.
    $entry.Hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
    $entry.Signer = $signature.SignerCertificate.Subject
    $entry.Version = [Diagnostics.FileVersionInfo]::GetVersionInfo($source).FileVersion
}
# Verify the complete source set before changing staging.
foreach ($entry in $files) {
    $source = Join-Path $entry.Directory $entry.Name
    $target = Join-Path $destination $entry.Name
    Copy-Item -LiteralPath $source -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $entry.Hash) { throw "Staged runtime changed: $target" }
    Write-Host ($entry.Name + ' ' + $entry.Version + ' SHA256 ' + $entry.Hash)
    [ordered]@{ name=$entry.Name; redistVersion=$RedistVersion; source=$source; fileVersion=$entry.Version; sha256=$entry.Hash; signer=$entry.Signer }
}
