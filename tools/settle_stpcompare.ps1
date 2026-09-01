# Post-processes a stpcompare run. The server only dispatches tasks; each
# client writes per-file info.txt / info_new.txt into the configured data table
# (the client config key "table"). This script reads those rows for a run and
# produces a comparison report: how many files differ between the two outputs
# and which ones. Run it manually after a run finishes.
param(
    [string]$RunId = '',
    [string]$Table = 'stp_compare_old',
    [string]$DbConn = 'host=127.0.0.1 port=5432 dbname=testtracking user=postgres',
    [string]$Psql = 'C:\WorkSpace\ThirdParty\pgsql\bin\psql.exe',
    [string]$OutFile = 'stpcompare_report.txt'
)
$ErrorActionPreference = 'Stop'

if (-not $RunId) {
    $RunId = (& $Psql $DbConn -c "SELECT run_id FROM stp_compare_checkpoints ORDER BY updated_at DESC LIMIT 1;" -t -A 2>$null).Trim()
}
if (-not $RunId) {
    throw 'No stpcompare run found; pass -RunId <run_id>'
}

$total = (& $Psql $DbConn -t -A -c "SELECT count(*) FROM $Table WHERE run_id='$RunId';" 2>$null).Trim()
if (-not $total -or $total -eq '0') {
    Write-Host "no results for run=$RunId in table $Table"
    exit 0
}
$different = (& $Psql $DbConn -t -A -c "SELECT count(*) FROM $Table WHERE run_id='$RunId' AND info_txt IS DISTINCT FROM info_new_txt;" 2>$null).Trim()
$same = [int]$total - [int]$different

Write-Host "stpcompare run=$RunId table=$Table total=$total different=$different same=$same"

# Report the differing files.
$diffFiles = & $Psql $DbConn -t -A -c "SELECT file FROM $Table WHERE run_id='$RunId' AND info_txt IS DISTINCT FROM info_new_txt ORDER BY file;" 2>$null
if ($diffFiles) {
    $diffFiles | Set-Content -Path $OutFile -Encoding UTF8
    Write-Host "differing files written: $OutFile"
} else {
    Write-Host "no differing files"
}

# A detection-heavy run: flag if any file is missing one of the two outputs.
$missing = (& $Psql $DbConn -t -A -c "SELECT count(*) FROM $Table WHERE run_id='$RunId' AND (info_txt IS NULL OR info_new_txt IS NULL);" 2>$null).Trim()
if ($missing -and [int]$missing -gt 0) {
    Write-Host "WARN: $missing rows with a missing output"
}
