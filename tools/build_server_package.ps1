<#
Build a relocatable server distribution from an existing CMake output.

The package is intentionally self-contained and is always run with its root as
the current directory:

  <out>\
    bin\scheduler.exe, plugins, result tools and runtime DLLs
    config\scheduler.json
    tools\settle_*.ps1 and ToolSupport.ps1
    data\dataset\ (optional STP manifest and inputs)
    data\artifacts, data\reports, data\logs
    scripts\start_server.ps1, stop_server.ps1, health_check.ps1
    release-manifest.json

No path in scheduler.json points back to the source or build tree. The
database connection is supplied at package-build time (or can be replaced in
config/scheduler.json before starting the package).
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$BinDir = '',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutDir = '',
    [string]$DbConn = 'host=127.0.0.1 port=5432 dbname=testtracking user=postgres',
    [string]$ResultDbConn = '',
    [int]$ServerPort = 5555,
    [string]$DatasetDir = '',
    [string]$DatasetVersion = '',
    [string]$ProgramVersion = '',
    [string]$PackageVersion = '0.1.0',
    [switch]$SkipDataset,
    [switch]$IncludeSymbols,
    [switch]$IncludePostgresRuntime,
    [string]$PgBinDir = ''
)
$ErrorActionPreference = 'Stop'

function Resolve-FullPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}

function Ensure-Directory([string]$Path) {
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Copy-FileChecked([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required file not found: $Source"
    }
    Ensure-Directory (Split-Path -Parent $Destination)
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Copy-DirectoryContents([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw "Directory not found: $Source"
    }
    Ensure-Directory $Destination
    if (Get-Command robocopy -ErrorAction SilentlyContinue) {
        & robocopy $Source $Destination /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE): $Source -> $Destination" }
    } else {
        Get-ChildItem -LiteralPath $Source -Force | Copy-Item -Destination $Destination -Recurse -Force
    }
}

function Find-BinaryDir {
    param([string]$Root, [string]$Explicit, [string]$BuildConfig)
    if ($Explicit) {
        $candidate = Resolve-FullPath $Explicit
        if (Test-Path (Join-Path $candidate 'scheduler.exe')) { return $candidate }
        $nested = Join-Path $candidate ("bin\$BuildConfig")
        if (Test-Path (Join-Path $nested 'scheduler.exe')) { return $nested }
        throw "scheduler.exe not found under -BinDir $candidate"
    }
    $candidates = @(
        (Join-Path $Root ("out\build\x64-Debug\bin\$BuildConfig")),
        (Join-Path $Root ("out\build\$BuildConfig\bin\$BuildConfig")),
        (Join-Path $Root ("build\rt\bin\$BuildConfig")),
        (Join-Path $Root ("build\offline_verify\bin\$BuildConfig")),
        (Join-Path $Root ("build\refactor-build-local-generated\bin\$BuildConfig")),
        (Join-Path $Root ("build\bin\$BuildConfig"))
    )
    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate 'scheduler.exe')) { return (Resolve-FullPath $candidate) }
    }
    $found = @(Get-ChildItem -LiteralPath (Join-Path $Root 'build') -Filter scheduler.exe -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.DirectoryName 'common.dll') } |
        Select-Object -First 1)
    if ($found.Count -gt 0) { return $found[0].DirectoryName }
    throw "scheduler.exe not found. Pass -BinDir <CMake bin directory>."
}

function Rewrite-SchedulerConfig([string]$Template, [string]$Destination, [string]$Connection, [int]$Port, [string]$ManifestPath, [string]$ProgramVer, [string]$DatasetVer) {
    if (-not (Test-Path -LiteralPath $Template -PathType Leaf)) { throw "scheduler template not found: $Template" }
    $config = Get-Content -LiteralPath $Template -Raw | ConvertFrom-Json
    if (-not $config.server) { $config | Add-Member -NotePropertyName server -NotePropertyValue ([pscustomobject]@{}) }
    $config.server.db_conn = $Connection
    $config.server.router_endpoint = "tcp://*:$Port"
    foreach ($pluginName in @('coverage', 'stpcompare')) {
        $plugin = $config.plugins.$pluginName
        if ($null -eq $plugin) { continue }
        if ($plugin.dll) { $plugin.dll = ('bin/' + [System.IO.Path]::GetFileName([string]$plugin.dll)) }
        $plugin.stp_manifest = $ManifestPath
        $plugin.db_conn = $Connection
        if ($ProgramVer) { $plugin | Add-Member -NotePropertyName expected_program_version -NotePropertyValue $ProgramVer -Force }
        if ($DatasetVer) { $plugin | Add-Member -NotePropertyName expected_dataset_version -NotePropertyValue $DatasetVer -Force }
    }
    if ($config.plugins) {
        foreach ($property in $config.plugins.PSObject.Properties) {
            if ($property.Value -and $property.Value.dll) {
                $property.Value.dll = ('bin/' + [System.IO.Path]::GetFileName([string]$property.Value.dll))
            }
        }
    }
    Ensure-Directory (Split-Path -Parent $Destination)
    $json = $config | ConvertTo-Json -Depth 20
    [System.IO.File]::WriteAllText($Destination, $json, (New-Object System.Text.UTF8Encoding($false)))
}

function Write-TextFile([string]$Path, [string]$Text) {
    Ensure-Directory (Split-Path -Parent $Path)
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

$ProjectRoot = Resolve-FullPath $ProjectRoot
if (-not $OutDir) { $OutDir = Join-Path $ProjectRoot ("dist\server-$PackageVersion") }
$OutDir = Resolve-FullPath $OutDir
$bin = Find-BinaryDir -Root $ProjectRoot -Explicit $BinDir -BuildConfig $Config

if (Test-Path -LiteralPath $OutDir) {
    # The output is owned by this command; reject suspicious destinations.
    $resolvedRoot = (Resolve-Path $ProjectRoot).Path.TrimEnd('\')
    $resolvedOut = [System.IO.Path]::GetFullPath($OutDir).TrimEnd('\')
    if ($resolvedOut -eq $resolvedRoot -or -not $resolvedOut.StartsWith($resolvedRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove output outside ProjectRoot: $OutDir"
    }
    Remove-Item -LiteralPath $OutDir -Recurse -Force
}
Ensure-Directory $OutDir
$binOut = Join-Path $OutDir 'bin'
$cfgOut = Join-Path $OutDir 'config'
$toolsOut = Join-Path $OutDir 'tools'
$scriptsOut = Join-Path $OutDir 'scripts'
$dataOut = Join-Path $OutDir 'data'
$resultOut = Join-Path $OutDir 'result_service'
foreach ($dir in @($binOut, $cfgOut, $toolsOut, $scriptsOut, $resultOut, (Join-Path $dataOut 'dataset'), (Join-Path $dataOut 'artifacts'), (Join-Path $dataOut 'reports'), (Join-Path $dataOut 'logs'))) {
    Ensure-Directory $dir
}

# All scheduler/plugin and third-party DLLs are built into one directory by
# CMake. Copying DLLs wholesale keeps the package independent of PATH.
$requiredExes = @('scheduler.exe')
$optionalExes = @('coverage_settle.exe')
foreach ($name in $requiredExes) { Copy-FileChecked (Join-Path $bin $name) (Join-Path $binOut $name) }
foreach ($name in $optionalExes) {
    $source = Join-Path $bin $name
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $binOut $name) -Force }
    else { Write-Warning "Optional server tool not found: $source" }
}
$dlls = @(Get-ChildItem -LiteralPath $bin -Filter '*.dll' -File -ErrorAction SilentlyContinue |
    Where-Object { $_.BaseName -notmatch '(?i)(^test_|_probe$|probe_)' })
if ($dlls.Count -eq 0) { throw "No DLLs found next to scheduler.exe: $bin" }
foreach ($dll in $dlls) { Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $binOut $dll.Name) -Force }
if ($IncludeSymbols) {
    Get-ChildItem -LiteralPath $bin -Filter '*.pdb' -File -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $binOut $_.Name) -Force
    }
}

$datasetSource = if ($DatasetDir) { Resolve-FullPath $DatasetDir } else { Join-Path $ProjectRoot 'runtime\dataset' }
$manifestRelative = 'data/dataset/manifest.txt'
if (-not $SkipDataset) {
    if (-not (Test-Path -LiteralPath $datasetSource -PathType Container)) { throw "Dataset directory not found: $datasetSource (pass -SkipDataset to omit it)" }
    Copy-DirectoryContents $datasetSource (Join-Path $dataOut 'dataset')
    $manifest = Join-Path $dataOut 'dataset\manifest.txt'
    if (-not (Test-Path -LiteralPath $manifest)) {
        & (Join-Path $PSScriptRoot 'build_dataset_manifest.ps1') -DatasetDir (Join-Path $dataOut 'dataset') -OutFile $manifest
    }
    if ($DatasetVersion) { Write-TextFile (Join-Path $dataOut 'dataset\version.txt') ($DatasetVersion + "`n") }
} elseif (-not (Test-Path -LiteralPath (Join-Path $dataOut 'dataset\manifest.txt'))) {
    Write-TextFile (Join-Path $dataOut 'dataset\README.txt') "Provide data/dataset/manifest.txt before enabling a plugin.`n"
}

$template = Join-Path $ProjectRoot 'scheduler\scheduler.json'
Rewrite-SchedulerConfig -Template $template -Destination (Join-Path $cfgOut 'scheduler.json') -Connection $DbConn -Port $ServerPort -ManifestPath $manifestRelative -ProgramVer $ProgramVersion -DatasetVer $DatasetVersion

# Settlement and maintenance scripts are part of the server package. The
# package-local ToolSupport is required by all three scripts.
foreach ($name in @('ToolSupport.ps1', 'settle_coverage.ps1', 'settle_stpcompare.ps1', 'prune_old_runs.ps1')) {
    Copy-FileChecked (Join-Path $PSScriptRoot $name) (Join-Path $toolsOut $name)
}
if (Test-Path (Join-Path $binOut 'coverage_settle.exe')) {
    # settle_coverage.ps1 first probes its own directory. Keep a package-local
    # wrapper that points to bin so callers do not need to pass -Exe.
    $settle = Get-Content -LiteralPath (Join-Path $toolsOut 'settle_coverage.ps1') -Raw
    $oldProbe = "(Join-Path `$PSScriptRoot 'coverage_settle.exe')"
    $newProbe = "(Join-Path (Split-Path -Parent `$PSScriptRoot) 'bin\\coverage_settle.exe')"
    $settle = $settle.Replace($oldProbe, $newProbe)
    Write-TextFile (Join-Path $toolsOut 'settle_coverage.ps1') $settle
}

if ($IncludePostgresRuntime) {
    if (-not $PgBinDir) { $PgBinDir = Join-Path $ProjectRoot 'runtime\pgsql\bin' }
    Copy-DirectoryContents (Resolve-FullPath $PgBinDir) (Join-Path $OutDir 'postgres\bin')
}

# Bundle the read-only result/API sidecar and its static UI. Python itself is
# an explicit prerequisite; the package does not silently copy a developer's
# virtual environment or credentials.
$resultSource = Join-Path $ProjectRoot 'result_service'
if (Test-Path -LiteralPath (Join-Path $resultSource 'launcher.py')) {
    Copy-DirectoryContents $resultSource $resultOut
    $resultConfig = [ordered]@{
        host = '127.0.0.1'
        port = 56330
        db_conn = $ResultDbConn
        static_dir = '../result_service/web'
        providers = [ordered]@{
            codex = [ordered]@{ command = 'codex'; args = @('exec', '--json', '--ephemeral', '--skip-git-repo-check', '--sandbox', 'read-only', '-'); timeout_seconds = 120 }
            dsh = [ordered]@{ command = 'dsh'; args = @('--profile', 'headless'); prompt_mode = 'argument'; timeout_seconds = 120 }
        }
    }
    Write-TextFile (Join-Path $cfgOut 'result_service.json') ($resultConfig | ConvertTo-Json -Depth 8)
} else {
    Write-Warning "result_service is not present; server package will not include the result UI/API"
}

$startScript = @'
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$pidFile = Join-Path $root 'data\scheduler.pid'
$exe = Join-Path $root 'bin\scheduler.exe'
$cfg = Join-Path $root 'config\scheduler.json'
if (-not (Test-Path $exe)) { throw "scheduler executable not found: $exe" }
if (-not (Test-Path $cfg)) { throw "scheduler config not found: $cfg" }
if (Test-Path $pidFile) {
    $oldPid = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
    $old = Get-Process -Id $oldPid -ErrorAction SilentlyContinue
    if ($old -and $old.ProcessName -eq 'scheduler') { Write-Host "scheduler already running (pid $oldPid)"; exit 0 }
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
}
$resultLauncher = Join-Path $root 'result_service\launcher.py'
$resultCfg = Join-Path $root 'config\result_service.json'
$resultPidFile = Join-Path $root 'data\result_service.pid'
$resultProc = $null
if ((Test-Path $resultLauncher) -and (Test-Path $resultCfg)) {
    $python = if ($env:PYTHON) { $env:PYTHON } else { 'python' }
    $resultLog = Join-Path $root 'data\logs\result_service.log'
    $resultProc = Start-Process -FilePath $python -ArgumentList @($resultLauncher, '--config', $resultCfg) -WorkingDirectory $root -RedirectStandardOutput $resultLog -RedirectStandardError (Join-Path $root 'data\logs\result_service.err.log') -PassThru
    [System.IO.File]::WriteAllText($resultPidFile, [string]$resultProc.Id)
    Write-Host "result service started (pid $($resultProc.Id))"
}
# Keep scheduler attached to this console so its stdin command protocol remains
# usable. The process is still tracked for health/stop scripts.
$proc = Start-Process -FilePath $exe -ArgumentList $cfg -WorkingDirectory $root -NoNewWindow -PassThru
[System.IO.File]::WriteAllText($pidFile, [string]$proc.Id)
Write-Host "scheduler started (pid $($proc.Id)); type help for commands"
try { Wait-Process -Id $proc.Id } finally {
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
    if ($resultProc) {
        Stop-Process -Id $resultProc.Id -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $resultPidFile -Force -ErrorAction SilentlyContinue
    }
}
'@
Write-TextFile (Join-Path $scriptsOut 'start_server.ps1') $startScript
$startBat = @'
@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0scripts\start_server.ps1"
'@
Write-TextFile (Join-Path $OutDir 'start_server.bat') $startBat

$resultStartScript = @'
[CmdletBinding()]
param([switch]$NoWait)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$python = if ($env:PYTHON) { $env:PYTHON } else { 'python' }
$launcher = Join-Path $root 'result_service\launcher.py'
$cfg = Join-Path $root 'config\result_service.json'
$log = Join-Path $root 'data\logs\result_service.log'
if (-not (Test-Path $launcher)) { throw "result service launcher not found: $launcher" }
if (-not (Test-Path $cfg)) { throw "result service config not found: $cfg" }
$proc = Start-Process -FilePath $python -ArgumentList @($launcher, '--config', $cfg) -WorkingDirectory $root -RedirectStandardOutput $log -RedirectStandardError (Join-Path $root 'data\logs\result_service.err.log') -PassThru
[System.IO.File]::WriteAllText((Join-Path $root 'data\result_service.pid'), [string]$proc.Id)
Write-Host "result service started (pid $($proc.Id)); logs: $log"
if (-not $NoWait) { Wait-Process -Id $proc.Id }
'@
Write-TextFile (Join-Path $scriptsOut 'start_result_service.ps1') $resultStartScript

$resultStopScript = @'
[CmdletBinding()]
param([int]$TimeoutSeconds = 15)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$pidFile = Join-Path $root 'data\result_service.pid'
if (-not (Test-Path $pidFile)) { Write-Host 'result service is not running'; exit 0 }
$pidValue = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
$proc = Get-Process -Id $pidValue -ErrorAction SilentlyContinue
if ($proc) {
    Stop-Process -Id $pidValue
    try { Wait-Process -Id $pidValue -Timeout $TimeoutSeconds -ErrorAction Stop } catch { Stop-Process -Id $pidValue -Force -ErrorAction SilentlyContinue }
}
Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
Write-Host 'result service stopped'
'@
Write-TextFile (Join-Path $scriptsOut 'stop_result_service.ps1') $resultStopScript

$stopScript = @'
[CmdletBinding()]
param([int]$TimeoutSeconds = 15)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$pidFile = Join-Path $root 'data\scheduler.pid'
if (-not (Test-Path $pidFile)) { Write-Host 'scheduler is not running'; exit 0 }
$pidValue = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
$proc = Get-Process -Id $pidValue -ErrorAction SilentlyContinue
if ($proc) {
    if ($proc.ProcessName -ne 'scheduler') { throw "PID file does not refer to scheduler: $($proc.ProcessName)" }
    Stop-Process -Id $pidValue
    try { Wait-Process -Id $pidValue -Timeout $TimeoutSeconds -ErrorAction Stop } catch { Stop-Process -Id $pidValue -Force -ErrorAction SilentlyContinue }
}
Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
Write-Host 'scheduler stopped'
'@
Write-TextFile (Join-Path $scriptsOut 'stop_server.ps1') $stopScript

$healthScript = @'
[CmdletBinding()]
param([int]$ServerPort = 0)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$pidFile = Join-Path $root 'data\scheduler.pid'
if (-not (Test-Path $pidFile)) { Write-Error 'scheduler is not running'; exit 2 }
$pidValue = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
$proc = Get-Process -Id $pidValue -ErrorAction SilentlyContinue
if (-not $proc -or $proc.ProcessName -ne 'scheduler') { Write-Error 'scheduler process is not healthy'; exit 2 }
if ($ServerPort -le 0) {
    $cfg = Get-Content -LiteralPath (Join-Path $root 'config\scheduler.json') -Raw | ConvertFrom-Json
    $endpoint = [string]$cfg.server.router_endpoint
    if ($endpoint -match ':(\d+)') { $ServerPort = [int]$Matches[1] }
}
if ($ServerPort -gt 0 -and -not (Test-NetConnection -ComputerName 127.0.0.1 -Port $ServerPort -InformationLevel Quiet)) {
    Write-Error "scheduler port $ServerPort is not accepting connections"; exit 3
}
Write-Output (ConvertTo-Json @{ status = 'ok'; pid = $pidValue; port = $ServerPort } -Compress)
'@
Write-TextFile (Join-Path $scriptsOut 'health_check.ps1') $healthScript

# Build a machine-readable inventory. The manifest excludes itself so its
# hashes are stable after writing and can be checked by a deployment tool.
$files = @()
Get-ChildItem -LiteralPath $OutDir -File -Recurse | Where-Object { $_.Name -ne 'release-manifest.json' } | ForEach-Object {
    $relative = $_.FullName.Substring($OutDir.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $files += [ordered]@{ path = $relative; size = $_.Length; sha256 = $hash }
}
$manifestObject = [ordered]@{
    format = 1
    package_type = 'testtracking-server'
    package_version = $PackageVersion
    generated_at_utc = [DateTime]::UtcNow.ToString('o')
    scheduler_config = 'config/scheduler.json'
    result_service = if (Test-Path (Join-Path $resultOut 'launcher.py')) { 'result_service/launcher.py' } else { $null }
    protocol = 'zmq'
    server_port = $ServerPort
    dataset_included = (-not $SkipDataset)
    files = @($files | Sort-Object path)
}
Write-TextFile (Join-Path $OutDir 'release-manifest.json') ($manifestObject | ConvertTo-Json -Depth 8)
Write-Host "Server package created: $OutDir"
Write-Host "Binary source: $bin"
Write-Host "Files: $($files.Count); manifest: $(Join-Path $OutDir 'release-manifest.json')"
