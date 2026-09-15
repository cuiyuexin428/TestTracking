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
    runtime\python\ (embedded Python and PostgreSQL driver)
    release-manifest.json

No path in scheduler.json points back to the source or build tree. The
database connection is supplied at package-build time (or can be replaced in
config/scheduler.json before starting the package).
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$BinDir = '',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [string]$OutDir = '',
    [string]$DbConn = 'host=127.0.0.1 port=5432 dbname=testtracking user=postgres',
    [string]$ResultDbConn = '',
    [string]$StpResultTable = '',
    [int]$ServerPort = 5555,
    [string]$DatasetDir = '',
    [string]$DatasetVersion = '',
    [string]$ProgramVersion = '',
    [string]$PackageVersion = '0.1.0',
    [switch]$SkipDataset,
    [switch]$IncludeSymbols,
    [switch]$IncludePostgresRuntime,
    [string]$PgBinDir = '',
    # Optional embeddable CPython directory. When supplied it is copied to
    # runtime/python so the package does not depend on Python being on PATH.
    [string]$PythonRuntimeDir = '',
    [switch]$UseSystemPython,
    [string]$VCRuntimeDir = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageSupport.ps1')
if ($Config -ne 'Release') { throw 'Portable distributions require a Release build; Debug runtime DLLs are not redistributable.' }
if ($UseSystemPython -and $PythonRuntimeDir) { throw 'UseSystemPython and PythonRuntimeDir cannot be combined.' }

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
        $candidate = if ([System.IO.Path]::IsPathRooted($Explicit)) {
            Resolve-FullPath $Explicit
        } else {
            Resolve-FullPath (Join-Path $Root $Explicit)
        }
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
$VCRuntimeDir = if ($VCRuntimeDir) {
    if ([System.IO.Path]::IsPathRooted($VCRuntimeDir)) { Resolve-FullPath $VCRuntimeDir }
    else { Resolve-FullPath (Join-Path $ProjectRoot $VCRuntimeDir) }
} else { '' }
if (-not $OutDir) {
    $OutDir = Join-Path $ProjectRoot ("dist\server-$PackageVersion")
} elseif (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $ProjectRoot $OutDir
}
$OutDir = Resolve-FullPath $OutDir
$PythonRuntimeDir = if ($PythonRuntimeDir) {
    if ([System.IO.Path]::IsPathRooted($PythonRuntimeDir)) {
        Resolve-FullPath $PythonRuntimeDir
    } else {
        Resolve-FullPath (Join-Path $ProjectRoot $PythonRuntimeDir)
    }
} else { '' }
if (-not $UseSystemPython -and -not $PythonRuntimeDir) {
    $PythonRuntimeDir = Join-Path $ProjectRoot 'runtime\python'
    $buildPython = if ($env:PYTHON) { $env:PYTHON } else { 'python' }
    & $buildPython (Join-Path $PSScriptRoot 'prepare_python_runtime.py') --out $PythonRuntimeDir
    if ($LASTEXITCODE -ne 0) { throw 'Preparing portable Python failed; for offline builds pass -PythonRuntimeDir with a prepared runtime.' }
}
if ($PythonRuntimeDir) {
    $pythonExe = Join-Path $PythonRuntimeDir 'python.exe'
    if (-not (Test-Path -LiteralPath $pythonExe -PathType Leaf)) { throw "Python runtime not found: $pythonExe" }
    if (Test-Path -LiteralPath (Join-Path $PythonRuntimeDir 'pyvenv.cfg')) { throw 'A virtual environment cannot be relocated. Supply an embedded Python runtime.' }
    & $pythonExe -B -I -c 'import sys, psycopg, psycopg_binary; assert sys.version_info >= (3, 10); assert sys.maxsize > 2**32'
    if ($LASTEXITCODE -ne 0) { throw 'Python runtime must contain Python 3.10+ x64 and psycopg[binary].' }
}
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
Copy-VCRuntime -Destination $binOut -RuntimeDir $VCRuntimeDir | Out-Null
if ($IncludeSymbols) {
    Get-ChildItem -LiteralPath $bin -Filter '*.pdb' -File -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $binOut $_.Name) -Force
    }
}

$datasetSource = if ($DatasetDir) {
    if ([System.IO.Path]::IsPathRooted($DatasetDir)) { Resolve-FullPath $DatasetDir }
    else { Resolve-FullPath (Join-Path $ProjectRoot $DatasetDir) }
} else { Join-Path $ProjectRoot 'runtime\dataset' }
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
    if (-not $PgBinDir) {
        $PgBinDir = Join-Path $ProjectRoot 'third_party\pgsql\bin'
        if (-not (Test-Path $PgBinDir)) { $PgBinDir = Join-Path $ProjectRoot 'runtime\pgsql\bin' }
    }
    $pgSource = if ([System.IO.Path]::IsPathRooted($PgBinDir)) { Resolve-FullPath $PgBinDir }
        else { Resolve-FullPath (Join-Path $ProjectRoot $PgBinDir) }
    $postgresBinOut = Join-Path $OutDir 'postgres\bin'
    Copy-DirectoryContents $pgSource $postgresBinOut
    # PostgreSQL executables are launched from their own bin directory. Copy
    # the same VC++ runtime used by the scheduler there as well, so an
    # otherwise clean Windows x64 host does not need a global VC++ install.
    foreach ($runtimeName in @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')) {
        $runtimeSource = Join-Path $binOut $runtimeName
        if (Test-Path -LiteralPath $runtimeSource -PathType Leaf) {
            Copy-Item -LiteralPath $runtimeSource -Destination (Join-Path $postgresBinOut $runtimeName) -Force
        }
    }
    # initdb/postgres need their share and lib trees, not only executables.
    foreach ($name in @('share', 'lib')) {
        Copy-DirectoryContents (Join-Path (Split-Path -Parent $pgSource) $name) (Join-Path $OutDir "postgres\$name")
    }
    Copy-FileChecked (Join-Path $PSScriptRoot 'init_postgres_offline.ps1') (Join-Path $toolsOut 'init_postgres_offline.ps1')
}

if ($PythonRuntimeDir) {
    Copy-DirectoryContents $PythonRuntimeDir (Join-Path $OutDir 'runtime\python')
}

# Bundle the read-only result/API sidecar and its static UI. Python and psycopg
# are included by default. Only an explicit -UseSystemPython relies on the host.
$resultSource = Join-Path $ProjectRoot 'result_service'
if (Test-Path -LiteralPath (Join-Path $resultSource 'launcher.py')) {
    Copy-DirectoryContents $resultSource $resultOut
    # Python caches are generated by local validation and are not portable
    # release inputs. Remove them from the owned package output before hashing.
    Get-ChildItem -LiteralPath $resultOut -Directory -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in @('__pycache__', '.pytest_cache') } |
        Sort-Object FullName -Descending |
        Remove-Item -Recurse -Force
    # Windows PowerShell 5 treats -Include unexpectedly when combined with
    # -LiteralPath and -Recurse, which can select every file. Filter by the
    # actual extension so release inputs such as launcher.py and index.html
    # are never removed.
    Get-ChildItem -LiteralPath $resultOut -File -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @('.pyc', '.pyo') } |
        Remove-Item -Force
    $resultConfig = [ordered]@{
        host = '127.0.0.1'
        port = 56330
        db_conn = $ResultDbConn
        stp_result_table = $StpResultTable
        static_dir = '../result_service/web'
        workspace = '..'
        providers = [ordered]@{
            codex = [ordered]@{ command = 'codex'; args = @('exec', '--json', '--skip-git-repo-check', '-'); timeout_seconds = 120 }
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
    $bundledPython = Join-Path $root 'runtime\python\python.exe'
    $python = if (Test-Path -LiteralPath $bundledPython -PathType Leaf) { $bundledPython }
        elseif ($env:PYTHON) { $env:PYTHON } else { 'python' }
    $resultLog = Join-Path $root 'data\logs\result_service.log'
    $resultProc = Start-Process -FilePath $python -ArgumentList @('-B', ('"' + $resultLauncher + '"'), '--config', ('"' + $resultCfg + '"')) -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput $resultLog -RedirectStandardError (Join-Path $root 'data\logs\result_service.err.log') -PassThru
    [System.IO.File]::WriteAllText($resultPidFile, [string]$resultProc.Id)
    Write-Host "result service started (pid $($resultProc.Id))"
}
# Keep scheduler attached to this console so its stdin command protocol remains
# usable. The process is still tracked for health/stop scripts.
$proc = Start-Process -FilePath $exe -ArgumentList ('"' + $cfg + '"') -WorkingDirectory $root -NoNewWindow -PassThru
[System.IO.File]::WriteAllText($pidFile, [string]$proc.Id)
Write-Host "scheduler started (pid $($proc.Id)); type help for commands"
try { Wait-Process -Id $proc.Id } finally {
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
    if ($resultProc) {
        & taskkill.exe /PID $resultProc.Id /T /F 2>$null | Out-Null
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
$bundledPython = Join-Path $root 'runtime\python\python.exe'
$python = if (Test-Path -LiteralPath $bundledPython -PathType Leaf) { $bundledPython }
    elseif ($env:PYTHON) { $env:PYTHON } else { 'python' }
$launcher = Join-Path $root 'result_service\launcher.py'
$cfg = Join-Path $root 'config\result_service.json'
$log = Join-Path $root 'data\logs\result_service.log'
if (-not (Test-Path $launcher)) { throw "result service launcher not found: $launcher" }
if (-not (Test-Path $cfg)) { throw "result service config not found: $cfg" }
$proc = Start-Process -FilePath $python -ArgumentList @('-B', ('"' + $launcher + '"'), '--config', ('"' + $cfg + '"')) -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError (Join-Path $root 'data\logs\result_service.err.log') -PassThru
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
    & taskkill.exe /PID $pidValue /T /F 2>$null | Out-Null
    Wait-Process -Id $pidValue -Timeout $TimeoutSeconds -ErrorAction SilentlyContinue
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

$packageReadme = @'
# TestTracking server (Windows x64)

Run start_server.bat from any directory. The scripts use this package as the
working directory. Release C++ runtime DLLs and (by default) Python + psycopg are
included. Do not copy only bin: config, result_service, runtime, tools and data
belong to the same package.

Before starting:
- Set database connections in config/scheduler.json and config/result_service.json.
  Use a reader database role for the result service. Its DB connection is not sent to AI.
- Provide data/dataset/manifest.txt, matching the dataset and versions on clients.
- Set stp_result_table in config/result_service.json to the table used by that plugin.

UI: http://127.0.0.1:56330/ (host/port are configurable).
To run only the UI/AI: scripts/start_result_service.ps1; stop with
scripts/stop_result_service.ps1. AI works without a database or a selected run.
Install/login to Codex or DSH as the account running this service. Local CLI
settings and permissions apply; provider commands/cwd are configurable. Codex
conversations resume across page refreshes and service restarts. The DSH native
tab starts and embeds the installed DSH Web UI on demand. With
-UseSystemPython, the target machine must provide Python 3.10+ x64 and
psycopg[binary] on its import path.

If built with -IncludePostgresRuntime, initialize a local database explicitly
from the package root:
  Set-Location <server-package>
  powershell -File .\tools\init_postgres_offline.ps1 -PgRoot .\postgres -PgData .\data\pgdata -NoService -NoFirewall -ClientCidr 127.0.0.1/32
Adjust database name/user/port and network access for your deployment. To stop it:
  postgres/bin/pg_ctl.exe -D data/pgdata -m fast stop
No developer database is shipped. Back up data/pgdata before upgrades; do not
overwrite an active deployment directory with a newly built package.

Post-processing and maintenance scripts are in tools; PostgreSQL tools in a
bundled postgres/bin are discovered automatically. release-manifest.json lists
the packaged files and hashes. Generated logs, PID files and database data are
runtime state, not release inputs.
'@
Write-TextFile (Join-Path $OutDir 'README.md') $packageReadme

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
    target_platform = 'windows-x64'
    python_bundled = [bool]$PythonRuntimeDir
    postgres_bundled = [bool]$IncludePostgresRuntime
    files = @($files | Sort-Object path)
}
Write-TextFile (Join-Path $OutDir 'release-manifest.json') ($manifestObject | ConvertTo-Json -Depth 8)
Write-Host "Server package created: $OutDir"
Write-Host "Binary source: $bin"
Write-Host "Files: $($files.Count); manifest: $(Join-Path $OutDir 'release-manifest.json')"
