# Regression test: after the scheduler crashes and restarts, a running client
# whose session went stale must re-register and resume pulling tasks (self-heal).
#
# Drives the real scheduler.exe / client.exe against a local PostgreSQL. The
# test is self-contained: it boots a throwaway PostgreSQL (or reuses one already
# listening on -PgPort), creates the testtracking DB, starts scheduler+client,
# starts a run, kills the scheduler mid-run, restarts it, then asserts the
# client re-registered and received a task batch afterward.
#
# Exit 0 = PASS, 1 = FAIL, 2 = skipped (build/binaries or postgres unavailable).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\test_client_self_heal.ps1 \
#       -Bin build\rt\bin\Debug [-PgPort 5433] [-KeepPg]

param(
    [string]$Bin = 'build\rt\bin\Debug',
    [int]$PgPort = 5433,
    [switch]$KeepPg
)
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path '.').Path
$bin = if ([System.IO.Path]::IsPathRooted($Bin)) { $Bin } else { Join-Path $root $Bin }
$schedulerExe = Join-Path $bin 'scheduler.exe'
$clientExe = Join-Path $bin 'client.exe'
$coverageDll = Join-Path $bin 'coverage.dll'
$pgbin = Join-Path $root 'third_party\pgsql\bin'
$pgctl = Join-Path $pgbin 'pg_ctl.exe'
$postgres = Join-Path $pgbin 'postgres.exe'
$psql = Join-Path $pgbin 'psql.exe'
$manifest = Join-Path $root 'runtime\dataset\manifest.txt'

# ---- 0. prerequisite checks --------------------------------------------
if (-not (Test-Path $schedulerExe) -or -not (Test-Path $clientExe) -or -not (Test-Path $coverageDll)) {
    Write-Host "SKIP: missing binaries (need scheduler.exe/client.exe/coverage.dll under $bin)"
    exit 2
}
if (-not (Test-Path $manifest)) {
    Write-Host "SKIP: manifest not found at $manifest (create runtime\dataset first)"
    exit 2
}
if (-not (Test-Path $psql)) {
    Write-Host "SKIP: postgres not available under $pgbin"
    exit 2
}

function Start-Drain($p, $file, $which) {
    $rs = [runspacefactory]::CreateRunspace(); $rs.Open()
    $ps = [powershell]::Create(); $ps.Runspace = $rs
    if ($which -eq 'out') {
        [void]$ps.AddScript({ param($p,$f) while (($l=$p.StandardOutput.ReadLine()) -ne $null) { [IO.File]::AppendAllText($f, $l + "`r`n") } }).AddArgument($p).AddArgument($file)
    } else {
        [void]$ps.AddScript({ param($p,$f) while (($l=$p.StandardError.ReadLine()) -ne $null) { [IO.File]::AppendAllText($f, $l + "`r`n") } }).AddArgument($p).AddArgument($file)
    }
    $h = $ps.BeginInvoke()
    return [pscustomobject]@{ ps=$ps; rs=$rs; h=$h }
}
function Start-CapProc([string]$exe, [string[]]$args, [string]$tag) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new(); $psi.FileName = $exe
    if ($args.Count -gt 0) { $psi.Arguments = (($args | ForEach-Object { '"' + $_ + '"' }) -join ' ') }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.RedirectStandardInput = $true
    $psi.CreateNoWindow = $true; $psi.WorkingDirectory = $bin
    $p = [System.Diagnostics.Process]::new(); $p.StartInfo = $psi; [void]$p.Start()
    $outf = Join-Path $bin ("$tag.out.log"); $errf = Join-Path $bin ("$tag.err.log")
    Remove-Item $outf,$errf -ErrorAction SilentlyContinue
    Start-Drain $p $outf 'out' | Out-Null; Start-Drain $p $errf 'err' | Out-Null
    return [pscustomobject]@{ p=$p; out=$outf; err=$errf }
}
function Get-Log([string]$file) { if (Test-Path $file) { Get-Content $file -Raw } else { '' } }
function Kill-Proc($h) { if ($h) { try { if (!$h.p.HasExited) { $h.p.Kill() } } catch {} } }
function DbSql([string]$sql) { & $psql -h 127.0.0.1 -p $PgPort -U postgres -d testtracking -t -A -c $sql 2>$null }

# ---- 1. bootstrap a throwaway postgres (fresh data dir per run) --------
$startedPg = $false
$pgdata = Join-Path $root ("runtime\pgdata_" + [System.Diagnostics.Process]::GetCurrentProcess().Id)
& (Join-Path $pgbin 'initdb.exe') -D $pgdata -U postgres -A trust -E UTF8 | Out-Null
# start postgres.exe directly
$p = Start-Process -FilePath $postgres -ArgumentList @('-D', $pgdata, '-p', "$PgPort") -WorkingDirectory $pgbin -PassThru -NoNewWindow
$startedPg = $true
$tries = 0
while ($tries -lt 60) {
    Start-Sleep -Milliseconds 500
    $listening = ((& $psql -h 127.0.0.1 -p $PgPort -U postgres -d postgres -t -A -c "SELECT 1;" 2>$null | Out-String).Trim() -eq '1')
    if ($listening) { break }
    $tries++
}
if (-not $listening) { Write-Host "FAIL: postgres did not come up on port $PgPort"; Stop-Process -Name postgres -Force -ErrorAction SilentlyContinue; exit 1 }
# create the db (if needed) and clear prior data
& $psql -h 127.0.0.1 -p $PgPort -U postgres -d postgres -c "CREATE DATABASE testtracking;" 2>$null | Out-Null
cmd /c "`"$psql`" -h 127.0.0.1 -p $PgPort -U postgres -d testtracking -c `"DELETE FROM runs; DELETE FROM tasks; DELETE FROM coverage_file_points; DELETE FROM coverage_cov_files; DELETE FROM coverage_checkpoints; DELETE FROM coverage_selected; DELETE FROM coverage_run_summary;`" >nul 2>nul"

# ---- 2. write configs into bin/config ----------------------------------
$cfgDir = Join-Path $bin 'config'; New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$j = @{
  server = @{ router_endpoint="tcp://127.0.0.1:5577"; poll_timeout_ms=50; process_threads=2; client_timeout_ms=5000; task_lease_timeout_ms=0; db_conn="host=127.0.0.1 port=$PgPort dbname=testtracking user=postgres" }
  plugins = @{ coverage = @{ dll="coverage.dll"; enabled=$true; stp_manifest=$manifest; store_type="postgres"; db_conn="host=127.0.0.1 port=$PgPort dbname=testtracking user=postgres"; task_size=10 } }
}
[IO.File]::WriteAllText((Join-Path $cfgDir 'scheduler.json'), ($j | ConvertTo-Json -Depth 6))
$jc = @{
  scheduler = @{ endpoint="tcp://127.0.0.1:5577"; poll_timeout_ms=50 }
  client = @{ worker_id="worker-1"; heartbeat_interval_ms=500; task_threads=2; request_backoff_initial_ms=200; request_backoff_max_ms=30000; register_timeout_ms=5000; register_max_attempts=5; request_timeout_ms=4000 }
  plugins = @{ coverage = @{ dll="coverage.dll"; enabled=$true; dry_run=$true; store_type="postgres"; db_conn="host=127.0.0.1 port=$PgPort dbname=testtracking user=postgres"; dry_run_delay_ms=300; export_dir="reports" } }
}
[IO.File]::WriteAllText((Join-Path $cfgDir 'client.json'), ($jc | ConvertTo-Json -Depth 6))

# ---- 3. run the scenario ----------------------------------------------
$sched = Start-CapProc $schedulerExe @() 'sched'
Start-Sleep -Seconds 3
$client = Start-CapProc $clientExe @() 'client'
Start-Sleep -Seconds 3
$sched.p.StandardInput.WriteLine('run coverage d1')
Start-Sleep -Seconds 6

# kill the scheduler (simulate crash) while the client is running
Kill-Proc $sched
Start-Sleep -Seconds 3
# restart the scheduler; it should restore the run
$sched = Start-CapProc $schedulerExe @() 'sched2'
Start-Sleep -Seconds 2
# wait for the client to detect the stale session and re-register
$deadline = (Get-Date).AddSeconds(30)
$healed = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    $clientLog = Get-Log $client.out
    if ($clientLog -match 're-registering \(stale session\?\)') {
        # require a task batch received AFTER the re-register marker
        $lines = $clientLog -split "`r?`n"
        $mIdx = -1
        for ($i=0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match 're-registering \(stale session\?\)') { $mIdx = $i; break } }
        if ($mIdx -ge 0) {
            $after = $lines[($mIdx+1)..($lines.Count-1)] -join "`n"
            if ($after -match 'task batch') { $healed = $true; break }
        }
    }
}

# teardown
try { $sched.p.StandardInput.WriteLine('quit'); Start-Sleep -Seconds 2 } catch {}
Kill-Proc $sched; Kill-Proc $client
if ($startedPg -and -not $KeepPg) {
    Stop-Process -Name postgres -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    if (Test-Path $pgdata) { Remove-Item -Recurse -Force $pgdata -ErrorAction SilentlyContinue }
}

# ---- 4. report ---------------------------------------------------------
$clientLog = Get-Log (Join-Path $bin 'client.out.log')
$schedLog = Get-Log (Join-Path $bin 'sched2.out.log')
if ($healed) {
    Write-Host 'PASS: client re-registered after scheduler restart and resumed pulling tasks'
    Write-Host "  client re-register lines:"; ($clientLog -split "`r?`n" | Where-Object { $_ -match 're-registering|registered|task batch' } | Select-Object -First 5 | ForEach-Object { "    $_" })
    Write-Host "  scheduler restart restore lines:"; ($schedLog -split "`r?`n" | Where-Object { $_ -match 'run restored|task .* -> client' } | Select-Object -First 5 | ForEach-Object { "    $_" })
    exit 0
}
Write-Host 'FAIL: client did not re-register / resume after scheduler restart'
Write-Host "  --- client log tail ---"; ($clientLog -split "`r?`n" | Select-Object -Last 20)
Write-Host "  --- scheduler log tail ---"; ($schedLog -split "`r?`n" | Select-Object -Last 15)
exit 1
