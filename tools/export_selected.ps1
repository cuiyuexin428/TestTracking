# Export persisted greedy STP selection with atomic output publication.
param([string]$RunId='',[string]$OutFile='selected.txt',[string]$DbConn='host=127.0.0.1 port=5432 dbname=testtracking user=postgres',[string]$Psql='')
$ErrorActionPreference='Stop'; . (Join-Path $PSScriptRoot 'ToolSupport.ps1'); $Psql=Resolve-ProjectPsql $Psql
if(-not $RunId){$RunId=((Invoke-ProjectSql $Psql $DbConn 'SELECT run_id FROM coverage_run_summary ORDER BY finished_at DESC, run_id DESC LIMIT 1;') -join "`n").Trim()}
if(-not $RunId){throw 'No selection found; pass -RunId'}
$summary=@(Invoke-ProjectSql $Psql $DbConn "SELECT total_files,selected_files,cumulative_points FROM coverage_run_summary WHERE run_id=:'run_id';" @{run_id=$RunId})
if($summary.Count -ne 1){throw "No completed selection for run=$RunId"}
$files=@(Invoke-ProjectSql $Psql $DbConn "SELECT file FROM coverage_selected WHERE run_id=:'run_id' ORDER BY file;" @{run_id=$RunId})
Write-AtomicText $OutFile (($files -join "`n") + $(if($files.Count){"`n"}else{''}))
Write-Host "run=$RunId summary=$summary; selected files written: $OutFile ($($files.Count))"
