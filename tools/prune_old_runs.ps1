# Deletes old runs (framework runs/tasks + coverage business rows), keeping
# the most recent -Keep runs. Use -DryRun to preview what would be removed.
param(
    [int]$Keep = 10,
    [switch]$DryRun,
    [string]$Psql = 'C:\WorkSpace\ThirdParty\pgsql\bin\psql.exe'
)
$ErrorActionPreference = 'Stop'

$runs = & $Psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -t -A -c "SELECT run_id FROM runs ORDER BY created_at DESC OFFSET $Keep;" 2>$null
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
    $sql = @"
BEGIN;
DELETE FROM coverage_wave_points WHERE run_id='$runId';
DELETE FROM coverage_cov_files WHERE run_id='$runId';
DELETE FROM coverage_cumulative WHERE run_id='$runId';
DELETE FROM coverage_selected WHERE run_id='$runId';
DELETE FROM coverage_run_summary WHERE run_id='$runId';
DELETE FROM coverage_wave_summary WHERE run_id='$runId';
DELETE FROM coverage_cumulative_cov WHERE run_id='$runId';
DELETE FROM coverage_checkpoints WHERE run_id='$runId';
DELETE FROM tasks WHERE run_id='$runId';
DELETE FROM runs WHERE run_id='$runId';
COMMIT;
"@
    & $Psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -c $sql 2>&1 | Out-Null
    Write-Host "pruned $runId"
}
Write-Host 'done'
