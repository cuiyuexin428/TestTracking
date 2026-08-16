# E2E main flow: demo plugin, register -> request -> execute -> report -> run finished.
param([string]$BinDir = $null, [int]$Port = 5557)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'e2e_common.ps1')

$bin = Get-BinDir $BinDir
$ws = New-E2EWorkspace 'main'
$endpoint = "tcp://127.0.0.1:$Port"

$schedCfg = @{
    server = @{
        router_endpoint = "tcp://*:$Port"
        poll_timeout_ms = 50
        process_threads = 2
        client_timeout_ms = 0
        task_lease_timeout_ms = 0
    }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $true }
        coverage = @{ dll = 'coverage.dll'; enabled = $false }
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path "$ws\scheduler.json" -Value $schedCfg -Encoding UTF8

$clientCfg = @{
    scheduler = @{ endpoint = $endpoint; poll_timeout_ms = 50 }
    client = @{ worker_id = 'worker-e2e'; heartbeat_interval_ms = 500; request_backoff_initial_ms = 200 }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $true; task_sleep_ms = 100 }
        coverage = @{ dll = 'coverage.dll'; enabled = $false }
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path "$ws\client.json" -Value $clientCfg -Encoding UTF8

$sched = Start-E2EProcess -Exe 'scheduler.exe' -CmdArgs "$ws\scheduler.json" -WorkDir $bin -LogFile "$ws\scheduler.log"
$client = Start-E2EProcess -Exe 'client.exe' -CmdArgs "$ws\client.json" -WorkDir $bin -LogFile "$ws\client.log"

Start-Sleep -Seconds 2
$sched.StandardInput.WriteLine('clients')
$sched.StandardInput.WriteLine('run demo v1')
Start-Sleep -Seconds 6
$sched.StandardInput.WriteLine('status')
$sched.StandardInput.WriteLine('quit')
$sched.WaitForExit(5000) | Out-Null
$client.StandardInput.WriteLine('quit')
$client.WaitForExit(5000) | Out-Null

Write-Host "workspace: $ws"
Write-Host "scheduler exit=$($sched.ExitCode) client exit=$($client.ExitCode)"
Assert-LogContains "$ws\scheduler.log" 'demo plugin: run finished' 'run finished'
Assert-LogContains "$ws\client.log" 'registered: plugin_type=demo' 'client registered'
Assert-LogContains "$ws\scheduler.log" 'run finished: ' 'scheduler cleared active run'
Write-Host 'RESULT: ALL PASS'
