# Exports the selected (minimal) stp file set of a run from PostgreSQL.
# With no -RunId, the most recently finished run is used.
param(
    [string]$RunId = '',
    [string]$OutFile = 'selected.txt',
    [string]$DbConn = 'host=127.0.0.1 port=5432 dbname=testtracking user=postgres',
    [string]$Psql = 'C:\WorkSpace\ThirdParty\pgsql\bin\psql.exe'
)
$ErrorActionPreference = 'Stop'

if (-not $RunId) {
    $RunId = (& $Psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -c "SELECT run_id FROM coverage_run_summary ORDER BY finished_at DESC LIMIT 1;" -t -A 2>$null).Trim()
}
if (-not $RunId) {
    throw 'No run found; pass -RunId <run_id>'
}

$summary = & $Psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -c "SELECT total_files, selected_files, cumulative_points FROM coverage_run_summary WHERE run_id='$RunId';" -t -A 2>$null
& $Psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -c "SELECT file FROM coverage_selected WHERE run_id='$RunId' ORDER BY file;" -t -A 2>$null | Set-Content -Path $OutFile -Encoding UTF8

$count = (Get-Content $OutFile | Where-Object { $_ }).Count
Write-Host "run=$RunId summary=$summary"
Write-Host "selected files written: $OutFile ($count)"
