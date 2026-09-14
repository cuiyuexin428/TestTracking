# Remove operational history for old terminal runs (finished / cancelled / interrupted / failed).
# Raw and long-term metadata remain. 'stopped' stays for the next start's recovery.
param([ValidateRange(0,2147483647)][int]$Keep=10,[switch]$DryRun,[string]$DbConn='host=127.0.0.1 port=5432 dbname=testtracking user=postgres',[string]$Psql='')
$ErrorActionPreference='Stop'; . (Join-Path $PSScriptRoot 'ToolSupport.ps1'); $Psql=Resolve-ProjectPsql $Psql
$ids=@(Invoke-ProjectSql $Psql $DbConn "SELECT run_id FROM runs WHERE status IN ('finished','cancelled','interrupted','failed') ORDER BY created_at DESC, run_id DESC OFFSET :keep;" @{keep=$Keep} | ? {$_.Trim()})
if($DryRun){$ids|%{Write-Host "would prune: $_"};exit 0}
foreach($runId in $ids){
  $sql=@'
BEGIN;
CREATE TEMP TABLE prune_target ON COMMIT DROP AS SELECT run_id FROM runs WHERE run_id=:'run_id' AND status IN ('finished','cancelled','interrupted','failed') FOR UPDATE;
DELETE FROM tasks WHERE run_id IN (SELECT run_id FROM prune_target);
DO $$ BEGIN
  IF to_regclass('coverage_checkpoints') IS NOT NULL THEN DELETE FROM coverage_checkpoints WHERE run_id IN (SELECT run_id FROM prune_target); END IF;
  IF to_regclass('stp_compare_checkpoints') IS NOT NULL THEN DELETE FROM stp_compare_checkpoints WHERE run_id IN (SELECT run_id FROM prune_target); END IF;
  IF to_regclass('plugin_task_completions') IS NOT NULL THEN DELETE FROM plugin_task_completions WHERE run_id IN (SELECT run_id FROM prune_target); END IF;
END $$;
DELETE FROM runs WHERE run_id IN (SELECT run_id FROM prune_target);
COMMIT;
'@
  try { Invoke-ProjectSql $Psql $DbConn $sql @{run_id=$runId}|Out-Null; Write-Host "pruned operational history: $runId" } catch { throw "prune failed for $runId; no success reported: $_" }
}
Write-Host "done: $($ids.Count) terminal runs"
