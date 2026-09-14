# Builds the full deployment package (or just one component) under OutDir:
#
#   <OutDir>\
#     client.exe / common.dll / coverage.dll / runtime DLLs
#     config\client.json
#     start_client.bat
#     testenv\
#       program\            StepTest.exe + PDB + all engine DLLs + version.txt
#       dataset\            stp files + version.txt
#       opencppcoverage\    OpenCppCoverage.exe + dependencies
#
# Versions are tracked per component in testenv\program\version.txt and
# testenv\dataset\version.txt. Update only the component that changed:
#   -OnlyClient   rebuild the client binaries/config only
#   -OnlyProgram  replace testenv\program and bump its version
#   -OnlyDataset  replace testenv\dataset and bump its version
param(
    [string]$OutDir = 'C:\Shared\client_package',
    [string]$BinDir = $null,
    [string]$Endpoint = '',
    [string]$WorkerId = 'worker-1',
    [ValidateSet('coverage', 'stpcompare')][string]$Plugin = 'coverage',
    [string]$ConverterExe = '',
    [int]$TaskThreads = 2,
    [string]$StoreType = 'postgres',
    [string]$DbConn = '',
    [string]$ServerIp = '',
    [int]$ServerPort = 5555,
    [int]$DbPort = 5432,
    [string]$DbName = 'testtracking',
    [string]$DbUser = 'postgres',
    [string]$DbPassword = '',
    [switch]$StoreCov,
    [switch]$OnlyClient,
    [switch]$OnlyProgram,
    [switch]$OnlyDataset,
    [string]$ProgramVersion = '1.0',
    [string]$DatasetVersion = 'd1',
    [string[]]$EngineDlls = @('TKernel.dll', 'TKBRep.dll', 'TKDESTEP.dll', 'TKTopAlgo.dll'),
    [string]$StepTestExe = 'C:\WorkSpace\occt_demo\build\Debug\StepTest.exe',

    [string]$EngineSrcDir = '',
    [string]$DatasetDir = 'C:\WorkSpace\occt_demo\step',
    [string]$OpenCppCoverageDir = 'C:\Program Files\OpenCppCoverage'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ToolSupport.ps1')
if ($Plugin -eq 'stpcompare') {
    if (-not $ConverterExe -and -not $OnlyClient -and -not $OnlyDataset) {
        throw 'Pass -ConverterExe for a stpcompare program package'
    }
    $StepTestExe = if ($ConverterExe) { $ConverterExe } else { 'Converter.exe' }
}
function Get-LocalIPv4 {
    $candidates = @()
    foreach ($nic in [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
        if ($nic.OperationalStatus -ne [System.Net.NetworkInformation.OperationalStatus]::Up) { continue }
        foreach ($addr in $nic.GetIPProperties().UnicastAddresses) {
            if ($addr.Address.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) { continue }
            $ip = $addr.Address.ToString()
            if ($ip -eq '127.0.0.1') { continue }
            if ($ip.StartsWith('169.254.')) { continue }
            $candidates += $ip
        }
    }
    if ($candidates.Count -gt 0) { return $candidates }
    throw 'Cannot auto-detect server IPv4 address; pass -ServerIp or -Endpoint.'
}

if (-not $ServerIp) {
    $candidateIps = @(Get-LocalIPv4)
    $ServerIp = $candidateIps[0]
    Write-Host "detected IPv4 candidates: $($candidateIps -join ', '); using $ServerIp (override with -ServerIp)"
}
if (-not $Endpoint) { $Endpoint = "tcp://${ServerIp}:$ServerPort" }
if (-not $DbConn -and $StoreType -eq 'postgres') {
    $DbConn = New-PostgresConnection $ServerIp $DbPort $DbName $DbUser $DbPassword
}

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

$testenv = Join-Path $OutDir 'testenv'

if (-not $OnlyClient -and -not $OnlyDataset) {
    # testenv\program: target exe + PDB + all engine DLLs + instrumented PDBs.
    if (-not (Test-Path $StepTestExe)) {
        throw "StepTest.exe not found: $StepTestExe"
    }
    if ($EngineSrcDir -and -not (Test-Path $EngineSrcDir)) {
        throw "engine source dir not found: $EngineSrcDir"
    }
    if ($Plugin -eq 'coverage' -and -not (Test-Path (Join-Path $OpenCppCoverageDir 'OpenCppCoverage.exe'))) {
        throw "OpenCppCoverage.exe not found under $OpenCppCoverageDir"
    }
    $progDir = Join-Path $testenv 'program'
    if (Test-Path $progDir) {
        Assert-OwnedPath $progDir $OutDir | Out-Null
        Remove-Item -LiteralPath $progDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force $progDir | Out-Null
    Copy-Item -LiteralPath $StepTestExe -Destination $progDir
    $stepPdb = [System.IO.Path]::ChangeExtension($StepTestExe, '.pdb')
    if (Test-Path $stepPdb) {
        Copy-Item -LiteralPath $stepPdb -Destination $progDir
    }
    $stepDir = Split-Path $StepTestExe -Parent
    foreach ($dll in $EngineDlls) {
        $pdbPath = [System.IO.Path]::ChangeExtension((Join-Path $stepDir $dll), '.pdb')
        if (Test-Path $pdbPath) {
            Copy-Item -LiteralPath $pdbPath -Destination $progDir
        }
    }
    Get-ChildItem $stepDir -Filter '*.dll' | Copy-Item -Destination $progDir
    if ($EngineSrcDir) {
        Copy-Item -Path $EngineSrcDir -Destination (Join-Path $progDir 'engine_src') -Recurse
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $progDir 'version.txt'),
        $ProgramVersion,
        (New-Object System.Text.UTF8Encoding($false)))

    if ($Plugin -eq 'coverage') {
    # testenv\opencppcoverage (static tooling).
    $occDir = Join-Path $testenv 'opencppcoverage'
    if (Test-Path $occDir) {
        Assert-OwnedPath $occDir $OutDir | Out-Null
        Remove-Item -LiteralPath $occDir -Recurse -Force
    }
    $occSourceName = Split-Path $OpenCppCoverageDir -Leaf
    if (Test-Path (Join-Path $testenv $occSourceName)) {
        Assert-OwnedPath (Join-Path $testenv $occSourceName) $OutDir | Out-Null
        Remove-Item -LiteralPath (Join-Path $testenv $occSourceName) -Recurse -Force
    }
    New-Item -ItemType Directory -Force $occDir | Out-Null
    if (Get-Command robocopy -ErrorAction SilentlyContinue) {
        & robocopy $OpenCppCoverageDir $occDir /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) {
            throw "robocopy opencppcoverage failed with exit code $LASTEXITCODE"
        }
    } else {
        Get-ChildItem $OpenCppCoverageDir | Copy-Item -Destination $occDir -Recurse
    }
    Get-ChildItem $occDir -Filter 'unins*' | Remove-Item -Force
    Write-Host "built testenv\program (version $ProgramVersion) + opencppcoverage"
    }
}

if (-not $OnlyClient -and -not $OnlyProgram) {
    $dataDir = Join-Path $testenv 'dataset'
    if (Test-Path $dataDir) {
        Assert-OwnedPath $dataDir $OutDir | Out-Null
        Remove-Item -LiteralPath $dataDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force $dataDir | Out-Null
    if (Get-Command robocopy -ErrorAction SilentlyContinue) {
        & robocopy $DatasetDir $dataDir *.stp /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) {
            throw "robocopy failed with exit code $LASTEXITCODE"
        }
    } else {
        Get-ChildItem $DatasetDir -File -Filter *.stp -Recurse | ForEach-Object {
            $rel = $_.FullName.Substring((Resolve-Path $DatasetDir).Path.Length + 1)
            $dest = Join-Path $dataDir $rel
            New-Item -ItemType Directory -Force (Split-Path $dest -Parent) | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $dest
        }
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $dataDir 'version.txt'),
        $DatasetVersion,
        (New-Object System.Text.UTF8Encoding($false)))
    & (Join-Path $PSScriptRoot 'build_dataset_manifest.ps1') `
        -DatasetDir $dataDir `
        -OutFile (Join-Path $dataDir 'manifest.txt')
    $count = (Get-ChildItem $dataDir -File -Filter *.stp -Recurse | Measure-Object).Count
    Write-Host "built testenv\dataset ($count files, version $DatasetVersion)"
}

if (-not $OnlyProgram -and -not $OnlyDataset) {
    New-Item -ItemType Directory -Force (Join-Path $OutDir 'config') | Out-Null
    Copy-Item -LiteralPath (Join-Path $BinDir 'client.exe') -Destination $OutDir
    Get-ChildItem $BinDir -Filter '*.dll' | Copy-Item -Destination $OutDir

    $coverageCfg = @{
        dll = 'coverage.dll'
        enabled = $true
        dry_run = $false
        store_type = $StoreType
        store_cov = [bool]$StoreCov
        store_dir = 'store'
        program_dir = 'testenv/program'
        stp_root = 'testenv/dataset'
        export_dir = 'reports'
        opencppcoverage = 'testenv/opencppcoverage/OpenCppCoverage.exe'
        target_exe = 'testenv/program/StepTest.exe'
        engine_dlls = $EngineDlls | ForEach-Object { "testenv/program/$_" }
    }
    if ($DbConn) {
        $coverageCfg['db_conn'] = $DbConn
    }
    $selectedPlugin = $coverageCfg
    if ($Plugin -eq 'stpcompare') {
        $selectedPlugin = @{
            dll = 'stpcompare.dll'; enabled = $true; dry_run = $false
            store_type = $StoreType; db_conn = $DbConn; store_dir = 'store'
            program_dir = 'testenv/program'; program_exe = ('testenv/program/' + (Split-Path -Leaf $StepTestExe))
            stp_root = 'testenv/dataset'; work_dir = 'stp_compare_work'
            info_file = 'info.txt'; info_new_file = 'info_new.txt'; table = 'stp_compare_old'
        }
    }
    $clientCfg = @{
        scheduler = @{
            endpoint = $Endpoint
            poll_timeout_ms = 50
        }
        client = @{
            worker_id = $WorkerId
            heartbeat_interval_ms = 1000
            task_threads = $TaskThreads
            request_backoff_initial_ms = 200
            request_backoff_max_ms = 30000
            register_timeout_ms = 5000
            register_max_attempts = 5
        }
        plugins = @{ $Plugin = $selectedPlugin }
    } | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText(
        (Join-Path $OutDir 'config\client.json'),
        $clientCfg,
        (New-Object System.Text.UTF8Encoding($false)))

    @"
@echo off
cd /d "%~dp0"
client.exe config\client.json
pause
"@ | Set-Content -Path (Join-Path $OutDir 'start_client.bat') -Encoding ASCII

    Write-Host "built client package: $OutDir"
}

Write-Host "done. copy '$OutDir' to the target machine and run start_client.bat"
Write-Host "NOTE: endpoint/db_conn default to this machine's detected IPv4 ($ServerIp); pass -ServerIp/-Endpoint/-DbConn to override."
