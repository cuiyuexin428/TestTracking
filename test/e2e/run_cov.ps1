# E2E coverage: version mismatch rejected -> deploy from shared dir -> re-register
# -> 80 files / 2 waves -> greedy settlement -> selected.txt has 80 lines.
param([string]$BinDir = $null, [int]$Port = 5557)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'e2e_common.ps1')

$bin = Get-BinDir $BinDir
$ws = New-E2EWorkspace 'cov'
$storeDir = "$ws\cov_store"
$endpoint = "tcp://127.0.0.1:$Port"
New-Item -ItemType Directory -Force $storeDir | Out-Null

$manifest = 1..80 | ForEach-Object { "file$_.stp,$($_ * 7 % 100 + 10)" }
Set-Content -Path "$ws\manifest.txt" -Value $manifest -Encoding UTF8
New-Item -ItemType Directory -Force "$ws\shared\program" | Out-Null
New-Item -ItemType Directory -Force "$ws\shared\dataset" | Out-Null
Set-Content -Path "$ws\shared\program\step.exe" -Value 'x' -Encoding UTF8
Set-Content -Path "$ws\shared\dataset\dummy.stp" -Value 'x' -Encoding UTF8

$schedCfg = @{
    server = @{
        router_endpoint = "tcp://*:$Port"
        poll_timeout_ms = 50
        process_threads = 2
        client_timeout_ms = 0
        task_lease_timeout_ms = 0
    }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $false }
        coverage = @{
            dll = 'coverage.dll'; enabled = $true
            stp_manifest = "$ws\manifest.txt"
            store_dir = $storeDir
            batch_size = 50
            task_size = 10
            expected_program_version = '1.0'
            expected_dataset_version = 'd1'
            shared_dir = "$ws\shared"
        }
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path "$ws\scheduler.json" -Value $schedCfg -Encoding UTF8

$clientCfg = @{
    scheduler = @{ endpoint = $endpoint; poll_timeout_ms = 50 }
    client = @{ worker_id = 'worker-e2e'; heartbeat_interval_ms = 500; request_backoff_initial_ms = 200; task_threads = 2 }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $false }
        coverage = @{
            dll = 'coverage.dll'; enabled = $true
            dry_run = $true
            store_dir = $storeDir
            program_version = '0.9'
            dataset_version = 'd0'
            deployed = $false
            program_dir = "$ws\prog"
            stp_root = "$ws\data"
        }
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path "$ws\client.json" -Value $clientCfg -Encoding UTF8

$sched = Start-E2EProcess -Exe 'scheduler.exe' -CmdArgs "$ws\scheduler.json" -WorkDir $bin -LogFile "$ws\scheduler.log"
$client = Start-E2EProcess -Exe 'client.exe' -CmdArgs "$ws\client.json" -WorkDir $bin -LogFile "$ws\client.log"

Start-Sleep -Seconds 3
$sched.StandardInput.WriteLine('run coverage d1')
Start-Sleep -Seconds 14
$sched.StandardInput.WriteLine('status')
$sched.StandardInput.WriteLine('quit')
$sched.WaitForExit(5000) | Out-Null
$client.StandardInput.WriteLine('quit')
$client.WaitForExit(5000) | Out-Null

Write-Host "workspace: $ws"
Write-Host "scheduler exit=$($sched.ExitCode) client exit=$($client.ExitCode)"
Assert-LogContains "$ws\scheduler.log" 'selected 80/80 files' 'all 80 files selected'
Assert-LogContains "$ws\scheduler.log" 'coverage plugin: run finished' 'run finished'
Assert-LogContains "$ws\client.log" 'deployed from ' 'client deployed from shared dir'

$runDir = Get-ChildItem $storeDir -Directory | Select-Object -First 1
$selected = Get-Content (Join-Path $runDir.FullName 'selected.txt') -ErrorAction SilentlyContinue
if (($selected | Measure-Object -Line).Lines -ne 80) {
    Write-Host "[FAIL] selected.txt lines=$($selected.Count), expected 80" -ForegroundColor Red
    exit 1
}
Write-Host '[PASS] selected.txt has 80 lines' -ForegroundColor Green
Write-Host 'RESULT: ALL PASS'
