# Local simulation of the remote blank-machine deployment:
#   1. builds the authoritative shared directory (program + dataset +
#      manifests) that a real server would expose over the network;
#   2. builds the "remote" client directory containing only the client
#      program and config (simulating a freshly copied client machine);
#   3. writes a scheduler config for the simulation.
# After running this script, start the scheduler with the generated
# scheduler_sim.json, start the client from the client dir, and run a test
# run. The client must pull program+dataset from the shared dir before it
# can execute real tasks.
param(
    [string]$BinDir = $null,
    [string]$SharedDir = 'C:\WorkSpace\CoverageShared',
    [string]$ClientDir = 'C:\WorkSpace\CoverageTest',
    [int]$Port = 5560,
    [int]$StpCount = 12,
    [string[]]$OcctDlls = @('TKernel.dll', 'TKBRep.dll', 'TKDESTEP.dll', 'TKTopAlgo.dll'),
    [string]$StepTestExe = 'C:\WorkSpace\occt_demo\build\Debug\StepTest.exe',
    [string]$OcctSrcDir = 'C:\WorkSpace\occt_demo\occtlib\src',
    [string]$DatasetDir = 'C:\WorkSpace\occt_demo\step',
    [string]$OpenCppCoverageDir = 'C:\Program Files\OpenCppCoverage',
    [string]$DbConn = 'host=127.0.0.1 port=5432 dbname=testtracking user=postgres'
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
if (-not (Test-Path $StepTestExe)) {
    throw "StepTest.exe not found: $StepTestExe"
}
if (-not (Test-Path $OcctSrcDir)) {
    throw "OCCT resources not found: $OcctSrcDir"
}
if (-not (Test-Path (Join-Path $OpenCppCoverageDir 'OpenCppCoverage.exe'))) {
    throw "OpenCppCoverage.exe not found under $OpenCppCoverageDir"
}

$sharedProgram = Join-Path $SharedDir 'program'
$sharedDataset = Join-Path $SharedDir 'dataset'
foreach ($d in @($SharedDir, $ClientDir)) {
    if (Test-Path $d) {
        Write-Host "Removing existing dir: $d"
        Remove-Item -LiteralPath $d -Recurse -Force
    }
}
New-Item -ItemType Directory -Force $sharedProgram | Out-Null
New-Item -ItemType Directory -Force $sharedDataset | Out-Null
New-Item -ItemType Directory -Force (Join-Path $ClientDir 'config') | Out-Null

# Program package: StepTest + PDB + OCCT DLLs/PDBs + OpenCppCoverage + OCCT
# resources (StepTest resolves occtlib next to the executable).
Copy-Item -LiteralPath $StepTestExe -Destination $sharedProgram
$stepPdb = [System.IO.Path]::ChangeExtension($StepTestExe, '.pdb')
if (Test-Path $stepPdb) {
    Copy-Item -LiteralPath $stepPdb -Destination $sharedProgram
}
$stepDir = Split-Path $StepTestExe -Parent
foreach ($dll in $OcctDlls) {
    $dllPath = Join-Path $stepDir $dll
    if (-not (Test-Path $dllPath)) {
        throw "OCCT DLL not found: $dllPath"
    }
    $pdbPath = [System.IO.Path]::ChangeExtension($dllPath, '.pdb')
    if (Test-Path $pdbPath) {
        Copy-Item -LiteralPath $pdbPath -Destination $sharedProgram
    }
}
# StepTest loads all of its OCCT dependencies transitively; ship every DLL
# from the build dir. PDBs are only needed for the modules that will be
# instrumented (copied above), keeping the package size sane.
Get-ChildItem $stepDir -Filter '*.dll' | Copy-Item -Destination $sharedProgram
Copy-Item -LiteralPath $OpenCppCoverageDir -Destination $sharedProgram -Recurse
Get-ChildItem (Join-Path $sharedProgram 'OpenCppCoverage') -Filter 'unins*' | Remove-Item -Force
Copy-Item -Path $OcctSrcDir -Destination (Join-Path $sharedProgram 'occtlib') -Recurse

# Dataset: a small subset of the real stp collection.
$stpFiles = Get-ChildItem $DatasetDir -File -Filter *.stp | Sort-Object Name | Select-Object -First $StpCount
if ($stpFiles.Count -lt 2) {
    throw "Need at least 2 stp files under $DatasetDir"
}
foreach ($f in $stpFiles) {
    Copy-Item -LiteralPath $f.FullName -Destination $sharedDataset
}

# Program manifest: relative path,size,sha256 for every file in the package.
$programManifest = @()
foreach ($f in Get-ChildItem $sharedProgram -Recurse -File | Sort-Object FullName) {
    $rel = $f.FullName.Substring($sharedProgram.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLower()
    $programManifest += "$rel,$($f.Length),$hash"
}
[System.IO.File]::WriteAllLines(
    (Join-Path $sharedProgram 'manifest.txt'),
    $programManifest,
    (New-Object System.Text.UTF8Encoding($false)))

$datasetManifest = Get-ChildItem $sharedDataset -File -Filter *.stp | Sort-Object Name | ForEach-Object {
    "$($_.Name),$($_.Length)"
}
[System.IO.File]::WriteAllLines(
    (Join-Path $sharedDataset 'manifest.txt'),
    $datasetManifest,
    (New-Object System.Text.UTF8Encoding($false)))

# Client package: runtime files + config pointing at the simulated remote
# machine paths.
Copy-Item -LiteralPath (Join-Path $BinDir 'client.exe') -Destination $ClientDir
Get-ChildItem $BinDir -Filter '*.dll' | Copy-Item -Destination $ClientDir

$clientCfg = @{
    scheduler = @{
        endpoint = "tcp://127.0.0.1:$Port"
        poll_timeout_ms = 50
    }
    client = @{
        worker_id = 'worker-sim'
        heartbeat_interval_ms = 1000
        task_threads = 2
    }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $false }
        coverage = @{
            dll = 'coverage.dll'
            enabled = $true
            dry_run = $false
            store_type = 'postgres'
            db_conn = $DbConn
            store_dir = (Join-Path $ClientDir 'store')
            store_cov = $true
            deploy_state = ''
            deploy_max_attempts = 3
            deploy_retry_delay_ms = 5000
            program_dir = (Join-Path $ClientDir 'program')
            stp_root = (Join-Path $ClientDir 'dataset')
            export_dir = (Join-Path $ClientDir 'reports')
            opencppcoverage = 'OpenCppCoverage/OpenCppCoverage.exe'
            target_exe = 'StepTest.exe'
            occt_dlls = $OcctDlls
        }
    }
} | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText(
    (Join-Path $ClientDir 'config\client.json'),
    $clientCfg,
    (New-Object System.Text.UTF8Encoding($false)))

@"
@echo off
cd /d "%~dp0"
client.exe config\client.json
pause
"@ | Set-Content -Path (Join-Path $ClientDir 'start_client.bat') -Encoding ASCII

# Scheduler config for the simulation (lives next to the shared dir).
$schedCfg = @{
    server = @{
        router_endpoint = "tcp://*:$Port"
        poll_timeout_ms = 50
        process_threads = 2
        client_timeout_ms = 0
        task_lease_timeout_ms = 0
        db_conn = ''
    }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $false }
        coverage = @{
            dll = 'coverage.dll'
            enabled = $true
            stp_manifest = (Join-Path $sharedDataset 'manifest.txt')
            store_type = 'postgres'
            db_conn = $DbConn
            batch_size = $StpCount
            task_size = 3
            failed_file_retries = 0
            store_cov = $true
            opencppcoverage = (Join-Path $OpenCppCoverageDir 'OpenCppCoverage.exe')
            expected_program_version = '1.0'
            expected_dataset_version = 'd1'
            shared_dir = $SharedDir
            keep_staging = $false
        }
    }
} | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText(
    (Join-Path $SharedDir 'scheduler_sim.json'),
    $schedCfg,
    (New-Object System.Text.UTF8Encoding($false)))

Write-Host "Shared dir : $SharedDir (program=$($programManifest.Count) files, dataset=$($stpFiles.Count) files)"
Write-Host "Client dir : $ClientDir (copy of the client program only)"
Write-Host "Scheduler  : scheduler.exe $SharedDir\scheduler_sim.json (port $Port)"
Write-Host "Start sim  : client.exe from $ClientDir (blank machine flow)"
