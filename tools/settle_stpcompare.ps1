# Compare stpcompare outputs using durable run metadata and report incomplete files.
param([string]$RunId='', [string]$Table='stp_compare_old', [string]$DbConn='host=127.0.0.1 port=5432 dbname=testtracking user=postgres', [string]$Psql='', [string]$OutFile='stpcompare_report.txt')
$ErrorActionPreference='Stop'; . (Join-Path $PSScriptRoot 'ToolSupport.ps1'); $Psql=Resolve-ProjectPsql $Psql
if($Table -notmatch '^[a-zA-Z_][a-zA-Z0-9_]*$'){throw 'Table must be a plain PostgreSQL table identifier'}
if(-not $RunId){$RunId=((Invoke-ProjectSql $Psql $DbConn "SELECT run_id FROM plugin_runs WHERE plugin_type='stpcompare' AND status='finished' ORDER BY finished_at DESC,run_id DESC LIMIT 1;") -join "`n").Trim()}
if(-not $RunId){throw 'No completed stpcompare run found; pass -RunId'}
$sql=@'
WITH run AS (
 SELECT expected_files,status FROM plugin_runs WHERE run_id=:'run_id' AND plugin_type='stpcompare'
), files AS (
 SELECT f.file,o.status,o.detail,
 CASE WHEN o.status IS DISTINCT FROM 'success' OR r.file IS NULL OR r.info_txt IS NULL OR r.info_new_txt IS NULL THEN 'incomplete'
      WHEN r.info_txt IS DISTINCT FROM r.info_new_txt THEN 'different' ELSE 'same' END AS comparison
 FROM plugin_run_files f
 LEFT JOIN plugin_file_outcomes o ON (o.run_id,o.plugin_type,o.file)=(f.run_id,f.plugin_type,f.file)
 LEFT JOIN :"result_table" r ON (r.run_id,r.file)=(f.run_id,f.file)
 WHERE f.run_id=:'run_id' AND f.plugin_type='stpcompare'
)
SELECT json_build_object('run',(SELECT row_to_json(run) FROM run),
 'files',COALESCE((SELECT json_agg(files ORDER BY file) FROM files),'[]'::json));
'@
$json=(@(Invoke-ProjectSql $Psql $DbConn $sql @{run_id=$RunId;result_table=$Table}) -join "`n")|ConvertFrom-Json
if(-not $json.run){throw "No durable manifest for run=$RunId; legacy runs must be restored before comparison"}
$files=@($json.files)
$missingManifest=[long]$json.run.expected_files-$files.Count
$incomplete=@($files|Where-Object {$_.comparison -eq 'incomplete'})
$different=@($files|Where-Object {$_.comparison -eq 'different'})
$same=@($files|Where-Object {$_.comparison -eq 'same'})
$lines=@("run=$RunId table=$Table expected=$($json.run.expected_files) same=$($same.Count) different=$($different.Count) incomplete=$($incomplete.Count) missing_manifest=$missingManifest")
$lines+=@($different|ForEach-Object {"different`t$($_.file)"})
$lines+=@($incomplete|ForEach-Object {"incomplete`t$($_.file)`t$($_.status)`t$($_.detail)"})
Write-AtomicText $OutFile (($lines -join "`n")+"`n")
Write-Host $lines[0]
Write-Host "comparison report written: $OutFile"
if($json.run.status -ne 'finished' -or $missingManifest -ne 0 -or $incomplete.Count -gt 0){throw 'Comparison is incomplete; the report includes missing/failed files'}
