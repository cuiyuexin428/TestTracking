# Remove operational history for old terminal runs (finished / cancelled / interrupted / failed).
# Raw and long-term metadata remain. 'stopped' stays for the next start's recovery.
param([ValidateRange(0,2147483647)][int]$Keep=10,[switch]$DryRun,[string]$DbConn='host=127.0.0.1 port=5432 dbname=testtracking user=postgres',[string]$Psql='')
$ErrorActionPreference='Stop'; . (Join-Path $PSScriptRoot 'ToolSupport.ps1'); $Psql=Resolve-ProjectPsql $Psql
$ids=@(Invoke-ProjectSql $Psql $DbConn "SELECT run_id FROM runs WHERE status IN ('finished','cancelled','interrupted','failed') ORDER BY created_at DESC, run_id DESC OFFSET :keep;" @{keep=$Keep} | ? {$_.Trim()})
if($DryRun){$ids|%{Write-Host "would prune: $_"};exit 0}
foreach($runId in $ids){
  $sql=@'
BEGIN;
CREATE TEMP TABLE prune_target ON COMMIT DROP AS SELECT run_id,plugin_type FROM runs WHERE run_id=:'run_id' AND status IN ('finished','cancelled','interrupted','failed') FOR UPDATE;
DELETE FROM tasks WHERE run_id IN (SELECT run_id FROM prune_target);
DO $$ DECLARE registered_table text; BEGIN
  IF to_regclass('plugin_checkpoint_tables') IS NULL THEN
    RAISE EXCEPTION 'Checkpoint table registry missing; initialize the server plugins with the updated runtime before pruning';
  END IF;
  IF EXISTS (SELECT 1 FROM prune_target target WHERE NOT EXISTS (
      SELECT 1 FROM plugin_checkpoint_tables registered WHERE registered.plugin_type=target.plugin_type)) THEN
    RAISE EXCEPTION 'Checkpoint table not registered for this run; initialize its server plugin with the updated runtime before pruning';
  END IF;
  FOR registered_table IN SELECT DISTINCT checkpoint_table FROM plugin_checkpoint_tables
      WHERE plugin_type IN (SELECT plugin_type FROM prune_target) LOOP
    IF to_regclass(format('%I',registered_table)) IS NOT NULL THEN
      EXECUTE format('DELETE FROM %I WHERE run_id IN (SELECT run_id FROM prune_target)',registered_table);
    END IF;
  END LOOP;
  IF to_regclass('plugin_task_completions') IS NOT NULL THEN DELETE FROM plugin_task_completions WHERE run_id IN (SELECT run_id FROM prune_target); END IF;
END $$;
DELETE FROM runs WHERE run_id IN (SELECT run_id FROM prune_target);
COMMIT;
'@
  try { Invoke-ProjectSql $Psql $DbConn $sql @{run_id=$runId}|Out-Null; Write-Host "pruned operational history: $runId" } catch { throw "prune failed for $runId; no success reported: $_" }
}
Write-Host "done: $($ids.Count) terminal runs"
