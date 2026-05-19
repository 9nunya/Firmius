#requires -version 5.0
<#
.SYNOPSIS
  Firmius installer for Windows (PowerShell).

.DESCRIPTION
  Installs firmius.exe + bundled runtime DLLs to the install prefix and seeds
  per-user data (prompts, workflows, hinting, themes) into %APPDATA%\Firmius.

  Default install prefix:
    Per-user (no admin needed):  $env:LOCALAPPDATA\Programs\Firmius
    System-wide (with admin):    $env:ProgramFiles\Firmius

.PARAMETER Prefix
  Override the install prefix.

.PARAMETER System
  Install system-wide to $env:ProgramFiles\Firmius. Requires admin.

.PARAMETER AddToPath
  Add the install prefix to the user's PATH.

.PARAMETER Uninstall
  Remove the installed binaries.

.EXAMPLE
  .\install.ps1
  .\install.ps1 -System
  .\install.ps1 -Prefix C:\Tools\Firmius -AddToPath
  .\install.ps1 -Uninstall
#>

[CmdletBinding()]
param(
  [string]$Prefix,
  [switch]$System,
  [switch]$AddToPath,
  [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$UserData  = Join-Path $env:APPDATA 'Firmius'

if (-not $Prefix) {
  if ($System) {
    $Prefix = Join-Path $env:ProgramFiles 'Firmius'
  } else {
    $Prefix = Join-Path $env:LOCALAPPDATA 'Programs\Firmius'
  }
}

function Test-IsAdmin {
  $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = New-Object System.Security.Principal.WindowsPrincipal($id)
  return $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

if ($Uninstall) {
  Write-Host "==> Uninstalling firmius from $Prefix"
  Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $Prefix 'firmius.exe')
  Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $Prefix 'firmiusd.exe')
  Get-ChildItem -Path $Prefix -Filter '*.dll' -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
  Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $Prefix 'share\firmius')
  if (Test-Path $Prefix) {
    try { Remove-Item -Force $Prefix -ErrorAction Stop } catch { }
  }
  Write-Host "    User data at $UserData was NOT removed (delete manually if desired)."
  Write-Host '==> Done.'
  return
}

$srcExe = Join-Path $ScriptDir 'bin\firmius.exe'
if (-not (Test-Path $srcExe)) {
  Write-Error "firmius.exe not found at $srcExe"
  exit 1
}

if ($System -and -not (Test-IsAdmin)) {
  Write-Error 'Installing system-wide requires Administrator. Re-run from an elevated PowerShell, or omit -System for a per-user install.'
  exit 1
}

Write-Host '==> Installing firmius'
Write-Host "    prefix:    $Prefix"
Write-Host "    user data: $UserData"

New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Prefix 'share\firmius') | Out-Null

# Copy binaries and any bundled runtime DLLs.
Get-ChildItem -Path (Join-Path $ScriptDir 'bin') -Filter '*.exe' -ErrorAction SilentlyContinue |
  ForEach-Object { Copy-Item -Force $_.FullName $Prefix }
Get-ChildItem -Path (Join-Path $ScriptDir 'bin') -Filter '*.dll' -ErrorAction SilentlyContinue |
  ForEach-Object { Copy-Item -Force $_.FullName $Prefix }

# Bundled themes/share data.
$srcShare = Join-Path $ScriptDir 'share\firmius'
if (Test-Path $srcShare) {
  Copy-Item -Recurse -Force "$srcShare\*" (Join-Path $Prefix 'share\firmius\')
}

# Per-user assets — seed only if absent.
New-Item -ItemType Directory -Force -Path $UserData | Out-Null
foreach ($sub in @('prompts','workflows','hinting','themes')) {
  $src = Join-Path $ScriptDir $sub
  $dst = Join-Path $UserData $sub
  if ((Test-Path $src) -and -not (Test-Path $dst)) {
    Copy-Item -Recurse -Force $src $dst
    Write-Host "    seeded $dst"
  }
}

if ($AddToPath) {
  $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
  if ($userPath -notlike "*$Prefix*") {
    [Environment]::SetEnvironmentVariable('Path', "$userPath;$Prefix", 'User')
    Write-Host "    added $Prefix to user PATH (open a new shell to pick it up)"
  } else {
    Write-Host "    $Prefix already in user PATH"
  }
}

Write-Host ''
Write-Host '==> Done.'
if (-not $AddToPath) {
  Write-Host "    To add firmius to your PATH (current user, no admin):"
  Write-Host "      [Environment]::SetEnvironmentVariable('Path', `"`$env:Path;$Prefix`", 'User')"
  Write-Host '    Or re-run with -AddToPath.'
}
Write-Host "    Run: firmius"
