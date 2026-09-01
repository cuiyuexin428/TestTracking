# Deletes old runs (framework runs/tasks + derived coverage rows), keeping
# the most recent -Keep runs. Raw per-file coverage rows are retained forever
# and are never pruned here. Use -DryRun to preview what would be removed.
param(
    [int]$Keep = 10,
    [switch]$DryRun,
    [string]$DbConn = 'host=127.0.0.1 port=5432 dbname=testtracking user=postgres',
    [string]$Psql = 'C:\WorkSpace\ThirdParty\pgsql\bin\psql.exe'
)
$ErrorActionPreference = 'Stop'

$runs = & $Psql $DbConn -t -A -c "SELECT run_id FROM runs ORDER BY created_at DESC OFFSET $Keep;" 2>$null
if (-not $runs) {
    Write-Host "no runs to prune (keep=$Keep)"
    exit 0
}
$ids = @($runs | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() })
Write-Host "runs to prune: $($ids.Count)"

if ($DryRun) {
    $ids | ForEach-Object { Write-Host "  $_" }
    exit 0
}

foreach ($runId in $ids) {
    # Raw per-file rows (coverage_file_points / coverage_cov_files) are
    # intentionally retained forever (per project decision), so they are NOT
    # deleted here. Only derived and framework rows are pruned.
    $sql = @"
BEGIN;
DELETE FROM coverage_selected WHERE run_id='$runId';
DELETE FROM coverage_run_summary WHERE run_id='$runId';
DELETE FROM coverage_cumulative_cov WHERE run_id='$runId';
DELETE FROM coverage_checkpoints WHERE run_id='$runId';
DELETE FROM tasks WHERE run_id='$runId';
DELETE FROM runs WHERE run_id='$runId';
COMMIT;
"@
    & $Psql $DbConn -c $sql 2>&1 | Out-Null
    Write-Host "pruned $runId (raw per-file rows retained)"
}
Write-Host 'done'
