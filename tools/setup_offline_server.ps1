<#
One-command offline server setup.

Copy the whole TestTracking folder to an offline machine, edit
tools\offline_deploy.json (dataset/program sources and versions), then run:

  powershell -ExecutionPolicy Bypass -File .\tools\setup_offline_server.ps1

The script, in order:
  1. verifies the toolchain and builds the project (Release by default)
  2. installs/initializes/starts the bundled PostgreSQL server and creates the database
  3. stages the stp dataset and regenerates the dataset manifest
  4. writes scheduler and web console configs (db, management, logs, analysis)
  5. builds the full client package (client.exe + selected plugin + business programs + dataset)
  6. installs the bundled Python analysis dependencies without using the network
  7. opens LAN firewall ports and generates startup scripts

Afterwards copy the client package to each client machine and run
start_client.bat. Start scheduler and web console with runtime\start_server.bat.
The web console can also run independently via runtime\start_result_service.bat.

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
    [int]$ManagementPort = 0,
    [int]$ResultPort = 0,
    [int]$DbPort = 0,
    [string]$PgData = '',
    [string]$DbName = '',
    [string]$DbUser = '',
    [string]$DbPassword = '',
    [string]$DatasetDir = '',
    [string]$DatasetVersion = '',
    [string]$StepTestExe = '',
    [ValidateSet('', 'coverage', 'stpcompare')][string]$Plugin = '',
    [string]$BaselineConverterExe = '',
    [string]$CandidateConverterExe = '',
    [string]$EngineSrcDir = '',
    [string]$OpenCppCoverageDir = '',
    [string]$ProgramVersion = '',
    [string]$BaselineProgramVersion = '',
    [string]$CandidateProgramVersion = '',
    [string]$OutDir = '',
    [string]$WorkerId = '',
    [switch]$SkipBuild,
    [switch]$SkipDatabase,
    [switch]$SkipDataset,
    [switch]$SkipPackage,
    [switch]$SkipFirewall,
    [switch]$SkipPythonAnalysis,
    [switch]$NoService,
    [switch]$StartScheduler
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ToolSupport.ps1')

$ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
function Resolve-ProjectPath([string]$Path) {
    if (-not $Path) { return '' }
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $Path))
}

if (-not $DeployConfig) {
    $DeployConfig = Join-Path $PSScriptRoot 'offline_deploy.json'
} else {
    $DeployConfig = Resolve-ProjectPath $DeployConfig
}
$deploy = @{}
if (Test-Path $DeployConfig) {
    $parsed = Get-Content -LiteralPath $DeployConfig -Raw -Encoding UTF8 | ConvertFrom-Json
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
    return New-PostgresConnection $HostName $Port $Db $User $Password
}

$buildConfig = if ($Config) { $Config } else { [string](Get-DeployValue 'build_config' 'Release') }
$serverPort = if ($ServerPort -gt 0) { $ServerPort } else { [int](Get-DeployValue 'server_port' 5555) }
$managementPort = if ($ManagementPort -gt 0) { $ManagementPort } else { [int](Get-DeployValue 'management_port' 5556) }
$resultPort = if ($ResultPort -gt 0) { $ResultPort } else { [int](Get-DeployValue 'result_port' 56330) }
$dbPort = if ($DbPort -gt 0) { $DbPort } else { [int](Get-DeployValue 'db_port' 5432) }
foreach ($port in @($serverPort, $managementPort, $resultPort, $dbPort)) {
    if ($port -lt 1 -or $port -gt 65535) { throw "Invalid TCP port: $port" }
}
if (@($serverPort, $managementPort, $resultPort, $dbPort | Select-Object -Unique).Count -ne 4) {
    throw 'Scheduler, management, result service and database must use different TCP ports.'
}
$dbName = if ($DbName) { $DbName } else { [string](Get-DeployValue 'db_name' 'testtracking') }
$dbUser = if ($DbUser) { $DbUser } else { [string](Get-DeployValue 'db_user' 'postgres') }
$dbPassword = if ($DbPassword) { $DbPassword } else { [string](Get-DeployValue 'db_password' '') }
$authMethod = [string](Get-DeployValue 'db_auth_method' 'trust')
$clientCidr = [string](Get-DeployValue 'client_cidr' '0.0.0.0/0')
$registerService = [bool](Get-DeployValue 'register_db_service' $true)
if ($NoService) { $registerService = $false }
$datasetDir = if ($DatasetDir) { $DatasetDir } else { [string](Get-DeployValue 'dataset_dir' '') }
$datasetVersion = if ($DatasetVersion) { $DatasetVersion } else { [string](Get-DeployValue 'dataset_version' 'd1') }
$engineDlls = @()
$rawDlls = Get-DeployValue 'engine_dlls' @()
if ($rawDlls) { $engineDlls = @($rawDlls) }
$stepTestExe = if ($StepTestExe) { $StepTestExe } else { [string](Get-DeployValue 'step_test_exe' '') }
$plugin = if ($Plugin) { $Plugin } else { [string](Get-DeployValue 'plugin' 'coverage') }
if ($plugin -notin @('coverage', 'stpcompare')) { throw 'plugin must be coverage or stpcompare' }
$baselineConverterExe = if ($BaselineConverterExe) { $BaselineConverterExe } else { [string](Get-DeployValue 'baseline_converter_exe' '') }
$candidateConverterExe = if ($CandidateConverterExe) { $CandidateConverterExe } else { [string](Get-DeployValue 'candidate_converter_exe' '') }
$engineSrcDir = if ($EngineSrcDir) { $EngineSrcDir } else { [string](Get-DeployValue 'engine_src_dir' '') }
$opencppcoverageDir = if ($OpenCppCoverageDir) { $OpenCppCoverageDir } else { [string](Get-DeployValue 'opencppcoverage_dir' '') }
$programVersion = if ($ProgramVersion) { $ProgramVersion } else { [string](Get-DeployValue 'program_version' '1.0') }
$baselineProgramVersion = if ($BaselineProgramVersion) { $BaselineProgramVersion } else { [string](Get-DeployValue 'baseline_program_version' '1.0') }
$candidateProgramVersion = if ($CandidateProgramVersion) { $CandidateProgramVersion } else { [string](Get-DeployValue 'candidate_program_version' '1.0') }
$outDirRaw = if ($OutDir) { $OutDir } else { [string](Get-DeployValue 'package_out_dir' 'dist\client_package') }
$workerId = if ($WorkerId) { $WorkerId } else { [string](Get-DeployValue 'worker_id' 'worker-1') }
$taskThreads = [int](Get-DeployValue 'task_threads' 2)
$taskSize = [int](Get-DeployValue 'task_size' 25)
$storeCov = [bool](Get-DeployValue 'store_cov' $true)

# All deployment inputs are interpreted relative to the project root, so the
# setup script behaves the same when invoked from another working directory.
$BuildDir = if ($BuildDir) { Resolve-ProjectPath $BuildDir } else { '' }
$PgData = if ($PgData) { Resolve-ProjectPath $PgData } else { '' }
$datasetDir = if ($datasetDir) { Resolve-ProjectPath $datasetDir } else { '' }
$stepTestExe = if ($stepTestExe) { Resolve-ProjectPath $stepTestExe } else { '' }
$baselineConverterExe = if ($baselineConverterExe) { Resolve-ProjectPath $baselineConverterExe } else { '' }
$candidateConverterExe = if ($candidateConverterExe) { Resolve-ProjectPath $candidateConverterExe } else { '' }
$engineSrcDir = if ($engineSrcDir) { Resolve-ProjectPath $engineSrcDir } else { '' }
$opencppcoverageDir = if ($opencppcoverageDir) { Resolve-ProjectPath $opencppcoverageDir } else { '' }

if (-not $serverIp) { $serverIp = [string](Get-DeployValue 'server_ip' '') }
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
$managedDatabase = $null

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
    $managedPgRoot = Join-Path $ProjectRoot 'third_party\pgsql'
    if (-not (Test-Path -LiteralPath (Join-Path $managedPgRoot 'bin\initdb.exe'))) {
        $managedPgRoot = Join-Path $runtimeDir 'pgsql'
    }
    $managedDatabase = @{
        pg_ctl = Join-Path $managedPgRoot 'bin\pg_ctl.exe'
        data_dir = if ($PgData) { $PgData } else { Join-Path $runtimeDir 'pgdata' }
    }
} else {
    Write-Host 'Skipped (as requested).'
}

Write-Host ''
Write-Host '=== 3. dataset staging + manifest ==='
if (-not $SkipDataset) {
    if (-not (Test-Path $datasetDir)) {
        throw "dataset source not found: $datasetDir"
    }
    $sourceDatasetPath = (Resolve-Path -LiteralPath $datasetDir).Path.TrimEnd('\', '/')
    $stagedDatasetPath = [IO.Path]::GetFullPath($datasetStage).TrimEnd('\', '/')
    if ($sourceDatasetPath.Equals($stagedDatasetPath, [StringComparison]::OrdinalIgnoreCase) -or
        $sourceDatasetPath.StartsWith($stagedDatasetPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Dataset source must be outside runtime\dataset. Use -SkipDataset to reuse an already staged dataset; no data was removed.'
    }
    if (Test-Path $datasetStage) {
        Assert-OwnedPath $datasetStage $runtimeDir | Out-Null
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
$sched = Get-Content -LiteralPath $schedTemplate -Raw -Encoding UTF8 | ConvertFrom-Json
$localConn = Build-DbConn '127.0.0.1' $dbPort $dbName $dbUser $dbPassword
$sched.server.db_conn = $localConn
$sched.server.router_endpoint = "tcp://*:$serverPort"
$sched.server | Add-Member -NotePropertyName log_file -NotePropertyValue (Join-Path $runtimeDir 'logs\scheduler.log') -Force
$sched.server | Add-Member -NotePropertyName management_endpoint -NotePropertyValue "http://127.0.0.1:$managementPort" -Force
$sched.plugins.coverage.db_conn = $localConn
$sched.plugins.coverage.stp_manifest = ($datasetStage + '/manifest.txt').Replace('\', '/')
$sched.plugins.coverage.expected_program_version = [string]$programVersion
$sched.plugins.coverage.expected_dataset_version = [string]$datasetVersion
$sched.plugins.coverage.task_size = $taskSize
foreach ($pluginName in @('coverage', 'stpcompare')) {
    $pluginConfig = $sched.plugins.$pluginName
    $pluginConfig.db_conn = $localConn
    $pluginConfig.stp_manifest = ($datasetStage + '/manifest.txt').Replace('\', '/')
    $pluginConfig.task_size = $taskSize
    $pluginConfig | Add-Member -NotePropertyName expected_dataset_version -NotePropertyValue ([string]$datasetVersion) -Force
    if ($pluginName -eq 'coverage') {
        $pluginConfig | Add-Member -NotePropertyName expected_program_version -NotePropertyValue ([string]$programVersion) -Force
    } else {
        $pluginConfig.PSObject.Properties.Remove('expected_program_version')
        $pluginConfig | Add-Member -NotePropertyName expected_baseline_program_version -NotePropertyValue ([string]$baselineProgramVersion) -Force
        $pluginConfig | Add-Member -NotePropertyName expected_candidate_program_version -NotePropertyValue ([string]$candidateProgramVersion) -Force
    }
}
# The CMake ALL target refreshes bin/config from the source template on every
# build. Keep deployment settings outside build output so later Release
# recompiles cannot replace the live database, endpoint and dataset choices.
New-Item -ItemType Directory -Force -Path $runtimeDir | Out-Null
$outCfg = Join-Path $runtimeDir 'scheduler.json'
Write-AtomicText $outCfg ($sched | ConvertTo-Json -Depth 10)
Write-Host "Scheduler config written: $outCfg"

Write-Host ''
Write-Host '=== 5. client package ==='
if (-not $SkipPackage) {
    $endpoint = "tcp://${serverIp}:$serverPort"
    & (Join-Path $PSScriptRoot 'build_package.ps1') `
        -OutDir $outDir `
        -BinDir $binDir `
        -Config $buildConfig `
        -Plugin $plugin -BaselineConverterExe $baselineConverterExe -CandidateConverterExe $candidateConverterExe `
        -Endpoint $endpoint `
        -DeploymentEndpoint "http://${serverIp}:$resultPort" `
        -ServerIp $serverIp `
        -ServerPort $serverPort `
        -DbPort $dbPort -DbName $dbName -DbUser $dbUser -DbPassword $dbPassword `
        -WorkerId $workerId `
        -TaskThreads $taskThreads `
        -StoreCov:$storeCov `
        -ProgramVersion $programVersion -BaselineProgramVersion $baselineProgramVersion `
        -CandidateProgramVersion $candidateProgramVersion -DatasetVersion $datasetVersion `
        -StepTestExe $stepTestExe -EngineSrcDir $engineSrcDir -EngineDlls $engineDlls `
        -DatasetDir $datasetStage -OpenCppCoverageDir $opencppcoverageDir
} else {
    Write-Host 'Skipped (as requested).'
}

Write-Host ''
Write-Host '=== 6. result service and offline Python analysis ==='
New-Item -ItemType Directory -Force -Path $runtimeDir, (Join-Path $runtimeDir 'logs'), (Join-Path $runtimeDir 'artifact_import') | Out-Null
$resultTemplate = Join-Path $ProjectRoot 'result_service\config.example.json'
if (-not (Test-Path -LiteralPath $resultTemplate -PathType Leaf)) {
    throw "Result service template not found: $resultTemplate"
}
$result = Get-Content -LiteralPath $resultTemplate -Raw -Encoding UTF8 | ConvertFrom-Json
$result.host = '0.0.0.0'
$result.port = $resultPort
$result.db_conn = $localConn
foreach ($entry in @{
    workspace = '..'
    static_dir = '../result_service/web'
    plugin_views_dir = ''
    plugin_adapters_dir = ''
    plugin_ai_contexts_dir = ''
    analysis_scripts_dir = 'result_service/analysis_scripts'
    analysis_reports_dir = 'result_service/reports'
    report_templates_dir = ''
    artifact_store_dir = 'result_service/artifacts'
    artifact_import_roots = @('artifact_import')
    scheduler_management_url = "http://127.0.0.1:$managementPort"
    scheduler_config_path = $outCfg
    logs_dir = 'logs'
}.GetEnumerator()) {
    $result | Add-Member -NotePropertyName $entry.Key -NotePropertyValue $entry.Value -Force
}
$clientConfig = Join-Path $outDir 'config\client.json'
if ($SkipPackage -or -not (Test-Path -LiteralPath $clientConfig -PathType Leaf)) {
    $templateName = if ($plugin -eq 'stpcompare') { 'client.stpcompare.json' } else { 'client.json' }
    $clientTemplate = Get-Content -LiteralPath (Join-Path $ProjectRoot "client\$templateName") -Raw -Encoding UTF8 | ConvertFrom-Json
    $clientTemplate.scheduler.endpoint = "tcp://${serverIp}:$serverPort"
    $clientTemplate.client.worker_id = $workerId
    $clientTemplate.client.task_threads = $taskThreads
    $remoteConn = Build-DbConn $serverIp $dbPort $dbName $dbUser $dbPassword
    foreach ($property in $clientTemplate.plugins.PSObject.Properties) {
        if ($property.Value.PSObject.Properties.Name -contains 'db_conn') { $property.Value.db_conn = $remoteConn }
        if ($property.Value.PSObject.Properties.Name -contains 'dry_run') { $property.Value.dry_run = $false }
    }
    if ($clientTemplate.plugins.stpcompare.programs) {
        $clientTemplate.plugins.stpcompare.programs.baseline.version = [string]$baselineProgramVersion
        $clientTemplate.plugins.stpcompare.programs.candidate.version = [string]$candidateProgramVersion
    }
    $clientConfig = Join-Path $runtimeDir 'client.template.json'
    Write-AtomicText $clientConfig ($clientTemplate | ConvertTo-Json -Depth 20)
    Write-Host 'Client template only: configure actual business executables and dataset paths before use; dry_run is disabled.'
}
$result | Add-Member -NotePropertyName client_config_path -NotePropertyValue $clientConfig -Force
$resultCfg = Join-Path $runtimeDir 'result_service.json'
# Preserve user-edited AI provider settings on repeated setup. Endpoint and
# dataset choices still come from this invocation's deployment configuration.
if (Test-Path -LiteralPath $resultCfg -PathType Leaf) {
    $previousResult = Get-Content -LiteralPath $resultCfg -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($previousResult.providers) { $result.providers = $previousResult.providers }
    if ($previousResult.plugin_options) { $result.plugin_options = $previousResult.plugin_options }
}
$dshCommand = [string](Get-DeployValue 'dsh_command' '')
if ($dshCommand) { $result.providers.dsh.command = $dshCommand }
$clientForAnalysis = Get-Content -LiteralPath $clientConfig -Raw -Encoding UTF8 | ConvertFrom-Json
if ($clientForAnalysis.plugins.stpcompare -and $clientForAnalysis.plugins.stpcompare.table) {
    $result.plugin_options.stpcompare.result_table = [string]$clientForAnalysis.plugins.stpcompare.table
} elseif (-not $result.plugin_options.stpcompare.result_table) {
    $result.plugin_options.stpcompare.result_table = 'stp_compare_old'
}
Write-AtomicText $resultCfg ($result | ConvertTo-Json -Depth 20)
if (-not $SkipPythonAnalysis) {
    & (Join-Path $PSScriptRoot 'install_python_analysis_offline.ps1') `
        -RuntimeDir (Join-Path $runtimeDir 'python') `
        -Wheelhouse (Join-Path $runtimeDir 'python-analysis-wheelhouse') `
        -SupportScript (Join-Path $PSScriptRoot 'python_analysis_support.py')
} else { Write-Host 'Python analysis installation skipped (as requested).' }

Write-Host ''
Write-Host '=== 7. firewall and startup scripts ==='
if (-not $SkipFirewall) {
    foreach ($port in @($serverPort, $dbPort, $resultPort)) {
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

$launchSettings = @{ bin_dir = $binDir; scheduler_config_path = $outCfg; managed_database = $managedDatabase }
Write-AtomicText (Join-Path $runtimeDir 'source_launch.json') ($launchSettings | ConvertTo-Json)
# Keep generated batch files ASCII: all non-ASCII paths are resolved by
# PowerShell from the runtime directory, never interpolated into cmd.exe text.
foreach ($action in @('start_server', 'start_scheduler', 'start_result_service', 'stop_result_service')) {
    $targetAction = if ($action -eq 'start_scheduler') { 'start_server' } else { $action }
    $batch = @"
@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\tools\source_services.ps1" -Action $targetAction -RuntimeDir "%~dp0."
pause
"@
    [IO.File]::WriteAllText((Join-Path $runtimeDir ($action + '.bat')), $batch, [Text.Encoding]::ASCII)
}
$startBat = Join-Path $runtimeDir 'start_server.bat'

Write-Host ''
Write-Host '=== Setup complete ==='
Write-Host "Scheduler exe : $schedulerExe"
Write-Host "Scheduler cfg : $outCfg"
Write-Host "Start script  : $startBat"
Write-Host "Result cfg    : $resultCfg"
Write-Host "Web console   : http://${serverIp}:$resultPort/"
Write-Host "Management    : http://127.0.0.1:$managementPort (local only)"
Write-Host "Client package: $outDir"
Write-Host "Database      : host=$serverIp port=$dbPort dbname=$dbName user=$dbUser"
Write-Host ''
Write-Host 'Next steps:'
Write-Host "  1. Start scheduler and web console: $startBat"
Write-Host "  2. Copy '$outDir' to each client machine and run start_client.bat"
Write-Host '  3. Install/configure DSH with tools\setup_offline_dependencies.ps1 if it is not already available.'
if ($StartScheduler) {
    & (Join-Path $PSScriptRoot 'source_services.ps1') -Action start_server -RuntimeDir $runtimeDir
}
