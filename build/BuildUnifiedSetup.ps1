[CmdletBinding()]
param(
 [string]$Staging = "$PSScriptRoot\..\bin\Release\x64",
 [Parameter(Mandatory)][string]$DriverPayload,
 [Parameter(Mandatory)][string]$Out,
 [string]$SignTool = "signtool.exe",
 [switch]$Sign,
 [switch]$VersionedOutput,
 [string]$CertName
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = (Resolve-Path "$PSScriptRoot\..").Path
Import-Module "$PSScriptRoot\ReleaseEvidence.psm1" -Force
$sourceBefore = Get-SourceEvidence $repo
$buildStartedUtc = [DateTime]::UtcNow.ToString('o')
$Staging = (Resolve-Path -LiteralPath $Staging).Path
$DriverPayload = (Resolve-Path -LiteralPath $DriverPayload).Path
$Out = [IO.Path]::GetFullPath($Out)
if (Test-Path -LiteralPath $Out) { throw 'Use a new output directory; existing recovery payloads are never overwritten.' }
$SignTool = (Get-Command $SignTool -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
if ($Sign -and [string]::IsNullOrWhiteSpace($CertName)) { throw 'Signing requires an operator-supplied certificate subject.' }
Import-Module "$PSScriptRoot\SignedDriver.psm1" -Force
$driverManifest = Read-DriverManifest "$PSScriptRoot\driver-payload.json"
Assert-SignedDriver $driverManifest $DriverPayload $SignTool
$version = ([xml](Get-Content "$repo\ProductVersion.props" -Raw)).Project.PropertyGroup.HidHideProductVersion
$wixVersion = (& wix --version).Trim()
if ($LASTEXITCODE -ne 0 -or $wixVersion.Split('+')[0] -ne '5.0.2') { throw 'This MSI requires verified WiX 5.0.2.' }
$applicationInputs = @('HidHideClient.exe','HidHideCLI.exe','Editor\HidHideProfiles.exe','Editor\resources\app.asar') | ForEach-Object {
 $inputPath = Join-Path $Staging $_
 [ordered]@{ path=$inputPath; sha256=(Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash; version=[Diagnostics.FileVersionInfo]::GetVersionInfo($inputPath).FileVersion }
}
function Run([string]$Command, [string[]]$Arguments) { & $Command @Arguments; if ($LASTEXITCODE -ne 0) { throw "$Command failed: $LASTEXITCODE" } }
function SignFile([string]$Path) {
 if (!$Sign) { return }
 Run $SignTool @('sign','/v','/n',$CertName,'/tr','http://timestamp.digicert.com','/fd','sha256','/td','sha256',$Path)
 Run $SignTool @('verify','/pa','/v',$Path)
}
# Work only on copies, including app-local Microsoft runtimes. Never re-sign driver files.
$payload = New-Item -ItemType Directory -Path "$Out\payload"
foreach($name in @('HidHideClient.exe','HidHideCLI.exe')) { Copy-Item -LiteralPath "$Staging\$name" -Destination $payload.FullName }
if (!(Test-Path -LiteralPath "$Staging\Editor\HidHideProfiles.exe")) { throw 'Missing independently packaged Electron editor. Run unified Ci to stage it.' }
Copy-Item -LiteralPath "$Staging\Editor" -Destination "$($payload.FullName)\Editor" -Recurse
$Staging = $payload.FullName
$vcRuntime = @(& "$repo\build\StageAppRuntime.ps1" -Staging $Staging)
foreach($name in @('HidHideClient.exe','HidHideCLI.exe')) { SignFile "$Staging\$name" }
SignFile "$Staging\Editor\HidHideProfiles.exe"
Run dotnet @('build',"$repo\Installer",'-c','Release')
if ($Sign) {
 foreach($name in @('HidHide.Installer.exe','HidHide.DriverSetup.exe')) { SignFile "$repo\Installer\bin\Release\net48\$name" }
}
$previousTool = [Environment]::GetEnvironmentVariable('HIDHIDE_SIGN_TOOL')
$previousCert = [Environment]::GetEnvironmentVariable('HIDHIDE_SIGN_CERT')
try {
 [Environment]::SetEnvironmentVariable('HIDHIDE_SIGN_TOOL',$(if($Sign){$SignTool}else{$null}))
 [Environment]::SetEnvironmentVariable('HIDHIDE_SIGN_CERT',$(if($Sign){$CertName}else{$null}))
 Run dotnet @('run','--no-build','--project',"$repo\Installer",'-c','Release','--','--unified','--staging',$Staging,'--driver-payload',$DriverPayload,'--out',"$Out\msi")
} finally {
 [Environment]::SetEnvironmentVariable('HIDHIDE_SIGN_TOOL',$previousTool)
 [Environment]::SetEnvironmentVariable('HIDHIDE_SIGN_CERT',$previousCert)
}
if ($Sign) { Run $SignTool @('verify','/pa','/v',"$Out\msi\HidHide.Profiles.msi") }
$finalName = 'HidHide.Profiles.msi'
if ($VersionedOutput) {
 $finalName = "HidHide_Profiles_$(([version]$version).ToString(3))_x64.msi"
}
Move-Item -LiteralPath "$Out\msi\HidHide.Profiles.msi" -Destination "$Out\$finalName"
# Refuse to label an artifact with sources changed by a concurrent edit/build.
# Prebuilt application inputs have their own hashes; this does not pretend that
# their supplied binaries were necessarily compiled from this source snapshot.
$sourceAfter = Get-SourceEvidence $repo
if ($sourceAfter.Digest -ne $sourceBefore.Digest) { throw 'Sources changed during packaging. Build again after edits finish; no release manifest was produced.' }
$manifest = [ordered]@{
 schemaVersion=1; version=$version; architecture='x64'; artifact=$finalName; signed=[bool]$Sign
 sourceCommit=$sourceBefore.Commit; dirty=$sourceBefore.Dirty; sourceStatus=$sourceBefore.Status; sourceFiles=$sourceBefore.Files; sourceDigest=$sourceBefore.Digest
 buildStartedUtc=$buildStartedUtc; buildFinishedUtc=[DateTime]::UtcNow.ToString('o'); wixVersion=$wixVersion
 applicationInputs=@($applicationInputs); applicationProvenance='Explicit prebuilt inputs; their hashes identify the binaries, not proof of compilation from this source snapshot.'
 artifactKind='Windows Installer package'; publicMsi=$true
 vcRuntime=$vcRuntime
 driver=$driverManifest
 files=@(Get-ChildItem -LiteralPath $Out -File -Recurse | ForEach-Object { @{ path=$_.FullName.Substring($Out.Length+1); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash } })
}
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath "$Out\release-manifest.json" -Encoding UTF8
Assert-SignedDriver $driverManifest $DriverPayload $SignTool
Write-Output "Built public MSI: $Out\$finalName (signed: $([bool]$Sign)). Packaging success does not establish lifecycle release readiness."
