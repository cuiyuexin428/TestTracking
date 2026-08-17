# E2E blank-machine deployment: a client with no program/dataset starts,
# registers, is rejected with deploy instructions, copies program+dataset
# from the shared dir, verifies manifests, persists its deployment state,
# re-registers and starts working. A second client restart then loads the
# persisted state and is accepted without copying again.
param([string]$BinDir = $null, [int]$Port = 5557)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'e2e_common.ps1')

$bin = Get-BinDir $BinDir
$ws = New-E2EWorkspace 'deploy'
$storeDir = "$ws\cov_store"
$endpoint = "tcp://127.0.0.1:$Port"
New-Item -ItemType Directory -Force $storeDir | Out-Null
New-Item -ItemType Directory -Force "$ws\shared\program" | Out-Null
New-Item -ItemType Directory -Force "$ws\shared\dataset" | Out-Null

$manifest = 1..8 | ForEach-Object { "file$_.stp,$($_ * 7 % 100 + 10)" }
Set-Content -Path "$ws\manifest.txt" -Value $manifest -Encoding UTF8
Set-Content -Path "$ws\shared\program\step.exe" -Value 'x' -Encoding UTF8
1..8 | ForEach-Object { Set-Content -Path "$ws\shared\dataset\file$_.stp" -Value "file$_" -Encoding UTF8 }
$stepSize = (Get-Item "$ws\shared\program\step.exe").Length
$stepHash = (Get-FileHash "$ws\shared\program\step.exe" -Algorithm SHA256).Hash.ToLower()
Set-Content -Path "$ws\shared\program\manifest.txt" -Value "step.exe,$stepSize,$stepHash" -Encoding UTF8
$stpManifest = 1..8 | ForEach-Object {
    $p = "$ws\shared\dataset\file$_.stp"
    "file$_.stp,$((Get-Item $p).Length)"
}
Set-Content -Path "$ws\shared\dataset\manifest.txt" -Value $stpManifest -Encoding UTF8

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
            shared_dir = "$ws\shared"
        }
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path "$ws\scheduler.json" -Value $schedCfg -Encoding UTF8

$clientCfg = @{
    scheduler = @{ endpoint = $endpoint; poll_timeout_ms = 50 }
    client = @{ worker_id = 'worker-deploy'; heartbeat_interval_ms = 500; request_backoff_initial_ms = 200; task_threads = 2 }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $false }
        coverage = @{
            dll = 'coverage.dll'; enabled = $true
            dry_run = $true
            store_dir = $storeDir
            program_dir = "$ws\prog"
            stp_root = "$ws\data"
            export_dir = "$ws\reports"
            # No deployed/version keys: the blank machine has nothing.
        }
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path "$ws\client.json" -Value $clientCfg -Encoding UTF8

# Phase 1: blank client deploys and runs.
$sched = Start-E2EProcess -Exe 'scheduler.exe' -CmdArgs "$ws\scheduler.json" -WorkDir $bin -LogFile "$ws\scheduler1.log"
$client = Start-E2EProcess -Exe 'client.exe' -CmdArgs "$ws\client.json" -WorkDir $bin -LogFile "$ws\client1.log"
Start-Sleep -Seconds 3
$sched.StandardInput.WriteLine('run coverage d1')
Start-Sleep -Seconds 12
$sched.StandardInput.WriteLine('quit')
$sched.WaitForExit(5000) | Out-Null
$client.StandardInput.WriteLine('quit')
$client.WaitForExit(5000) | Out-Null

Assert-LogContains "$ws\client1.log" 'deploy required' 'phase 1 rejected with deploy required'
Assert-LogContains "$ws\scheduler1.log" 'coverage plugin: run finished' 'phase 1 run finished'
Assert-LogContains "$ws\client1.log" 'deployment state saved' 'deployment state persisted'
if (-not (Test-Path "$ws\prog\deploy.json")) {
    Write-Host '[FAIL] deploy.json missing' -ForegroundColor Red
    exit 1
}
Write-Host '[PASS] deploy.json written' -ForegroundColor Green

# Phase 2: restart the client; the persisted state must avoid a second copy.
$sched2 = Start-E2EProcess -Exe 'scheduler.exe' -CmdArgs "$ws\scheduler.json" -WorkDir $bin -LogFile "$ws\scheduler2.log"
$client2 = Start-E2EProcess -Exe 'client.exe' -CmdArgs "$ws\client.json" -WorkDir $bin -LogFile "$ws\client2.log"
Start-Sleep -Seconds 3
$sched2.StandardInput.WriteLine('run coverage d1')
Start-Sleep -Seconds 12
$sched2.StandardInput.WriteLine('quit')
$sched2.WaitForExit(5000) | Out-Null
$client2.StandardInput.WriteLine('quit')
$client2.WaitForExit(5000) | Out-Null

Assert-LogContains "$ws\client2.log" 'deployment state loaded' 'restart loaded deployment state'
Assert-LogContains "$ws\scheduler2.log" 'coverage plugin: run finished' 'phase 2 run finished'
$client2Log = Get-Content -Raw "$ws\client2.log"
if ($client2Log -match 'deployed from') {
    Write-Host '[FAIL] phase 2 copied deployment again' -ForegroundColor Red
    exit 1
}
Write-Host '[PASS] phase 2 accepted without copying' -ForegroundColor Green

Write-Host "workspace: $ws"
Write-Host 'RESULT: ALL PASS'
