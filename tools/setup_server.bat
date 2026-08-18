@echo off
setlocal

REM One-time server-side setup (run with administrator privileges):
REM   1. creates C:\Shared
REM   2. shares it as "Shared" with read access for Everyone
REM   3. grants NTFS read on existing and future files
REM   4. opens inbound firewall ports 445 (SMB), 5555 (scheduler), 5432 (PostgreSQL)
REM The scheduler config already points at \\<machine-ip>\Shared.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo === TestTracking server setup ===

if not exist "C:\Shared" mkdir "C:\Shared"
if not exist "C:\Shared" (
    echo [ERROR] Cannot create C:\Shared
    goto :done
)

REM Re-create the share so the grant list is exactly "Everyone read".
net share Shared /delete >nul 2>&1
net share Shared=C:\Shared /grant:Everyone,READ
if %errorlevel% neq 0 (
    echo [ERROR] Failed to create share "Shared". Is C:\Shared already shared under another name?
)

REM NTFS read for Everyone, propagated to existing files (may take a minute
REM on a large dataset) and inherited by future files.
icacls "C:\Shared" /grant Everyone:(OI)(CI)R /T >nul
if %errorlevel% neq 0 (
    echo [WARN] icacls reported an error; existing files may not be readable.
)

netsh advfirewall firewall delete rule name="TestTracking SMB 445" >nul 2>&1
netsh advfirewall firewall add rule name="TestTracking SMB 445" dir=in action=allow protocol=TCP localport=445

netsh advfirewall firewall delete rule name="TestTracking Scheduler 5555" >nul 2>&1
netsh advfirewall firewall add rule name="TestTracking Scheduler 5555" dir=in action=allow protocol=TCP localport=5555

netsh advfirewall firewall delete rule name="TestTracking PostgreSQL 5432" >nul 2>&1
netsh advfirewall firewall add rule name="TestTracking PostgreSQL 5432" dir=in action=allow protocol=TCP localport=5432

echo.
echo === Done ===
echo Share    : \\MACHINE-IP\Shared  (detect your IP with ipconfig)
echo Firewall : 445 / 5555 / 5432 inbound allowed
echo.
echo Next steps:
echo   1. Run tools\init_database.ps1, then restart the scheduler so it loads the new config.
echo   2. Copy C:\Shared\client_package to the target machine.
echo   3. For remote clients writing to the local PostgreSQL, run:
echo      powershell -ExecutionPolicy Bypass -File tools\setup_postgres_remote.ps1
echo   4. On the target machine run start_client.bat.

:done
pause
