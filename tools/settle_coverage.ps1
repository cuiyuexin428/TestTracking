# Runs the standalone coverage post-processor for a finished run.
# The scheduler only dispatches tasks; this script performs the derived
# post-processing: minimal stp set (global greedy), cumulative cov merge, and
# the run summary. Run it manually after a run finishes.
param(
    [string]$RunId = '',
    [string]$DbConn = 'host=127.0.0.1 port=5432 dbname=testtracking user=postgres',
    [string]$Exe = 'coverage_settle.exe',
    [string]$OpenCppCoverage = 'C:/Program Files/OpenCppCoverage/OpenCppCoverage.exe',
    [string]$WorkDir = ''
)
$ErrorActionPreference = 'Stop'

if (-not $RunId) {
    $Psql = 'C:\WorkSpace\ThirdParty\pgsql\bin\psql.exe'
    $RunId = (& $Psql $DbConn -c "SELECT run_id FROM runs WHERE status='finished' ORDER BY created_at DESC LIMIT 1;" -t -A 2>$null).Trim()
}
if (-not $RunId) {
    throw 'No finished run found; pass -RunId <run_id>'
}

$argsList = @('--run', $RunId, '--db', $DbConn, '--opencppcoverage', $OpenCppCoverage)
if ($WorkDir) {
    $argsList += @('--workdir', $WorkDir)
}

& $Exe @argsList
if ($LASTEXITCODE -ne 0) {
    throw "coverage_settle failed (exit $LASTEXITCODE)"
}
Write-Host "settled run=$RunId"
