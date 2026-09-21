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
    [string]$ProjectRoot = '',
    [string]$BinDir = '',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [string]$OutDir = '',
    [string]$DbConn = 'host=127.0.0.1 port=5432 dbname=testtracking user=postgres',
    [string]$ResultDbConn = '',
    [string]$StpResultTable = '',
    [int]$ServerPort = 5555,
    [ValidateRange(1, 65535)][int]$ManagementPort = 5556,
    [string]$DatasetDir = '',
    [string]$DatasetVersion = '',
    [string]$ProgramVersion = '',
    [string]$BaselineProgramVersion = '',
    [string]$CandidateProgramVersion = '',
    [string]$PackageVersion = '0.1.0',
    [switch]$SkipDataset,
    [switch]$IncludeSymbols,
    [switch]$IncludePostgresRuntime,
    [string]$PgBinDir = '',
    # Optional embeddable CPython directory. When supplied it is copied to
    # runtime/python so the package does not depend on Python being on PATH.
    [string]$PythonRuntimeDir = '',
    # Optional wheelhouse prepared by prepare_python_analysis_wheelhouse.py.
    # When present, it is carried in the release and installed into the
    # package-local embedded Python before the release manifest is generated.
    [string]$AnalysisWheelhouseDir = '',
    [switch]$UseSystemPython,
    [string]$VCRuntimeDir = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageSupport.ps1')
$ProjectRoot = if ($ProjectRoot) {
    [System.IO.Path]::GetFullPath($ProjectRoot)
} else {
    Split-Path -Parent $PSScriptRoot
}
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

function Rewrite-SchedulerConfig([string]$Template, [string]$Destination, [string]$Connection, [int]$Port, [string]$ManifestPath, [string]$ProgramVer, [string]$BaselineProgramVer, [string]$CandidateProgramVer, [string]$DatasetVer) {
    if (-not (Test-Path -LiteralPath $Template -PathType Leaf)) { throw "scheduler template not found: $Template" }
    $config = Get-Content -LiteralPath $Template -Raw | ConvertFrom-Json
    if (-not $config.server) { $config | Add-Member -NotePropertyName server -NotePropertyValue ([pscustomobject]@{}) }
    $config.server.db_conn = $Connection
    $config.server.router_endpoint = "tcp://*:$Port"
    $config.server | Add-Member -NotePropertyName log_file -NotePropertyValue 'data/logs/scheduler.log' -Force
    $config.server | Add-Member -NotePropertyName management_endpoint -NotePropertyValue "http://127.0.0.1:$ManagementPort" -Force
    foreach ($pluginName in @('coverage', 'stpcompare')) {
        $plugin = $config.plugins.$pluginName
        if ($null -eq $plugin) { continue }
        if ($plugin.dll) { $plugin.dll = ('bin/' + [System.IO.Path]::GetFileName([string]$plugin.dll)) }
        $plugin.stp_manifest = $ManifestPath
        $plugin.db_conn = $Connection
        if ($pluginName -eq 'coverage' -and $ProgramVer) {
            $plugin | Add-Member -NotePropertyName expected_program_version -NotePropertyValue $ProgramVer -Force
        } elseif ($pluginName -eq 'stpcompare') {
            $plugin.PSObject.Properties.Remove('expected_program_version')
            if ($BaselineProgramVer) { $plugin | Add-Member -NotePropertyName expected_baseline_program_version -NotePropertyValue $BaselineProgramVer -Force }
            if ($CandidateProgramVer) { $plugin | Add-Member -NotePropertyName expected_candidate_program_version -NotePropertyValue $CandidateProgramVer -Force }
        }
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
$AnalysisWheelhouseDir = if ($AnalysisWheelhouseDir) {
    if ([System.IO.Path]::IsPathRooted($AnalysisWheelhouseDir)) {
        Resolve-FullPath $AnalysisWheelhouseDir
    } else {
        Resolve-FullPath (Join-Path $ProjectRoot $AnalysisWheelhouseDir)
    }
} elseif (-not $UseSystemPython -and (Test-Path -LiteralPath (Join-Path $ProjectRoot 'runtime\python-analysis-wheelhouse') -PathType Container)) {
    Join-Path $ProjectRoot 'runtime\python-analysis-wheelhouse'
} else { '' }
if ($AnalysisWheelhouseDir -and $UseSystemPython) {
    throw 'AnalysisWheelhouseDir requires a package-local Python runtime; remove -UseSystemPython.'
}
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
$bin = Resolve-PackageBinaryDir -ProjectRoot $ProjectRoot -Explicit $BinDir -Executable 'scheduler.exe' -Config $Config

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
$docsOut = Join-Path $OutDir 'docs'
foreach ($dir in @($binOut, $cfgOut, $toolsOut, $scriptsOut, $resultOut, (Join-Path $dataOut 'dataset'), (Join-Path $dataOut 'artifacts'), (Join-Path $dataOut 'artifact_import'), (Join-Path $dataOut 'reports'), (Join-Path $dataOut 'logs'))) {
    Ensure-Directory $dir
}
foreach ($name in @('自动部署使用指南.md', '服务端插件包格式.md', '已知限制.md')) {
    Copy-FileChecked (Join-Path $ProjectRoot ('docs/' + $name)) (Join-Path $docsOut $name)
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
Rewrite-SchedulerConfig -Template $template -Destination (Join-Path $cfgOut 'scheduler.json') `
    -Connection $DbConn -Port $ServerPort -ManifestPath $manifestRelative `
    -ProgramVer $ProgramVersion -BaselineProgramVer $BaselineProgramVersion `
    -CandidateProgramVer $CandidateProgramVersion -DatasetVer $DatasetVersion
# Editable deployment template only; changing it cannot modify remote clients.
$clientTemplate = Get-Content -LiteralPath (Join-Path $ProjectRoot 'client\client.json') -Raw | ConvertFrom-Json
$clientTemplate.scheduler.endpoint = "tcp://127.0.0.1:$ServerPort"
if ($clientTemplate.plugins.stpcompare.programs) {
    if ($BaselineProgramVersion) { $clientTemplate.plugins.stpcompare.programs.baseline.version = $BaselineProgramVersion }
    if ($CandidateProgramVersion) { $clientTemplate.plugins.stpcompare.programs.candidate.version = $CandidateProgramVersion }
}
foreach ($property in $clientTemplate.plugins.PSObject.Properties) {
    if ($property.Value.PSObject.Properties.Name -contains 'db_conn') { $property.Value.db_conn = $DbConn }
    if ($property.Value.PSObject.Properties.Name -contains 'dry_run') { $property.Value.dry_run = $false }
}
Write-TextFile (Join-Path $cfgOut 'client.template.json') ($clientTemplate | ConvertTo-Json -Depth 20)

# Settlement and maintenance scripts are part of the server package. The
# package-local ToolSupport is required by all three scripts.
foreach ($name in @('ToolSupport.ps1', 'settle_coverage.ps1', 'settle_stpcompare.ps1', 'prune_old_runs.ps1', 'build_plugin_bundle.py')) {
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
if ($AnalysisWheelhouseDir) {
    foreach ($name in @('requirements.txt', 'pip.pyz', 'wheelhouse-manifest.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $AnalysisWheelhouseDir $name) -PathType Leaf)) {
            throw "Analysis wheelhouse is incomplete; missing $name under $AnalysisWheelhouseDir"
        }
    }
    $analysisWheelhouseOut = Join-Path $OutDir 'runtime\python-analysis-wheelhouse'
    Copy-DirectoryContents $AnalysisWheelhouseDir $analysisWheelhouseOut
    foreach ($name in @('install_python_analysis_offline.ps1', 'python_analysis_support.py',
                        'setup_offline_dependencies.ps1')) {
        Copy-FileChecked (Join-Path $PSScriptRoot $name) (Join-Path $toolsOut $name)
    }
    $analysisInstaller = Join-Path $toolsOut 'install_python_analysis_offline.ps1'
    & $analysisInstaller `
        -RuntimeDir (Join-Path $OutDir 'runtime\python') `
        -Wheelhouse $analysisWheelhouseOut `
        -SupportScript (Join-Path $toolsOut 'python_analysis_support.py')
    if ($LASTEXITCODE -ne 0) { throw 'Installing the package-local Python analysis dependencies failed.' }
}

# Bundle the read-only result/API sidecar and its static UI. Python and psycopg
# are included by default. Only an explicit -UseSystemPython relies on the host.
$resultSource = Join-Path $ProjectRoot 'result_service'
if (Test-Path -LiteralPath (Join-Path $resultSource 'launcher.py')) {
    Copy-DirectoryContents $resultSource $resultOut
    $resultViewsOut = Join-Path $dataOut 'result_views'
    $resultAdaptersOut = Join-Path $resultOut 'plugin_adapters'
    $pluginAiContextsOut = Join-Path $resultOut 'plugin_ai_contexts'
    $pluginResources = [ordered]@{}
    foreach ($pluginDir in Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'plugins') -Directory) {
        $viewDefinition = Join-Path $pluginDir.FullName 'result_view.json'
        # Execution plugins need not provide analysis resources. Only result
        # plugins declare a view; their adapter and report remain optional.
        if (-not (Test-Path -LiteralPath $viewDefinition -PathType Leaf)) { continue }
        Copy-FileChecked $viewDefinition (Join-Path $resultViewsOut ($pluginDir.Name + '.json'))
        $resources = @('view')
        $aiContext = Join-Path $pluginDir.FullName 'AI_CONTEXT.md'
        if (-not (Test-Path -LiteralPath $aiContext -PathType Leaf)) {
            throw "Result plugin $($pluginDir.Name) is missing AI_CONTEXT.md"
        }
        Copy-FileChecked $aiContext (Join-Path $pluginAiContextsOut ($pluginDir.Name + '.md'))
        $resources += 'ai_context'
        $resultAdapter = Join-Path $pluginDir.FullName 'result_adapter.py'
        if (Test-Path -LiteralPath $resultAdapter -PathType Leaf) {
            Copy-FileChecked $resultAdapter (Join-Path $resultAdaptersOut ($pluginDir.Name + '.py'))
            $resources += 'adapter'
        }
        $reportTemplate = Join-Path $pluginDir.FullName 'result_report.py'
        if (Test-Path -LiteralPath $reportTemplate -PathType Leaf) {
            Copy-FileChecked $reportTemplate (Join-Path $resultOut ('report_templates/' + $pluginDir.Name + '.py'))
            $resources += 'report_template'
        }
        $pluginResources[$pluginDir.Name] = $resources
    }
    if ($pluginResources.Count -eq 0) { throw 'No plugin result views were found for the server package.' }
    # This JSON document deliberately has no .json suffix: it is an installed
    # resource declaration, not another plugin view discovered by the UI.
    Write-TextFile (Join-Path $resultViewsOut 'resources.manifest') `
        (@{ schema_version = 1; plugins = $pluginResources } | ConvertTo-Json -Depth 5)
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
        host = '0.0.0.0'
        port = 56330
        db_conn = $ResultDbConn
        scheduler_management_url = "http://127.0.0.1:$ManagementPort"
        scheduler_config_path = 'scheduler.json'
        client_config_path = 'client.template.json'
        logs_dir = '../data/logs'
        plugin_options = [ordered]@{
            stpcompare = [ordered]@{ result_table = $StpResultTable }
        }
        static_dir = '../result_service/web'
        plugin_views_dir = '../data/result_views'
        plugin_adapters_dir = '../result_service/plugin_adapters'
        plugin_ai_contexts_dir = '../result_service/plugin_ai_contexts'
        analysis_scripts_dir = '../data/analysis_scripts'
        analysis_reports_dir = '../data/reports'
        report_templates_dir = '../result_service/report_templates'
        artifact_store_dir = '../data/artifacts'
        artifact_import_roots = @('../data/artifact_import')
        workspace = '..'
        providers = [ordered]@{
            dsh = [ordered]@{ command = 'dsh'; timeout_seconds = 120 }
        }
    }
    Write-TextFile (Join-Path $cfgOut 'result_service.json') ($resultConfig | ConvertTo-Json -Depth 8)
    Copy-FileChecked (Join-Path $PSScriptRoot 'open_result_service_firewall.ps1') (Join-Path $toolsOut 'open_result_service_firewall.ps1')
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
$resultStart = Join-Path $root 'scripts\start_result_service.ps1'
if (Test-Path -LiteralPath $resultStart -PathType Leaf) {
    try { & $resultStart -NoWait }
    catch { Write-Warning "result service did not start; scheduler will continue: $_" }
}
if (Test-Path $pidFile) {
    $oldPid = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
    $old = Get-Process -Id $oldPid -ErrorAction SilentlyContinue
    if ($old -and $old.ProcessName -eq 'scheduler') { Write-Host "scheduler already running (pid $oldPid)"; exit 0 }
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
}
# Keep scheduler attached to this console so its stdin command protocol remains
# usable. The process is still tracked for health/stop scripts.
$proc = Start-Process -FilePath $exe -ArgumentList ('"' + $cfg + '"') -WorkingDirectory $root -NoNewWindow -PassThru
[System.IO.File]::WriteAllText($pidFile, [string]$proc.Id)
Write-Host "scheduler started (pid $($proc.Id)); type help for commands"
try { Wait-Process -Id $proc.Id } finally {
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
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
$errorLog = Join-Path $root 'data\logs\result_service.err.log'
$pidFile = Join-Path $root 'data\result_service.pid'
if (-not (Test-Path $launcher)) { throw "result service launcher not found: $launcher" }
if (-not (Test-Path $cfg)) { throw "result service config not found: $cfg" }
$serviceConfig = Get-Content -LiteralPath $cfg -Raw -Encoding UTF8 | ConvertFrom-Json
$listenHost = if ($env:RESULT_SERVICE_HOST) { [string]$env:RESULT_SERVICE_HOST } else { [string]$serviceConfig.host }
$listenPort = if ($env:RESULT_SERVICE_PORT) { [int]$env:RESULT_SERVICE_PORT } else { [int]$serviceConfig.port }
if ($listenPort -lt 1 -or $listenPort -gt 65535) { throw "invalid result service port: $listenPort" }
$wildcardHosts = @('', '*', '0.0.0.0')
$probeHost = if ($listenHost -in $wildcardHosts) { '127.0.0.1' } else { $listenHost }
$probeAuthority = if ($probeHost.Contains(':') -and -not $probeHost.StartsWith('[')) { "[$probeHost]" } else { $probeHost }
$healthUrl = "http://${probeAuthority}:$listenPort/healthz"
function Test-ResultServiceHealth {
    try {
        $request = [System.Net.WebRequest]::Create($healthUrl)
        $request.Proxy = $null
        $request.Timeout = 1000
        $response = $request.GetResponse()
        try { return [int]$response.StatusCode -eq 200 } finally { $response.Close() }
    } catch { return $false }
}
function Write-ResultServiceUrls {
    if ($listenHost -in $wildcardHosts) {
        $addresses = @()
        try {
            $addresses = @(Get-NetIPAddress -AddressFamily IPv4 -AddressState Preferred -ErrorAction Stop |
                Where-Object { $_.IPAddress -ne '127.0.0.1' -and -not $_.IPAddress.StartsWith('169.254.') } |
                Select-Object -ExpandProperty IPAddress -Unique)
        } catch {
            $addresses = @([System.Net.Dns]::GetHostAddresses([System.Net.Dns]::GetHostName()) |
                Where-Object { $_.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetwork -and
                    -not [System.Net.IPAddress]::IsLoopback($_) } |
                ForEach-Object { $_.ToString() } |
                Select-Object -Unique)
        }
        if ($addresses.Count -eq 0) {
            Write-Host "UI (LAN): http://<server-LAN-IP>:$listenPort/"
        } else {
            foreach ($address in $addresses) { Write-Host "UI (LAN): http://${address}:$listenPort/" }
        }
    } else {
        $authority = if ($listenHost.Contains(':') -and -not $listenHost.StartsWith('[')) { "[$listenHost]" } else { $listenHost }
        Write-Host "UI: http://${authority}:$listenPort/"
        if ($listenHost -in @('127.0.0.1', 'localhost', '::1')) {
            Write-Warning 'The result service is listening on loopback only; remote browsers cannot connect.'
        }
    }
    Write-Host "If another LAN machine cannot connect, run tools\open_result_service_firewall.ps1 as administrator."
}
if (Test-Path -LiteralPath $pidFile -PathType Leaf) {
    $oldPid = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
    $old = Get-CimInstance Win32_Process -Filter "ProcessId = $oldPid" -ErrorAction SilentlyContinue
    $oldCommand = if ($old) { [string]$old.CommandLine } else { '' }
    if ($old -and $old.Name -match '^python(?:w)?\.exe$' -and
            $oldCommand.IndexOf($launcher, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        if (-not (Test-ResultServiceHealth)) {
            throw "result service process $oldPid exists but $healthUrl is not healthy; inspect $errorLog"
        }
        Write-Host "result service already running (pid $oldPid)"
        Write-ResultServiceUrls
        return
    }
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
}
$proc = Start-Process -FilePath $python -ArgumentList @('-B', ('"' + $launcher + '"'), '--config', ('"' + $cfg + '"')) -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError $errorLog -PassThru
[System.IO.File]::WriteAllText($pidFile, [string]$proc.Id)
$healthy = $false
for ($attempt = 0; $attempt -lt 60; $attempt++) {
    Start-Sleep -Milliseconds 250
    $proc.Refresh()
    if ($proc.HasExited) { break }
    if (Test-ResultServiceHealth) { $healthy = $true; break }
}
if (-not $healthy) {
    if (-not $proc.HasExited) { & taskkill.exe /PID $proc.Id /T /F 2>$null | Out-Null }
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
    $details = if (Test-Path -LiteralPath $errorLog -PathType Leaf) {
        ((Get-Content -LiteralPath $errorLog -Tail 10 -ErrorAction SilentlyContinue) -join "`n").Trim()
    } else { '' }
    if ($details) { throw "result service failed health check at $healthUrl`n$details" }
    throw "result service failed health check at $healthUrl; inspect $errorLog"
}
Write-Host "result service ready (pid $($proc.Id)); logs: $log"
Write-ResultServiceUrls
if (-not $NoWait) {
    try { Wait-Process -Id $proc.Id }
    finally {
        if ((Test-Path -LiteralPath $pidFile -PathType Leaf) -and
                ([int](Get-Content -LiteralPath $pidFile -Raw).Trim()) -eq $proc.Id) {
            Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
        }
    }
}
'@
Write-TextFile (Join-Path $scriptsOut 'start_result_service.ps1') $resultStartScript

$resultStopScript = @'
[CmdletBinding()]
param([int]$TimeoutSeconds = 15)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$pidFile = Join-Path $root 'data\result_service.pid'
$launcher = Join-Path $root 'result_service\launcher.py'
if (-not (Test-Path $pidFile)) { Write-Host 'result service is not running'; exit 0 }
$pidValue = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
$proc = Get-Process -Id $pidValue -ErrorAction SilentlyContinue
if ($proc) {
    $details = Get-CimInstance Win32_Process -Filter "ProcessId = $pidValue" -ErrorAction SilentlyContinue
    $commandLine = if ($details) { [string]$details.CommandLine } else { '' }
    if (-not $details -or $details.Name -notmatch '^python(?:w)?\.exe$' -or
            $commandLine.IndexOf($launcher, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "PID file does not refer to this result service: $pidValue"
    }
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

The browser console includes live clients, task run/stop/wakeup, bounded log
viewing and editable JSON configuration. The scheduler management endpoint is
local-only (default 127.0.0.1:5556); remote browsers use the main UI port.
Configuration saves are validated and backed up; restart the affected process
to apply changes. config/client.template.json is a deployment template: copy
its settings into the intended client package and restart that client. Saving
the template does not reconfigure connected clients. Change loopback addresses
to the actual server LAN address when deploying the template to another machine.
Templates use dry_run=false: configure the real business executables and the full
dataset before starting a client. Use dry_run=true only for an explicit trial.
Analysis and saved reports remain available when the scheduler is offline.

The client deployment wizard selects uploaded plugin, program and dataset
artifacts, previews the final configuration and publishes to one client at a time.
Set client.deployment_endpoint=http://<server-LAN-IP>:56330 in the client base
configuration and give each client a unique worker_id. The client Agent pulls
artifacts over HTTP, checks SHA-256, then activates an immutable configuration
after current tasks finish and the update window permits it. Execution and update
windows are independent. Each client currently enables exactly one plugin.
For built-in program artifacts, preview derives program identities from their
content and execution settings. Save the matching scheduler configuration and
restart the scheduler before submitting tasks with the new identities.
Uploading a dataset does not update the server manifest or expected dataset
version; keep these consistent with the complete local data on each client.

Server plugin ZIPs containing server_plugin.json can be previewed and installed
with their analysis resources and AI_CONTEXT.md. Installation uses independent
directories and recoverable configuration writes. New plugins start disabled;
configure and enable them explicitly, then restart the scheduler and result
service. Finish or recover unfinished runs using the original deployment before
upgrading. Installation does not update framework EXEs or install Python packages.
Use compatible builds for the framework and all plugin DLLs.

Package-local instructions:
- [Deployment guide](docs/自动部署使用指南.md)
- [Server plugin package format](docs/服务端插件包格式.md)
- [Known limitations](docs/已知限制.md)

Before starting:
- Set database connections in config/scheduler.json and config/result_service.json.
  Use a reader database role for the result service. Its DB connection is not sent to AI.
- Provide data/dataset/manifest.txt, matching the dataset and versions on clients.
- Set plugin_options.stpcompare.result_table in config/result_service.json to the table
  configured in each stpcompare client package (normally stp_compare_old).

LAN clients use the scheduler port (default TCP 5555) for tasks and the database
port (default TCP 5432) for direct result writes. Browsers use the result service
port (default TCP 56330) for the UI, DSH chat and the proxied DSH Web UI.
Allow the configured ports from the client LAN in Windows Firewall. Run
tools/open_result_service_firewall.ps1 once as administrator for the UI port.
It allows LocalSubnet by default; for a routed trusted LAN, pass its CIDR with
-RemoteAddress (for example, -RemoteAddress 192.168.100.0/24).

UI: http://<server-LAN-IP>:56330/ (default host 0.0.0.0; host/port are configurable).
The service is intended for a trusted LAN and has no login. DSH HTTP and WebSocket
connections use this same port. start_server.bat starts both the scheduler and the
result service. Their lifetimes are independent: the result service remains available
if the scheduler exits or restarts. Stop it explicitly with
scripts/stop_result_service.ps1. To run only the UI/AI, use
scripts/start_result_service.ps1. AI works without a database or a selected run.
Scheduler logs append to data/logs/scheduler.log via server.log_file while its
console and stdin commands remain available. Result-service logs share data/logs.
Install and configure DSH as the account running this service. Local CLI settings
and permissions apply; the provider command/cwd is configurable. DSH conversations
resume across page refreshes and service restarts. The DSH native
tab starts and embeds the installed DSH Web UI on demand through the main service.
Remote browsers use isolated sessions backed by the server account's DSH installation. With
-UseSystemPython, the target machine must provide Python 3.10+ x64 and
psycopg[binary] on its import path.

When runtime/python-analysis-wheelhouse is present, numpy, pandas, openpyxl,
matplotlib and plotly are already installed in runtime/python and can be restored
without internet access:
  powershell -File .\tools\install_python_analysis_offline.ps1
The installer verifies all wheel hashes, exact versions and imports before writing
runtime/python/analysis-runtime-manifest.json.
To also install a separately supplied DSH offline bundle, use:
  powershell -ExecutionPolicy Bypass -File .\tools\setup_offline_dependencies.ps1 -DshBundleDir <bundle-dist> -LlmBaseUrl <LAN-endpoint/v1> -LlmModel <model-id>
The default DSH CLI installation is discovered automatically. With a custom
-DshInstallDir, set providers.dsh.command in config/result_service.json to its
dsh.cmd path. DSH connects to the configured LAN model service; installation
does not test model inference.

Each client package currently carries the complete local dataset under
testenv/dataset. The server needs the matching manifest to plan tasks. Dataset
and program versions must agree across packages; assign a unique worker_id to
each client. Initial data can be copied with the client package; dataset artifacts
can later be delivered through the deployment wizard. Clients execute against
their local data, so no shared directory is required.

Result analysis runs replaceable Python scripts against durable database results.
Default scripts: result_service/report_templates. Saved scripts: data/analysis_scripts.
Generated standalone HTML reports: data/reports. Editing or rerunning a report
does not depend on a running scheduler/client. See result_service/REPORTS.md.

If built with -IncludePostgresRuntime, initialize a local database explicitly
from an elevated PowerShell at the package root. The example trusts only the stated
LAN; replace the example CIDR with the actual client subnet:
  Set-Location <server-package>
  powershell -File .\tools\init_postgres_offline.ps1 -PgRoot .\postgres -PgData .\data\pgdata -AuthMethod trust -ClientCidr 192.168.100.0/24
This registers PostgreSQL as a Windows service and opens TCP 5432. Also allow the
configured scheduler port (default TCP 5555) from the client LAN. Adjust database
name/user/port and network access for your deployment. To stop a non-service instance:
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
    python_analysis_bundled = [bool]$AnalysisWheelhouseDir
    postgres_bundled = [bool]$IncludePostgresRuntime
    files = @($files | Sort-Object path)
}
Write-TextFile (Join-Path $OutDir 'release-manifest.json') ($manifestObject | ConvertTo-Json -Depth 8)
Write-Host "Server package created: $OutDir"
Write-Host "Binary source: $bin"
Write-Host "Files: $($files.Count); manifest: $(Join-Path $OutDir 'release-manifest.json')"
