@echo off
:: Firmius installer for Windows (cmd.exe).
::
:: Lays out:
::   %ProgramFiles%\Firmius\firmius.exe
::   %ProgramFiles%\Firmius\share\firmius\themes
::   %APPDATA%\Firmius\{prompts,workflows,hinting,themes}
::
:: For per-user installs without admin, set INSTALL_DIR to a directory under
:: your user profile (e.g. %LOCALAPPDATA%\Programs\Firmius).
::
:: Usage:
::   install.bat
::   install.bat /uninstall
::
:: Environment:
::   INSTALL_DIR    Override the install prefix (default: %ProgramFiles%\Firmius)

setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
if "%INSTALL_DIR%"=="" set "INSTALL_DIR=%ProgramFiles%\Firmius"
set "USER_DATA=%APPDATA%\Firmius"
set "ACTION=install"

if /I "%~1"=="/uninstall" set "ACTION=uninstall"
if /I "%~1"=="--uninstall" set "ACTION=uninstall"
if /I "%~1"=="-u" set "ACTION=uninstall"

if "%ACTION%"=="uninstall" (
  echo ==^> Uninstalling firmius from "%INSTALL_DIR%"
  if exist "%INSTALL_DIR%\firmius.exe" del /Q "%INSTALL_DIR%\firmius.exe"
  if exist "%INSTALL_DIR%\firmiusd.exe" del /Q "%INSTALL_DIR%\firmiusd.exe"
  if exist "%INSTALL_DIR%\share\firmius" rd /S /Q "%INSTALL_DIR%\share\firmius"
  if exist "%INSTALL_DIR%" (
    rd "%INSTALL_DIR%" 2>nul
  )
  echo     User data at "%USER_DATA%" was NOT removed (delete manually if desired).
  echo ==^> Done.
  exit /b 0
)

if not exist "%SCRIPT_DIR%bin\firmius.exe" (
  echo error: firmius.exe not found at "%SCRIPT_DIR%bin\firmius.exe"
  exit /b 1
)

echo ==^> Installing firmius
echo     prefix:    %INSTALL_DIR%
echo     user data: %USER_DATA%

if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%" 2>nul
if errorlevel 1 (
  echo error: cannot create "%INSTALL_DIR%". Re-run as Administrator, or set INSTALL_DIR to a user-writable path.
  exit /b 1
)

:: Copy binary + bundled DLLs.
copy /Y "%SCRIPT_DIR%bin\*.exe" "%INSTALL_DIR%\" >nul
copy /Y "%SCRIPT_DIR%bin\*.dll" "%INSTALL_DIR%\" >nul 2>&1

:: System data dir.
if not exist "%INSTALL_DIR%\share\firmius" mkdir "%INSTALL_DIR%\share\firmius" 2>nul
if exist "%SCRIPT_DIR%share\firmius" (
  xcopy /E /I /Y /Q "%SCRIPT_DIR%share\firmius" "%INSTALL_DIR%\share\firmius" >nul
)

:: Per-user assets.
if not exist "%USER_DATA%" mkdir "%USER_DATA%" 2>nul
for %%S in (prompts workflows hinting themes) do (
  if exist "%SCRIPT_DIR%%%S" if not exist "%USER_DATA%\%%S" (
    xcopy /E /I /Y /Q "%SCRIPT_DIR%%%S" "%USER_DATA%\%%S" >nul
    echo     seeded %USER_DATA%\%%S
  )
)

echo.
echo ==^> Done.
echo     Add "%INSTALL_DIR%" to your PATH if it isn't already, then run: firmius
echo.
echo     To add to PATH for the current user (PowerShell, no admin needed):
echo       [Environment]::SetEnvironmentVariable("Path", "$env:Path;%INSTALL_DIR%", "User")

endlocal
exit /b 0
