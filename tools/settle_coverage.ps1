# Select a deterministic greedy STP cover from a completed coverage run.
# Raw results are retained. Add -MergeCov only when merged OCC reports are needed.
param(
    [string]$RunId = '',
    [string]$DbConn = 'host=127.0.0.1 port=5432 dbname=testtracking user=postgres',
    [string]$Exe = '',
    [string]$Psql = '',
    [switch]$MergeCov,
    [string]$OpenCppCoverage = 'C:/Program Files/OpenCppCoverage/OpenCppCoverage.exe',
    [string]$WorkDir = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

. (Join-Path $PSScriptRoot 'ToolSupport.ps1')

$Exe = Resolve-ProjectTool $Exe @(
    (Join-Path $PSScriptRoot 'coverage_settle.exe'),
    (Join-Path $projectRoot 'build/rt/bin/Release/coverage_settle.exe'),
    (Join-Path $projectRoot 'build/rt/bin/Debug/coverage_settle.exe'),
    (Join-Path $projectRoot 'out/build/x64-Debug/bin/Debug/coverage_settle.exe'),
    'coverage_settle.exe'
) 'coverage_settle (-Exe)'

if (-not $RunId) {
    $Psql = Resolve-ProjectPsql $Psql
    $hasMetadata = (@(Invoke-ProjectSql $Psql $DbConn "SELECT to_regclass('plugin_runs') IS NOT NULL;") -join '').Trim() -eq 't'
    if ($hasMetadata) {
        $output = Invoke-ProjectSql $Psql $DbConn "SELECT run_id FROM plugin_runs WHERE plugin_type='coverage' AND status='finished' ORDER BY finished_at DESC, run_id DESC LIMIT 1;"
    }
    if (-not $hasMetadata -or -not $output) {
        # coverage_settle validates and migrates complete v3 legacy runs.
        $output = Invoke-ProjectSql $Psql $DbConn "SELECT run_id FROM runs WHERE plugin_type='coverage' AND status='finished' ORDER BY created_at DESC, run_id DESC LIMIT 1;"
    }
    $RunId = ($output -join "`n").Trim()
}
if (-not $RunId) { throw 'No completed coverage run found; pass -RunId <run_id>' }

$argsList = @('--run', $RunId, '--db', $DbConn)
if ($MergeCov) { $argsList += @('--merge-cov', '--opencppcoverage', $OpenCppCoverage) }
if ($WorkDir) { $argsList += @('--workdir', $WorkDir) }
Invoke-NativeCommand $Exe $argsList | Out-Host
Write-Host "settled coverage run=$RunId"
