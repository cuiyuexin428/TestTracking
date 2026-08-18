# Packages a runnable client into a standalone directory that can be copied
# to a blank machine: client.exe, common.dll, the coverage plugin, all
# runtime DLLs, a target-machine client.json, a start script and a README.
param(
    [string]$BinDir = $null,
    [string]$OutDir = 'client_package',
    [string]$Endpoint = 'tcp://127.0.0.1:5555',
    [string]$WorkerId = 'worker-1',
    [string]$StoreType = 'file',
    [string]$DbConn = '',
    [switch]$Deployed,
    [string]$ProgramVersion = '',
    [string]$DatasetVersion = '',
    [switch]$DryRun,
    [switch]$StoreCov,
    [switch]$IncludePdb
)
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
if (-not $BinDir) {
    $candidates = @(
        (Join-Path $root 'out\build\x64-Debug\bin\Debug'),
        (Join-Path $root 'out\build\Release\bin\Release'),
        (Join-Path $root 'build\bin\Release')
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c 'client.exe')) {
            $BinDir = $c
            break
        }
    }
}
if (-not $BinDir -or -not (Test-Path (Join-Path $BinDir 'client.exe'))) {
    throw "client.exe not found; pass -BinDir <bin dir>"
}
if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $root $OutDir
}

if (Test-Path $OutDir) {
    Write-Host "Removing existing package dir: $OutDir"
    Remove-Item -LiteralPath $OutDir -Recurse -Force
}
New-Item -ItemType Directory -Force (Join-Path $OutDir 'config') | Out-Null

Copy-Item -LiteralPath (Join-Path $BinDir 'client.exe') -Destination $OutDir
Get-ChildItem $BinDir -Filter '*.dll' | Copy-Item -Destination $OutDir
if ($IncludePdb) {
    Get-ChildItem $BinDir -Filter '*.pdb' | Copy-Item -Destination $OutDir
}

$coverageCfg = @{
    dll = 'coverage.dll'
    enabled = $true
    dry_run = [bool]$DryRun
    store_type = $StoreType
    store_dir = 'store'
    store_cov = [bool]$StoreCov
    deploy_state = ''
    deploy_max_attempts = 3
    deploy_retry_delay_ms = 5000
    program_dir = 'program'
    stp_root = 'dataset'
    export_dir = 'reports'
    opencppcoverage = 'OpenCppCoverage/OpenCppCoverage.exe'
    target_exe = 'StepTest.exe'
    occt_dlls = @('TKernel.dll', 'TKBRep.dll', 'TKDESTEP.dll', 'TKTopAlgo.dll')
}
if ($DbConn) {
    $coverageCfg['db_conn'] = $DbConn
}
if ($Deployed) {
    $coverageCfg['deployed'] = $true
    $coverageCfg['program_version'] = $ProgramVersion
    $coverageCfg['dataset_version'] = $DatasetVersion
}

$clientCfg = @{
    scheduler = @{
        endpoint = $Endpoint
        poll_timeout_ms = 50
    }
    client = @{
        worker_id = $WorkerId
        heartbeat_interval_ms = 1000
        task_threads = 2
    }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $false }
        coverage = $coverageCfg
    }
} | ConvertTo-Json -Depth 6
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $OutDir 'config\client.json'), $clientCfg, $utf8NoBom)

@"
@echo off
cd /d "%~dp0"
client.exe config\client.json
pause
"@ | Set-Content -Path (Join-Path $OutDir 'start_client.bat') -Encoding ASCII

@"
client_package - standalone client
================================

1. MUST EDIT config/client.json
   - scheduler.endpoint: the scheduler address, e.g. tcp://192.168.1.100:5555
   - client.worker_id: must be unique per machine
   - When packaged with -StoreType postgres, db_conn points at the scheduler
     machine's PostgreSQL and must be reachable from this machine.

2. PREREQUISITES
   - Firewall must allow outbound to the scheduler port 5555
     (and 5432 when PostgreSQL is used)
   - The client must be able to read the shared directory configured on the
     scheduler (UNC path + read permission)
   - The scheduler side must have stp_manifest, expected versions and
     shared_dir configured
   - Shared directory layout:
     shared/program/  StepTest.exe, OCCT DLLs, PDBs, OpenCppCoverage.exe, manifest.txt
     shared/dataset/  stp files, manifest.txt

3. START
   Double-click start_client.bat, or run:
       client.exe config\client.json

4. TWO TEST MODES
   - Blank-machine deployment (default): do NOT set deployed/version keys in
     the config. The client copies program+dataset from the shared directory
     and verifies the manifests during registration.
   - Quick connectivity: keep coverage.dry_run true and set
     "deployed": true plus program_version/dataset_version matching the
     scheduler; the client skips deployment and runs synthetic tasks.

5. REAL COVERAGE RUN
   Set coverage.dry_run to false. The program package must contain
   OpenCppCoverage.exe and PDBs. Set store_cov to true on both the client and
   the scheduler when cov merging is required.
"@ | Set-Content -Path (Join-Path $OutDir 'README.txt') -Encoding UTF8

Write-Host "Package created: $OutDir"
Write-Host "  bin: $BinDir"
Write-Host "  edit config\client.json: endpoint=$Endpoint worker_id=$WorkerId"
Write-Host "  copy the whole folder to the target machine and run start_client.bat"
