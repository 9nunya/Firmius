# Run on Windows: powershell -NoProfile -File scripts/test-install-ps1.ps1
# Real archive extraction and file replacement, with downloads served locally.
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This fixture requires Windows (user PATH and executable locking semantics).' }
$root = Split-Path $PSScriptRoot -Parent
$work = Join-Path $env:TEMP "firmius-installer-test-$([guid]::NewGuid())"
$oldPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$oldProcessPath = $env:Path
$oldInstallDir = $env:FIRMIUS_INSTALL_DIR
$oldRepo = $env:FIRMIUS_REPO
$oldVersion = $env:FIRMIUS_VERSION
try {
  New-Item -ItemType Directory -Force "$work/payload" | Out-Null
  $env:FIRMIUS_INSTALL_DIR = "$work/bin"
  $env:FIRMIUS_REPO = 'fixture/repo'
  $env:FIRMIUS_VERSION = 'v1.2.3'
  foreach ($name in @('firmius', 'firmiusd', 'firmius-desktop')) {
    Set-Content "$work/payload/$name.exe" "fixture-$name"
  }
  Compress-Archive "$work/payload/*" "$work/release.zip"
  $digest = (Get-FileHash "$work/release.zip" -Algorithm SHA256).Hash
  Set-Content "$work/SHA256SUMS" "$digest  firmius-x86_64-pc-windows-msvc.zip"
  function Invoke-WebRequest {
    param($Uri, $OutFile)
    if ($Uri.EndsWith('/SHA256SUMS')) { Copy-Item "$work/SHA256SUMS" $OutFile }
    else { Copy-Item "$work/release.zip" $OutFile }
  }
  & "$root/install.ps1"
  foreach ($name in @('firmius', 'firmiusd', 'firmius-desktop')) {
    if ((Get-Content "$work/bin/$name.exe") -ne "fixture-$name") { throw "$name was not installed" }
  }
  if (-not (Test-Path "$work/bin/firmius-install.json")) { throw 'Missing install marker' }
  # Test replacement with existing destinations as well as fresh installation.
  & "$root/install.ps1"
  # Old archives must not replace CLI or metadata.
  Remove-Item "$work/release.zip"
  Compress-Archive "$work/payload/firmius.exe" "$work/release.zip"
  $digest = (Get-FileHash "$work/release.zip" -Algorithm SHA256).Hash
  Set-Content "$work/SHA256SUMS" "$digest  firmius-x86_64-pc-windows-msvc.zip"
  Set-Content "$work/bin/firmius.exe" 'old-cli'
  $rejected = $false
  try { & "$root/install.ps1" } catch {
    if ($_.Exception.Message -notlike '*Older CLI-only releases are unsupported*') { throw }
    $rejected = $true
  }
  if (-not $rejected) { throw 'Legacy archive was accepted' }
  if ((Get-Content "$work/bin/firmius.exe") -ne 'old-cli') { throw 'Legacy install replaced CLI' }
  Write-Host 'install.ps1 fixture verification passed'
} finally {
  [Environment]::SetEnvironmentVariable('Path', $oldPath, 'User')
  $env:Path = $oldProcessPath
  $env:FIRMIUS_INSTALL_DIR = $oldInstallDir
  $env:FIRMIUS_REPO = $oldRepo
  $env:FIRMIUS_VERSION = $oldVersion
  Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}