# E2E STOP/WAKEUP + idempotent execution:
#   - WAKEUP in normal mode is a no-op;
#   - STOP: client clears queued tasks, lets the executing task finish,
#     discards its result and enters silent state (no more task pulls);
#   - WAKEUP resumes the client;
#   - after a scheduler restart the interrupted run is restored and tasks
#     whose files were already stored are skipped by the client (DB
#     idempotency).
param([string]$BinDir = $null, [int]$Port = 5573,
      [string]$DbConn = 'host=127.0.0.1 port=5432 dbname=testtracking user=postgres')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'e2e_common.ps1')

$bin = Get-BinDir $BinDir
$ws = New-E2EWorkspace 'stopwake'
$endpoint = "tcp://127.0.0.1:$Port"
$psql = 'C:\WorkSpace\ThirdParty\pgsql\bin\psql.exe'

$manifest = 1..12 | ForEach-Object { "file$_.stp,$($_ * 7 % 100 + 10)" }
Set-Content -Path "$ws\manifest.txt" -Value $manifest -Encoding UTF8

$schedCfg = @{
    server = @{
        router_endpoint = "tcp://*:$Port"
        poll_timeout_ms = 50
        process_threads = 2
        client_timeout_ms = 0
        task_lease_timeout_ms = 0
        db_conn = $DbConn
    }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $false }
        coverage = @{
            dll = 'coverage.dll'; enabled = $true
            stp_manifest = "$ws\manifest.txt"
            store_type = 'postgres'
            db_conn = $DbConn
            batch_size = 12
            task_size = 3
            expected_program_version = ''
            expected_dataset_version = ''
        }
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path "$ws\scheduler.json" -Value $schedCfg -Encoding UTF8

$clientCfg = @{
    scheduler = @{ endpoint = $endpoint; poll_timeout_ms = 50 }
    client = @{ worker_id = 'worker-s'; heartbeat_interval_ms = 500; request_backoff_initial_ms = 100; task_threads = 2 }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $false }
        coverage = @{
            dll = 'coverage.dll'; enabled = $true
            dry_run = $true
            dry_run_delay_ms = 2000
            store_type = 'postgres'
            db_conn = $DbConn
        }
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path "$ws\client.json" -Value $clientCfg -Encoding UTF8

$procs = New-Object System.Collections.Generic.List[object]

function Start-Direct([string]$Exe, [string]$Arg, [string]$WorkDir, [bool]$NeedStdin) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $Arg
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $NeedStdin
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = $WorkDir
    $p = [System.Diagnostics.Process]::Start($psi)
    $h = @{ proc = $p; out = $p.StandardOutput.ReadToEndAsync(); err = $p.StandardError.ReadToEndAsync() }
    $procs.Add($h)
    return $h
}

function Write-Log([object]$h, [string]$Log) {
    if (-not $h.proc.HasExited) {
        $h.proc.Kill()
        $h.proc.WaitForExit(3000) | Out-Null
    }
    [IO.File]::WriteAllText($Log, ($h.out.Result + $h.err.Result))
}

function Stop-ClientProcs {
    Get-Process client -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$bin\client.exe" } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

function Count-RunSummaries {
    $v = & $psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -t -A -c "SELECT count(*) FROM coverage_run_summary WHERE finished_at > now() - interval '5 minutes';" 2>$null
    if ($null -eq $v) { return 0 }
    return [int]$v.Trim()
}

function Get-LatestRunId {
    $v = & $psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -t -A -c "SELECT run_id FROM coverage_checkpoints ORDER BY updated_at DESC LIMIT 1;" 2>$null
    if ($null -eq $v) { return '' }
    return $v.Trim()
}

function Count-WavePoints([string]$RunId) {
    $v = & $psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -t -A -c "SELECT count(*) FROM coverage_wave_points WHERE run_id='$RunId';" 2>$null
    if ($null -eq $v) { return 0 }
    return [int]$v.Trim()
}

$baseline = Count-RunSummaries

try {
    # Phase 1: run, wakeup-as-noop, stop mid-run, silent, wakeup resume.
    $sched1 = Start-Direct "$bin\scheduler.exe" "$ws\scheduler.json" $bin $true
    Start-Sleep -Seconds 2
    $client1 = Start-Direct "$bin\client.exe" "$ws\client.json" $bin $true
    Start-Sleep -Seconds 1
    $sched1.proc.StandardInput.WriteLine('run coverage v1')
    $sched1.proc.StandardInput.Flush()
    $run1 = ''
    $wpDeadline = (Get-Date).AddSeconds(40)
    while ((Get-Date) -lt $wpDeadline) {
        Start-Sleep -Milliseconds 500
        if ($run1 -eq '') { $run1 = Get-LatestRunId }
        $points = Count-WavePoints $run1
        if ($points -ge 2) { break }
    }
    $pointsAtStop = Count-WavePoints $run1
    if ($pointsAtStop -lt 2 -or $pointsAtStop -ge 6) {
        Write-Host "[FAIL] stop timing window missed (points=$pointsAtStop)" -ForegroundColor Red
        exit 1
    }
    $sched1.proc.StandardInput.WriteLine('wakeup coverage')
    $sched1.proc.StandardInput.Flush()
    Start-Sleep -Seconds 1
    $sched1.proc.StandardInput.WriteLine('stop coverage')
    $sched1.proc.StandardInput.Flush()
    Start-Sleep -Seconds 4

    # Resume and finish a second run.
    $sched1.proc.StandardInput.WriteLine('run coverage v2')
    $sched1.proc.StandardInput.Flush()
    Start-Sleep -Seconds 1
    $sched1.proc.StandardInput.WriteLine('wakeup coverage')
    $sched1.proc.StandardInput.Flush()
    $v2Deadline = (Get-Date).AddSeconds(60)
    $v2Done = $false
    while ((Get-Date) -lt $v2Deadline) {
        Start-Sleep -Seconds 2
        $c = Count-RunSummaries
        if ($c -ge ($baseline + 1)) { $v2Done = $true; break }
    }
    if (-not $v2Done) {
        Write-Host '[FAIL] run v2 did not finish after wakeup' -ForegroundColor Red
        exit 1
    }
    Write-Host '[PASS] run v2 finished after wakeup' -ForegroundColor Green
    $sched1.proc.StandardInput.WriteLine('quit')
    $sched1.proc.StandardInput.Flush()
    Start-Sleep -Seconds 2
    if (-not $sched1.proc.HasExited) { $sched1.proc.Kill() }
    Stop-ClientProcs
    Write-Log $sched1 "$ws\scheduler1.log"
    Write-Log $client1 "$ws\client1.log"
    Assert-LogContains "$ws\client1.log" 'wakeup received \(no-op, already active\)' 'wakeup no-op in normal mode'
    Assert-LogContains "$ws\scheduler1.log" 'run stopped: ' 'stop executed'
    Assert-LogContains "$ws\scheduler1.log" 'coverage plugin: run cancelled' 'plugin pool cancelled'
    Assert-LogContains "$ws\scheduler1.log" 'coverage plugin: requeued to priority' 'tasks requeued at stop'
    Assert-LogContains "$ws\client1.log" 'STOP received' 'client handled STOP'
    Assert-LogContains "$ws\client1.log" 'silent state' 'client entered silent state'

    # Between silent and wakeup there must be no task batch in client1.log.
    $client1Text = Get-Content -Raw "$ws\client1.log"
    $silentIdx = $client1Text.IndexOf('entering silent state')
    $resumeIdx = $client1Text.IndexOf('wakeup received, resuming')
    if ($silentIdx -lt 0 -or $resumeIdx -lt 0 -or $resumeIdx -le $silentIdx) {
        Write-Host '[FAIL] silent/wakeup ordering invalid' -ForegroundColor Red
        exit 1
    }
    $silentWindow = $client1Text.Substring($silentIdx, $resumeIdx - $silentIdx)
    if ($silentWindow -match 'task batch') {
        Write-Host '[FAIL] tasks dispatched while client is silent' -ForegroundColor Red
        exit 1
    }
    Write-Host '[PASS] no tasks dispatched while silent' -ForegroundColor Green

    # Phase 2: restart the scheduler; the interrupted run is restored and
    # already-stored files are skipped by the client.
    $sched2 = Start-Direct "$bin\scheduler.exe" "$ws\scheduler.json" $bin $true
    Start-Sleep -Seconds 5
    $client2 = Start-Direct "$bin\client.exe" "$ws\client.json" $bin $true
    $restoreDeadline2 = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $restoreDeadline2) {
        Start-Sleep -Seconds 2
        if ((Count-RunSummaries) -ge ($baseline + 2)) { break }
    }
    Start-Sleep -Seconds 2
    Write-Log $sched2 "$ws\scheduler2.log"
    Write-Log $client2 "$ws\client2.log"
    Assert-LogContains "$ws\scheduler2.log" 'run restored' 'run auto-restored'
    Assert-LogContains "$ws\scheduler2.log" 'coverage plugin: run finished' 'restored run finished'
    Assert-LogContains "$ws\client2.log" 'already covered, skip' 'client skipped already-stored files'

    Write-Host "workspace: $ws"
    Write-Host 'RESULT: ALL PASS'
} finally {
    $i = 0
    foreach ($h in $procs) {
        if (-not $h.proc.HasExited) {
            $h.proc.Kill()
            $h.proc.WaitForExit(3000) | Out-Null
        }
        ++$i
        try {
            [IO.File]::WriteAllText("$ws\proc_$i.log", ($h.out.Result + $h.err.Result))
        } catch {
        }
    }
    Stop-ClientProcs
}
