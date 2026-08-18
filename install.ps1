# Firmius installer for Windows PowerShell
# irm https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.ps1 | iex

$ErrorActionPreference = 'Stop'
$Repo = if ($env:FIRMIUS_REPO) { $env:FIRMIUS_REPO } else { '9nunya/Firmius' }
$Version = if ($env:FIRMIUS_VERSION) { $env:FIRMIUS_VERSION } else { 'latest' }
$InstallDir = if ($env:FIRMIUS_INSTALL_DIR) { $env:FIRMIUS_INSTALL_DIR } else { Join-Path $HOME '.local\bin' }

if ([Environment]::Is64BitOperatingSystem -eq $false) {
  throw 'Firmius requires a 64-bit Windows installation.'
}
$base = if ($Version -eq 'latest') {
  "https://github.com/$Repo/releases/latest/download"
} else {
  if (-not $Version.StartsWith('v')) { $Version = "v$Version" }
  "https://github.com/$Repo/releases/download/$Version"
}
$archive = Join-Path $env:TEMP 'firmius.zip'
$unpacked = Join-Path $env:TEMP "firmius-$([guid]::NewGuid())"
New-Item -ItemType Directory -Force $unpacked | Out-Null

Write-Host "`n  ┌──────────────────────────────────────────┐" -ForegroundColor Cyan
Write-Host "  │              FIRMIUS INSTALLER           │" -ForegroundColor Cyan
Write-Host "  └──────────────────────────────────────────┘" -ForegroundColor Cyan
Write-Host "  Platform: x86_64-pc-windows-msvc"
Write-Host "  Destination: $(Join-Path $InstallDir 'firmius.exe')"
Write-Host '  Downloading release...'

try {
  Invoke-WebRequest -Uri "$base/firmius-x86_64-pc-windows-msvc.zip" -OutFile $archive
  Expand-Archive -Path $archive -DestinationPath $unpacked -Force
  $binary = Get-ChildItem $unpacked -Filter 'firmius.exe' -Recurse | Select-Object -First 1
  if (-not $binary) { throw 'The release archive did not contain firmius.exe.' }
  New-Item -ItemType Directory -Force $InstallDir | Out-Null
  Copy-Item $binary.FullName (Join-Path $InstallDir 'firmius.exe') -Force
} finally {
  Remove-Item $archive -Force -ErrorAction SilentlyContinue
  Remove-Item $unpacked -Recurse -Force -ErrorAction SilentlyContinue
}

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (($userPath -split ';') -notcontains $InstallDir) {
  [Environment]::SetEnvironmentVariable('Path', (($userPath, $InstallDir) -join ';'), 'User')
  Write-Host "  Added $InstallDir to your user PATH. Open a new terminal to use it."
}
Write-Host "`n  ✓ Firmius installed successfully. Run: firmius" -ForegroundColor Green
