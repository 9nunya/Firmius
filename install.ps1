# Firmius installer for Windows PowerShell
# irm https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.ps1 | iex

$ErrorActionPreference = 'Stop'
$Repo = if ($env:FIRMIUS_REPO) { $env:FIRMIUS_REPO } else { '9nunya/Firmius' }
$Version = if ($env:FIRMIUS_VERSION) { $env:FIRMIUS_VERSION } else { 'latest' }
$InstallDir = if ($env:FIRMIUS_INSTALL_DIR) { $env:FIRMIUS_INSTALL_DIR } else { Join-Path $HOME '.local\bin' }

if ($Repo -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
  throw 'FIRMIUS_REPO must be a safe owner/repository name.'
}
if (($Version -ne 'latest') -and ($Version -notmatch '^v?[0-9]+(?:\.[0-9]+)*$')) {
  throw 'FIRMIUS_VERSION must be latest or a numeric release tag (for example v1.2.3).'
}

if ([Environment]::Is64BitOperatingSystem -eq $false) {
  throw 'Firmius requires a 64-bit Windows installation.'
}
$base = if ($Version -eq 'latest') {
  "https://github.com/$Repo/releases/latest/download"
} else {
  if (-not $Version.StartsWith('v')) { $Version = "v$Version" }
  "https://github.com/$Repo/releases/download/$Version"
}
$archive = Join-Path $env:TEMP "firmius-$([guid]::NewGuid()).zip"
$checksums = Join-Path $env:TEMP "firmius-SHA256SUMS-$([guid]::NewGuid())"
$unpacked = Join-Path $env:TEMP "firmius-$([guid]::NewGuid())"
New-Item -ItemType Directory -Force $unpacked | Out-Null
$Utf8NoBom = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false
$markerTemp = $null
$updateStateTemp = $null
$deferred = $false

function Write-JsonNoBom([string] $Path, [object] $Value) {
  $json = $Value | ConvertTo-Json -Compress
  [System.IO.File]::WriteAllText($Path, $json, $Utf8NoBom)
}

function Test-SharingViolation([System.Exception] $Exception) {
  $current = $Exception
  while ($null -ne $current) {
    if ($current -is [System.IO.IOException]) {
      $nativeError = $current.HResult -band 0xffff
      if (($nativeError -eq 32) -or ($nativeError -eq 33)) { return $true }
    }
    $current = $current.InnerException
  }
  return $false
}

Write-Host "`n  ┌──────────────────────────────────────────┐" -ForegroundColor Cyan
Write-Host "  │              FIRMIUS INSTALLER           │" -ForegroundColor Cyan
Write-Host "  └──────────────────────────────────────────┘" -ForegroundColor Cyan
Write-Host "  Platform: x86_64-pc-windows-msvc"
Write-Host "  Destination: $(Join-Path $InstallDir 'firmius.exe')"
Write-Host '  Downloading release...'

try {
  $asset = 'firmius-x86_64-pc-windows-msvc.zip'
  Invoke-WebRequest -Uri "$base/$asset" -OutFile $archive
  Invoke-WebRequest -Uri "$base/SHA256SUMS" -OutFile $checksums
  $expectedLine = Get-Content $checksums | Where-Object { $_ -match "^[0-9a-fA-F]{64}\s+\*?$([regex]::Escape($asset))$" } | Select-Object -First 1
  if (-not $expectedLine) { throw "SHA256SUMS did not contain $asset; refusing an unverified install." }
  $expected = ($expectedLine -split '\s+')[0]
  $actual = (Get-FileHash -Algorithm SHA256 $archive).Hash
  if ($actual -ne $expected) { throw 'Checksum verification failed.' }
  Write-Host '  Checksum verified.'
  Expand-Archive -Path $archive -DestinationPath $unpacked -Force
  $binary = Get-ChildItem $unpacked -Filter 'firmius.exe' -Recurse | Select-Object -First 1
  if (-not $binary) { throw 'The release archive did not contain firmius.exe.' }
  New-Item -ItemType Directory -Force $InstallDir | Out-Null
  $destination = Join-Path $InstallDir 'firmius.exe'
  $staged = Join-Path $InstallDir ".firmius.new.$PID.exe"
  $marker = Join-Path $InstallDir 'firmius-install.json'
  $markerTemp = Join-Path $InstallDir ".firmius-install.json.$PID"
  $updateState = Join-Path $InstallDir 'firmius-update-state.json'
  $updateStateTemp = Join-Path $InstallDir ".firmius-update-state.json.$PID"

  $daemonRoot = if ($env:FIRMIUS_DATA_DIR) { $env:FIRMIUS_DATA_DIR } else { Join-Path $HOME '.firmius' }
  if ((Test-Path $destination) -and (Test-Path (Join-Path $daemonRoot 'daemon.lock'))) {
    Write-Host '  Stopping the running Firmius daemon before replacing the shared executable...'
    & $destination daemon-stop
    if ($LASTEXITCODE -ne 0) { throw 'Could not stop the running Firmius daemon; refusing to replace its executable.' }
  }

  # A live pending record owns its stage file. Old, unreferenced stage files
  # are safe to remove, but never race a detached replacement helper.
  $protectedStage = $null
  if (Test-Path $updateState) {
    try {
      $oldState = Get-Content -Raw $updateState | ConvertFrom-Json
      if ($oldState.status -eq 'pending') {
        throw "A Firmius replacement is already pending. Inspect $updateState before retrying."
      }
      $protectedStage = $oldState.staged
    } catch [System.Management.Automation.RuntimeException] {
      throw
    } catch {
      throw "Could not validate existing update state ${updateState}: $($_.Exception.Message)"
    }
  }
  Get-ChildItem $InstallDir -Filter '.firmius.new.*.exe' -File -ErrorAction SilentlyContinue |
    Where-Object { ($_.FullName -ne $protectedStage) -and ($_.LastWriteTimeUtc -lt [DateTime]::UtcNow.AddDays(-1)) } |
    Remove-Item -Force -ErrorAction SilentlyContinue

  # Write BOM-less UTF-8 on Windows PowerShell 5.1 as well as PowerShell 7.
  # This marker is informational; the client never treats it as authentication.
  Write-JsonNoBom $markerTemp ([ordered]@{ channel = 'release-script'; repo = $Repo; version = $Version })
  Copy-Item $binary.FullName $staged -Force
  $hadExisting = Test-Path $destination
  if ($hadExisting) { Write-Host '  Existing install found; replacing it safely.' } else { Write-Host '  Creating a new install.' }
  try {
    if ($hadExisting) {
      [System.IO.File]::Replace($staged, $destination, $null)
    } else {
      [System.IO.File]::Move($staged, $destination)
    }
  } catch {
    if (-not (Test-SharingViolation $_.Exception)) {
      Remove-Item $staged -Force -ErrorAction SilentlyContinue
      Remove-Item $markerTemp -Force -ErrorAction SilentlyContinue
      throw
    }

    # Only an OS sharing/lock violation may be deferred. Record pending state
    # atomically before launching a detached helper; callers must not mistake
    # a successfully queued helper for a verified installation.
    Write-Host '  Firmius is in use; recording a pending replacement for process exit.' -ForegroundColor Yellow
    Write-JsonNoBom $updateStateTemp ([ordered]@{
      status = 'pending'
      staged = $staged
      destination = $destination
      created_utc = [DateTime]::UtcNow.ToString('o')
    })
    Move-Item $updateStateTemp $updateState -Force
    $quotedStaged = $staged.Replace("'", "''")
    $quotedDestination = $destination.Replace("'", "''")
    $quotedMarkerTemp = $markerTemp.Replace("'", "''")
    $quotedMarker = $marker.Replace("'", "''")
    $quotedState = $updateState.Replace("'", "''")
    $helper = @"
`$ErrorActionPreference = 'Stop'
`$utf8 = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList `$false
function Write-Failure([string] `$message) {
  `$value = [ordered]@{ status = 'failed'; staged = '$quotedStaged'; destination = '$quotedDestination'; error = `$message; failed_utc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json -Compress
  [System.IO.File]::WriteAllText('$quotedState', `$value, `$utf8)
}
function Is-Sharing([System.Exception] `$exception) {
  `$current = `$exception
  while (`$null -ne `$current) {
    if (`$current -is [System.IO.IOException]) {
      `$code = `$current.HResult -band 0xffff
      if ((`$code -eq 32) -or (`$code -eq 33)) { return `$true }
    }
    `$current = `$current.InnerException
  }
  return `$false
}
for (`$i = 0; `$i -lt 120; `$i++) {
  try {
    if (Test-Path '$quotedDestination') { [System.IO.File]::Replace('$quotedStaged', '$quotedDestination', `$null) }
    else { [System.IO.File]::Move('$quotedStaged', '$quotedDestination') }
    try {
      Move-Item '$quotedMarkerTemp' '$quotedMarker' -Force -ErrorAction Stop
      Remove-Item '$quotedState' -Force -ErrorAction Stop
      exit 0
    } catch {
      Write-Failure "Binary replaced but metadata finalization failed: `$(`$_.Exception.Message)"
      exit 1
    }
  } catch {
    if (-not (Is-Sharing `$_.Exception)) {
      Write-Failure `$_.Exception.Message
      exit 1
    }
    Start-Sleep -Milliseconds 500
  }
}
Write-Failure 'Timed out waiting for the existing Firmius process to release the executable.'
exit 1
"@
    $encodedHelper = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($helper))
    try {
      Start-Process powershell.exe -WindowStyle Hidden -ArgumentList @('-NoProfile', '-NonInteractive', '-EncodedCommand', $encodedHelper) | Out-Null
      $deferred = $true
    } catch {
      Write-JsonNoBom $updateStateTemp ([ordered]@{
        status = 'failed'
        staged = $staged
        destination = $destination
        error = "Could not launch replacement helper: $($_.Exception.Message)"
        failed_utc = [DateTime]::UtcNow.ToString('o')
      })
      Move-Item $updateStateTemp $updateState -Force
      throw
    }
  }
  if (-not $deferred) {
    try {
      Move-Item $markerTemp $marker -Force -ErrorAction Stop
      Remove-Item $updateState -Force -ErrorAction SilentlyContinue
    } catch {
      Write-JsonNoBom $updateStateTemp ([ordered]@{
        status = 'failed'
        staged = $null
        destination = $destination
        error = "Binary replaced but metadata finalization failed: $($_.Exception.Message)"
        failed_utc = [DateTime]::UtcNow.ToString('o')
      })
      Move-Item $updateStateTemp $updateState -Force
      throw
    }
  }
} finally {
  Remove-Item $archive -Force -ErrorAction SilentlyContinue
  Remove-Item $checksums -Force -ErrorAction SilentlyContinue
  Remove-Item $unpacked -Recurse -Force -ErrorAction SilentlyContinue
  if ($null -ne $updateStateTemp) {
    Remove-Item $updateStateTemp -Force -ErrorAction SilentlyContinue
  }
  if ((-not $deferred) -and ($null -ne $markerTemp)) {
    Remove-Item $markerTemp -Force -ErrorAction SilentlyContinue
  }
}

if ($deferred) {
  Write-Host "`n  ! Replacement is pending, not yet installed. Status: $updateState" -ForegroundColor Yellow
  Write-Host '  Exit all Firmius processes, then inspect the status file or rerun the installer.'
  exit 2
}

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (($userPath -split ';') -notcontains $InstallDir) {
  [Environment]::SetEnvironmentVariable('Path', (($userPath, $InstallDir) -join ';'), 'User')
  Write-Host "  Added $InstallDir to your user PATH. Open a new terminal to use it."
}
Write-Host "`n  ✓ Firmius installed successfully. Run: firmius" -ForegroundColor Green
