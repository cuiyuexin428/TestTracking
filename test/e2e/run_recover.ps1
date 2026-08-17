# E2E recovery: stop mid-run with DB persistence, restart the scheduler,
# auto-restore the run from its checkpoint, and finish it with a fresh client.
param([string]$BinDir = $null, [int]$Port = 5557,
      [string]$DbConn = 'host=127.0.0.1 port=5432 dbname=testtracking user=postgres')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'e2e_common.ps1')

$bin = Get-BinDir $BinDir
$ws = New-E2EWorkspace 'recover'
$endpoint = "tcp://127.0.0.1:$Port"

$manifest = 1..80 | ForEach-Object { "file$_.stp,$($_ * 7 % 100 + 10)" }
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
            batch_size = 50
            task_size = 10
            expected_program_version = ''
            expected_dataset_version = ''
            shared_dir = ''
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
            store_type = 'postgres'
            db_conn = $DbConn
            program_version = '0.9'
            dataset_version = 'd0'
            deployed = $true
        }
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path "$ws\client.json" -Value $clientCfg -Encoding UTF8

# Phase 1: start a run, stop it mid-run, then shut everything down.
$sched1 = Start-E2EProcess -Exe 'scheduler.exe' -CmdArgs "$ws\scheduler.json" -WorkDir $bin -LogFile "$ws\scheduler1.log"
$client1 = Start-E2EProcess -Exe 'client.exe' -CmdArgs "$ws\client.json" -WorkDir $bin -LogFile "$ws\client1.log"
Start-Sleep -Seconds 2
$sched1.StandardInput.WriteLine('run coverage d1')
Start-Sleep -Milliseconds 500
$sched1.StandardInput.WriteLine('stop coverage')
Start-Sleep -Seconds 2
$sched1.StandardInput.WriteLine('quit')
$sched1.WaitForExit(5000) | Out-Null
$client1.StandardInput.WriteLine('quit')
$client1.WaitForExit(5000) | Out-Null

Assert-LogContains "$ws\scheduler1.log" 'run stopped: ' 'stop executed'
Assert-LogContains "$ws\scheduler1.log" 'coverage plugin: run cancelled' 'plugin pool cancelled'

# Phase 2: restart the scheduler, expect auto-restore; a fresh client resumes
# the run and it finishes.
$sched2 = Start-E2EProcess -Exe 'scheduler.exe' -CmdArgs "$ws\scheduler.json" -WorkDir $bin -LogFile "$ws\scheduler2.log"
Start-Sleep -Seconds 2
Assert-LogContains "$ws\scheduler2.log" 'run restored: ' 'run auto restored'
$client2 = Start-E2EProcess -Exe 'client.exe' -CmdArgs "$ws\client.json" -WorkDir $bin -LogFile "$ws\client2.log"
Start-Sleep -Seconds 10
$sched2.StandardInput.WriteLine('quit')
$sched2.WaitForExit(5000) | Out-Null
$client2.StandardInput.WriteLine('quit')
$client2.WaitForExit(5000) | Out-Null

Assert-LogContains "$ws\scheduler2.log" 'coverage plugin: run finished' 'resumed run finished'
Assert-LogContains "$ws\scheduler2.log" 'selected 80/80 files' 'all 80 files selected after resume'
Write-Host 'RESULT: ALL PASS'
