<#
One-command offline server setup.

Copy the whole TestTracking folder to an offline machine, edit
tools\offline_deploy.json (dataset/program sources and versions), then run:

  powershell -ExecutionPolicy Bypass -File .\tools\setup_offline_server.ps1

The script, in order:
  1. verifies the toolchain and builds the project (Debug by default)
  2. installs/initializes/starts the bundled PostgreSQL server and creates the database
  3. stages the stp dataset and regenerates the dataset manifest
  4. writes the scheduler config (db, manifest, expected versions)
  5. builds the full client package (client.exe + plugins + StepTest + dataset + OpenCppCoverage)
  6. opens firewall ports and prints where everything is

Afterwards copy the client package to each client machine and run
start_client.bat. Start the scheduler with runtime\start_scheduler.bat.

Run as administrator so the Windows service and firewall rules can be set up;
without elevation pg_ctl is used and firewall setup is skipped with a warning.

Parameters override values from tools\offline_deploy.json.
#>
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$DeployConfig = '',
    [string]$Config = '',
    [string]$BuildDir = '',
    [string]$ServerIp = '',
    [int]$ServerPort = 0,
    [int]$DbPort = 0,
    [string]$PgData = '',
    [string]$DbName = '',
    [string]$DbUser = '',
    [string]$DbPassword = '',
    [string]$DatasetDir = '',
    [string]$DatasetVersion = '',
    [string]$StepTestExe = '',
    [string]$OcctSrcDir = '',
    [string]$OpenCppCoverageDir = '',
    [string]$ProgramVersion = '',
    [string]$OutDir = '',
    [string]$WorkerId = '',
    [switch]$SkipBuild,
    [switch]$SkipDatabase,
    [switch]$SkipDataset,
    [switch]$SkipPackage,
    [switch]$SkipFirewall,
    [switch]$NoService,
    [switch]$StartScheduler
)
$ErrorActionPreference = 'Stop'

if (-not $DeployConfig) {
    $DeployConfig = Join-Path $PSScriptRoot 'offline_deploy.json'
}
$deploy = @{}
if (Test-Path $DeployConfig) {
    $parsed = Get-Content -Raw $DeployConfig | ConvertFrom-Json
    $parsed.PSObject.Properties | ForEach-Object {
        $deploy[$_.Name] = $_.Value
    }
    Write-Host "Deploy config: $DeployConfig"
} else {
    Write-Host "WARN: deploy config not found ($DeployConfig); using defaults"
}

function Get-DeployValue([string]$Name, $Default) {
    if ($deploy.ContainsKey($Name) -and $null -ne $deploy[$Name]) {
        return $deploy[$Name]
    }
    return $Default
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
    throw 'Cannot auto-detect server IPv4 address; pass -ServerIp or set server_ip in offline_deploy.json.'
}

function Build-DbConn([string]$HostName, [int]$Port, [string]$Db, [string]$User, [string]$Password) {
    $conn = "host=$HostName port=$Port dbname=$Db user=$User"
    if ($Password) { $conn += " password=$Password" }
    return $conn
}

$buildConfig = if ($Config) { $Config } else { [string](Get-DeployValue 'build_config' 'Debug') }
$serverPort = if ($ServerPort -gt 0) { $ServerPort } else { [int](Get-DeployValue 'server_port' 5555) }
$dbPort = if ($DbPort -gt 0) { $DbPort } else { [int](Get-DeployValue 'db_port' 5432) }
$dbName = if ($DbName) { $DbName } else { [string](Get-DeployValue 'db_name' 'testtracking') }
$dbUser = if ($DbUser) { $DbUser } else { [string](Get-DeployValue 'db_user' 'postgres') }
$dbPassword = if ($DbPassword) { $DbPassword } else { [string](Get-DeployValue 'db_password' '') }
$authMethod = [string](Get-DeployValue 'db_auth_method' 'trust')
$clientCidr = [string](Get-DeployValue 'client_cidr' '0.0.0.0/0')
$registerService = [bool](Get-DeployValue 'register_db_service' $true)
if ($NoService) { $registerService = $false }
$datasetDir = if ($DatasetDir) { $DatasetDir } else { [string](Get-DeployValue 'dataset_dir' '') }
$datasetVersion = if ($DatasetVersion) { $DatasetVersion } else { [string](Get-DeployValue 'dataset_version' 'd1') }
$stepTestExe = if ($StepTestExe) { $StepTestExe } else { [string](Get-DeployValue 'step_test_exe' '') }
$occtSrcDir = if ($OcctSrcDir) { $OcctSrcDir } else { [string](Get-DeployValue 'occt_src_dir' '') }
$opencppcoverageDir = if ($OpenCppCoverageDir) { $OpenCppCoverageDir } else { [string](Get-DeployValue 'opencppcoverage_dir' '') }
$programVersion = if ($ProgramVersion) { $ProgramVersion } else { [string](Get-DeployValue 'program_version' '1.0') }
$outDirRaw = if ($OutDir) { $OutDir } else { [string](Get-DeployValue 'package_out_dir' 'dist\client_package') }
$workerId = if ($WorkerId) { $WorkerId } else { [string](Get-DeployValue 'worker_id' 'worker-1') }
$taskThreads = [int](Get-DeployValue 'task_threads' 2)
$batchSize = [int](Get-DeployValue 'batch_size' 1000)
$taskSize = [int](Get-DeployValue 'task_size' 25)
$failedRetries = [int](Get-DeployValue 'failed_file_retries' 0)
$storeCov = [bool](Get-DeployValue 'store_cov' $true)

if (-not $serverIp) {
    $candidateIps = @(Get-LocalIPv4)
    $serverIp = $candidateIps[0]
    Write-Host "Detected IPv4 candidates: $($candidateIps -join ', '); using $serverIp"
}
if (-not $BuildDir) {
    $BuildDir = Join-Path $ProjectRoot 'build\offline_verify'
}
if ([System.IO.Path]::IsPathRooted($outDirRaw)) {
    $outDir = $outDirRaw
} else {
    $outDir = Join-Path $ProjectRoot $outDirRaw
}
$runtimeDir = Join-Path $ProjectRoot 'runtime'
$datasetStage = Join-Path $runtimeDir 'dataset'
$binDir = Join-Path $BuildDir ("bin\" + $buildConfig)

Write-Host ''
Write-Host '=== 1. build ==='
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'verify_offline_build.ps1') `
        -ProjectRoot $ProjectRoot -BuildDir $BuildDir -Config $buildConfig
    if ($LASTEXITCODE -ne 0) {
        throw 'offline build failed'
    }
}
$schedulerExe = Join-Path $binDir 'scheduler.exe'
if (-not (Test-Path $schedulerExe)) {
    throw "scheduler.exe not found under $binDir; run without -SkipBuild"
}
$clientExe = Join-Path $binDir 'client.exe'
if (-not (Test-Path $clientExe)) {
    throw "client.exe not found under $binDir; run without -SkipBuild"
}
Write-Host "Build output: $binDir"

Write-Host ''
Write-Host '=== 2. database ==='
if (-not $SkipDatabase) {
    $dbScript = Join-Path $PSScriptRoot 'init_postgres_offline.ps1'
    & $dbScript `
        -ProjectRoot $ProjectRoot `
        -DbPort $dbPort -PgData $PgData -DbName $dbName -DbUser $dbUser -DbPassword $dbPassword `
        -AuthMethod $authMethod -ClientCidr $clientCidr `
        -NoService:$(-not $registerService) `
        -NoFirewall:$SkipFirewall
} else {
    Write-Host 'Skipped (as requested).'
}

Write-Host ''
Write-Host '=== 3. dataset staging + manifest ==='
if (-not $SkipDataset) {
    if (-not (Test-Path $datasetDir)) {
        throw "dataset source not found: $datasetDir"
    }
    if (Test-Path $datasetStage) {
        Remove-Item -LiteralPath $datasetStage -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $datasetStage | Out-Null
    if (Get-Command robocopy -ErrorAction SilentlyContinue) {
        & robocopy $datasetDir $datasetStage *.stp /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) {
            throw "dataset staging failed (robocopy exit code $LASTEXITCODE)"
        }
    } else {
        Get-ChildItem $datasetDir -File -Filter *.stp -Recurse | ForEach-Object {
            $rel = $_.FullName.Substring((Resolve-Path $datasetDir).Path.Length + 1)
            $dest = Join-Path $datasetStage $rel
            New-Item -ItemType Directory -Force -Path (Split-Path $dest -Parent) | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $dest
        }
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $datasetStage 'version.txt'),
        $datasetVersion,
        (New-Object System.Text.UTF8Encoding($false)))
    & (Join-Path $PSScriptRoot 'build_dataset_manifest.ps1') `
        -DatasetDir $datasetStage `
        -OutFile (Join-Path $datasetStage 'manifest.txt')
    $stpCount = (Get-ChildItem $datasetStage -File -Filter *.stp -Recurse | Measure-Object).Count
    Write-Host "Staged dataset: $stpCount stp files, version $datasetVersion -> $datasetStage"
} else {
    Write-Host 'Skipped (as requested); using existing staged dataset.'
}

Write-Host ''
Write-Host '=== 4. scheduler config ==='
$schedTemplate = Join-Path $ProjectRoot 'scheduler\scheduler.json'
if (-not (Test-Path $schedTemplate)) {
    throw "scheduler template not found: $schedTemplate"
}
$sched = Get-Content -Raw $schedTemplate | ConvertFrom-Json
$localConn = Build-DbConn '127.0.0.1' $dbPort $dbName $dbUser $dbPassword
$sched.server.db_conn = $localConn
$sched.plugins.coverage.db_conn = $localConn
$sched.plugins.coverage.stp_manifest = ($datasetStage + '/manifest.txt').Replace('\', '/')
$sched.plugins.coverage.expected_program_version = [string]$programVersion
$sched.plugins.coverage.expected_dataset_version = [string]$datasetVersion
$sched.plugins.coverage.opencppcoverage = (Join-Path $opencppcoverageDir 'OpenCppCoverage.exe').Replace('\', '/')
$sched.plugins.coverage.batch_size = $batchSize
$sched.plugins.coverage.task_size = $taskSize
$sched.plugins.coverage.failed_file_retries = $failedRetries
$sched.plugins.coverage.store_cov = $storeCov
$outCfgDir = Join-Path $binDir 'config'
New-Item -ItemType Directory -Force -Path $outCfgDir | Out-Null
$outCfg = Join-Path $outCfgDir 'scheduler.json'
[System.IO.File]::WriteAllText(
    $outCfg,
    ($sched | ConvertTo-Json -Depth 10),
    (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Scheduler config written: $outCfg"

Write-Host ''
Write-Host '=== 5. client package ==='
if (-not $SkipPackage) {
    $endpoint = "tcp://${serverIp}:$serverPort"
    & (Join-Path $PSScriptRoot 'build_package.ps1') `
        -OutDir $outDir `
        -BinDir $binDir `
        -Endpoint $endpoint `
        -ServerIp $serverIp `
        -ServerPort $serverPort `
        -DbPort $dbPort -DbName $dbName -DbUser $dbUser -DbPassword $dbPassword `
        -WorkerId $workerId `
        -TaskThreads $taskThreads `
        -StoreCov:$storeCov `
        -ProgramVersion $programVersion -DatasetVersion $datasetVersion `
        -StepTestExe $stepTestExe -OcctSrcDir $occtSrcDir `
        -DatasetDir $datasetDir -OpenCppCoverageDir $opencppcoverageDir
} else {
    Write-Host 'Skipped (as requested).'
}

Write-Host ''
Write-Host '=== 6. firewall ==='
if (-not $SkipFirewall) {
    foreach ($port in @($serverPort, $dbPort)) {
        & netsh advfirewall firewall delete rule name="TestTracking $port" 2>$null | Out-Null
        & netsh advfirewall firewall add rule name="TestTracking $port" dir=in action=allow protocol=TCP localport=$port | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "WARN: firewall rule for port $port failed; run as administrator or open it manually."
        } else {
            Write-Host "Firewall: inbound TCP $port allowed."
        }
    }
} else {
    Write-Host 'Skipped (as requested).'
}

$startBat = Join-Path $runtimeDir 'start_scheduler.bat'
New-Item -ItemType Directory -Force -Path $runtimeDir | Out-Null
@"
@echo off
cd /d "$binDir"
scheduler.exe config\scheduler.json
pause
"@ | Set-Content -Path $startBat -Encoding ASCII

Write-Host ''
Write-Host '=== Setup complete ==='
Write-Host "Scheduler exe : $schedulerExe"
Write-Host "Scheduler cfg : $outCfg"
Write-Host "Start script  : $startBat"
Write-Host "Client package: $outDir"
Write-Host "Database      : host=$serverIp port=$dbPort dbname=$dbName user=$dbUser"
Write-Host ''
Write-Host 'Next steps:'
Write-Host "  1. Start the scheduler: $startBat"
Write-Host "  2. Copy '$outDir' to each client machine and run start_client.bat"
if ($StartScheduler) {
    Push-Location $binDir
    & $schedulerExe (Join-Path $binDir 'config\scheduler.json')
    Pop-Location
}
