# E2E offline reclaim: a client is killed mid-run; its in-flight tasks must
# be requeued into the coverage plugin's priority queue and dispatched to the
# next client. Tasks already reported before the kill must NOT be reclaimed.
param([string]$BinDir = $null, [int]$Port = 5572)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'e2e_common.ps1')

$bin = Get-BinDir $BinDir
$ws = New-E2EWorkspace 'reclaim'
$storeDir = "$ws\store"
$endpoint = "tcp://127.0.0.1:$Port"
New-Item -ItemType Directory -Force $storeDir | Out-Null

$manifest = 1..12 | ForEach-Object { "file$_.stp,$($_ * 7 % 100 + 10)" }
Set-Content -Path "$ws\manifest.txt" -Value $manifest -Encoding UTF8

$schedCfg = @{
    server = @{
        router_endpoint = "tcp://*:$Port"
        poll_timeout_ms = 50
        process_threads = 2
        client_timeout_ms = 2000
        task_lease_timeout_ms = 0
    }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $false }
        coverage = @{
            dll = 'coverage.dll'; enabled = $true
            stp_manifest = "$ws\manifest.txt"
            store_dir = $storeDir
            batch_size = 12
            task_size = 3
            expected_program_version = ''
            expected_dataset_version = ''
        }
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path "$ws\scheduler.json" -Value $schedCfg -Encoding UTF8

function New-ClientCfg([string]$Id) {
    @{
        scheduler = @{ endpoint = $endpoint; poll_timeout_ms = 50 }
        client = @{ worker_id = $Id; heartbeat_interval_ms = 500; request_backoff_initial_ms = 100; task_threads = 2 }
        plugins = @{
            demo = @{ dll = 'demo.dll'; enabled = $false }
            coverage = @{ dll = 'coverage.dll'; enabled = $true; dry_run = $true; dry_run_delay_ms = 500; store_dir = $storeDir }
        }
    } | ConvertTo-Json -Depth 5
}
Set-Content -Path "$ws\client-a.json" -Value (New-ClientCfg 'worker-a') -Encoding UTF8
Set-Content -Path "$ws\client-b.json" -Value (New-ClientCfg 'worker-b') -Encoding UTF8

function Stop-ClientProcs {
    Get-Process client -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$bin\client.exe" } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    Get-Process client -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

$sched = Start-E2EProcess -Exe 'scheduler.exe' -CmdArgs "$ws\scheduler.json" -WorkDir $bin -LogFile "$ws\scheduler.log"
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Milliseconds 500
    if (Select-String -Path "$ws\scheduler.log" -Pattern 'scheduler started' -Quiet) { break }
}

$clientA = Start-E2EProcess -Exe 'client.exe' -CmdArgs "$ws\client-a.json" -WorkDir $bin -LogFile "$ws\client-a.log"
$sched.StandardInput.WriteLine('run coverage d1')
$sched.StandardInput.Flush()

# Wait until at least one task completed+reported and two tasks were acked.
$ready = $false
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 200
    $log = Get-Content "$ws\scheduler.log" -ErrorAction SilentlyContinue
    $acked = @($log | Select-String 'acked by worker-a').Count
    $reported = @($log | Select-String 'coverage plugin: result').Count
    if ($acked -ge 2 -and $reported -ge 1) { $ready = $true; break }
}
if (-not $ready) {
    Write-Host '[FAIL] client did not ack/report tasks in time' -ForegroundColor Red
    exit 1
}
$completedLine = Get-Content "$ws\scheduler.log" | Select-String 'coverage plugin: result' | Select-Object -First 1
$completedId = ($completedLine.Line -split ' ')[5]
Write-Host "completed task: $completedId"

Stop-ClientProcs
$clientA.WaitForExit(5000) | Out-Null

# Wait for offline detection + reclaim.
Start-Sleep -Seconds 4
Assert-LogContains "$ws\scheduler.log" 'client offline: worker-a' 'offline detected'
Assert-LogContains "$ws\scheduler.log" 'coverage plugin: requeued to priority' 'tasks requeued to priority'

$requeued = @()
Get-Content "$ws\scheduler.log" | Select-String 'coverage plugin: requeued to priority' | ForEach-Object {
    $requeued += ($_.Line -split ' ')[-1]
}
if ($requeued.Count -lt 1) {
    Write-Host '[FAIL] no requeued tasks parsed' -ForegroundColor Red
    exit 1
}
if ($requeued -contains $completedId) {
    Write-Host "[FAIL] completed task $completedId was reclaimed" -ForegroundColor Red
    exit 1
}
Write-Host "[PASS] completed task not reclaimed; $($requeued.Count) tasks requeued" -ForegroundColor Green

# Start client B; the priority tasks must be dispatched to it first.
$clientB = Start-E2EProcess -Exe 'client.exe' -CmdArgs "$ws\client-b.json" -WorkDir $bin -LogFile "$ws\client-b.log"
$dispatchOk = $false
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 300
    $log = Get-Content "$ws\scheduler.log" -Raw -ErrorAction SilentlyContinue
    $allDispatched = $true
    foreach ($id in $requeued) {
        if ($log -notmatch [regex]::Escape("task $id -> client worker-b")) {
            $allDispatched = $false
            break
        }
    }
    if ($allDispatched) { $dispatchOk = $true; break }
}
if (-not $dispatchOk) {
    Write-Host '[FAIL] requeued tasks were not dispatched to worker-b' -ForegroundColor Red
    exit 1
}
Write-Host '[PASS] all requeued tasks dispatched to worker-b' -ForegroundColor Green

for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 300
    if (Select-String -Path "$ws\scheduler.log" -Pattern 'coverage plugin: run finished' -Quiet) { break }
}
Assert-LogContains "$ws\scheduler.log" 'coverage plugin: run finished' 'run finished after reclaim'

$sched.StandardInput.WriteLine('quit')
$sched.WaitForExit(5000) | Out-Null
$clientB.StandardInput.WriteLine('quit')
$clientB.WaitForExit(5000) | Out-Null

Write-Host "workspace: $ws"
Write-Host 'RESULT: ALL PASS'
