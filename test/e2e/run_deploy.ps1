# E2E registration gate: a client whose dataset version does not match the
# scheduler expectation is rejected and exits with an error; after updating
# version.txt it registers and runs normally.
param([string]$BinDir = $null, [int]$Port = 5557)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'e2e_common.ps1')

$bin = Get-BinDir $BinDir
$ws = New-E2EWorkspace 'deploy'
$storeDir = "$ws\cov_store"
$endpoint = "tcp://127.0.0.1:$Port"
New-Item -ItemType Directory -Force $storeDir | Out-Null
New-Item -ItemType Directory -Force "$ws\prog" | Out-Null
New-Item -ItemType Directory -Force "$ws\data" | Out-Null
Set-Content -Path "$ws\prog\version.txt" -Value '1.0' -Encoding ASCII
Set-Content -Path "$ws\data\version.txt" -Value 'd0' -Encoding ASCII

$manifest = 1..8 | ForEach-Object { "file$_.stp,$($_ * 7 % 100 + 10)" }
Set-Content -Path "$ws\manifest.txt" -Value $manifest -Encoding UTF8

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
            batch_size = 4
            task_size = 2
            expected_program_version = '1.0'
            expected_dataset_version = 'd1'
        }
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path "$ws\scheduler.json" -Value $schedCfg -Encoding UTF8

$clientCfg = @{
    scheduler = @{ endpoint = $endpoint; poll_timeout_ms = 50 }
    client = @{ worker_id = 'worker-gate'; heartbeat_interval_ms = 500; request_backoff_initial_ms = 200; task_threads = 2 }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $false }
        coverage = @{
            dll = 'coverage.dll'; enabled = $true
            dry_run = $true
            store_dir = $storeDir
            program_dir = "$ws\prog"
            stp_root = "$ws\data"
        }
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path "$ws\client.json" -Value $clientCfg -Encoding UTF8

# Phase 1: stale dataset version -> registration rejected, client exits.
$sched = Start-E2EProcess -Exe 'scheduler.exe' -CmdArgs "$ws\scheduler.json" -WorkDir $bin -LogFile "$ws\scheduler1.log"
$client = Start-E2EProcess -Exe 'client.exe' -CmdArgs "$ws\client.json" -WorkDir $bin -LogFile "$ws\client1.log"
$client.WaitForExit(15000) | Out-Null
$sched.StandardInput.WriteLine('quit')
$sched.WaitForExit(5000) | Out-Null

Assert-LogContains "$ws\client1.log" 'registration rejected' 'phase 1 rejected'
Assert-LogContains "$ws\client1.log" 'version mismatch' 'phase 1 reason contains mismatch'
if ($client.ExitCode -ne 1) {
    Write-Host "[FAIL] client exit code=$($client.ExitCode), expected 1" -ForegroundColor Red
    exit 1
}
Write-Host '[PASS] rejected client exited with code 1' -ForegroundColor Green

# Phase 2: fix the dataset version, client registers and runs.
Set-Content -Path "$ws\data\version.txt" -Value 'd1' -Encoding ASCII
$sched2 = Start-E2EProcess -Exe 'scheduler.exe' -CmdArgs "$ws\scheduler.json" -WorkDir $bin -LogFile "$ws\scheduler2.log"
$client2 = Start-E2EProcess -Exe 'client.exe' -CmdArgs "$ws\client.json" -WorkDir $bin -LogFile "$ws\client2.log"
Start-Sleep -Seconds 3
$sched2.StandardInput.WriteLine('run coverage d1')
Start-Sleep -Seconds 10
$sched2.StandardInput.WriteLine('quit')
$sched2.WaitForExit(5000) | Out-Null
$client2.StandardInput.WriteLine('quit')
$client2.WaitForExit(5000) | Out-Null

Assert-LogContains "$ws\client2.log" 'registered: plugin_type=coverage' 'phase 2 registered'
Assert-LogContains "$ws\scheduler2.log" 'coverage plugin: run finished' 'phase 2 run finished'

Write-Host "workspace: $ws"
Write-Host 'RESULT: ALL PASS'
