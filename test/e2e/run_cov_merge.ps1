# E2E cov merge: runs real StepTest files under OpenCppCoverage, stores
# per-file point sets and .cov binaries, settles multiple waves, and checks
# that the wave-level total coverage (covered/total/percent) was computed
# from a cov-only merge and persisted.
#
# File-store variant keeps staging for offline cross-validation
# (tools/validate_coverage.py); postgres variant verifies per-wave cleanup.
param(
    [string]$BinDir = $null,
    [int]$Port = 5557,
    [string]$DbConn = '',
    [string]$StepDir = 'C:\WorkSpace\occt_demo\step',
    [string]$StepTestExe = 'C:\WorkSpace\occt_demo\build\Debug\StepTest.exe',
    [string]$OpenCppCoverageExe = 'C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe',
    [string[]]$OcctDlls = @('TKernel.dll', 'TKBRep.dll', 'TKDESTEP.dll', 'TKTopAlgo.dll')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'e2e_common.ps1')

$bin = Get-BinDir $BinDir
$ws = New-E2EWorkspace 'cov_merge'
$storeDir = "$ws\cov_store"
$endpoint = "tcp://127.0.0.1:$Port"
$usePg = $DbConn -ne ''
New-Item -ItemType Directory -Force $storeDir | Out-Null
New-Item -ItemType Directory -Force "$ws\reports" | Out-Null

$stpFiles = Get-ChildItem $StepDir -File -Filter *.stp | Sort-Object Name | Select-Object -First 6
if ($stpFiles.Count -lt 4) {
    throw "Need at least 4 stp files under $StepDir"
}
$manifest = $stpFiles | ForEach-Object { "$($_.Name),$($_.Length)" }
Set-Content -Path "$ws\manifest.txt" -Value $manifest -Encoding UTF8

$occtDllPaths = $OcctDlls | ForEach-Object {
    $p = Join-Path (Split-Path $StepTestExe) $_
    if (-not (Test-Path $p)) { throw "OCCT DLL not found: $p" }
    $p
}

$storeConfig = @{}
if ($usePg) {
    $storeConfig['store_type'] = 'postgres'
    $storeConfig['db_conn'] = $DbConn
} else {
    $storeConfig['store_dir'] = $storeDir
}

$schedCfg = @{
    server = @{
        router_endpoint = "tcp://*:$Port"
        poll_timeout_ms = 50
        process_threads = 2
        client_timeout_ms = 0
        task_lease_timeout_ms = 0
        db_conn = if ($usePg) { $DbConn } else { '' }
    }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $false }
        coverage = @{
            dll = 'coverage.dll'; enabled = $true
            stp_manifest = "$ws\manifest.txt"
            batch_size = 4
            task_size = 2
            store_cov = $true
            opencppcoverage = $OpenCppCoverageExe
            failed_file_retries = 0
            keep_staging = -not $usePg
            expected_program_version = ''
            expected_dataset_version = ''
        } + $storeConfig
    }
} | ConvertTo-Json -Depth 6
Set-Content -Path "$ws\scheduler.json" -Value $schedCfg -Encoding UTF8

$clientCfg = @{
    scheduler = @{ endpoint = $endpoint; poll_timeout_ms = 50 }
    client = @{ worker_id = 'worker-cov-merge'; heartbeat_interval_ms = 500; request_backoff_initial_ms = 200; task_threads = 2 }
    plugins = @{
        demo = @{ dll = 'demo.dll'; enabled = $false }
        coverage = @{
            dll = 'coverage.dll'; enabled = $true
            dry_run = $false
            store_cov = $true
            stp_root = $StepDir
            opencppcoverage = $OpenCppCoverageExe
            target_exe = $StepTestExe
            occt_dlls = $occtDllPaths
            export_dir = "$ws\reports"
            keep_reports = $true
        } + $storeConfig
    }
} | ConvertTo-Json -Depth 6
Set-Content -Path "$ws\client.json" -Value $clientCfg -Encoding UTF8

$sched = Start-E2EProcess -Exe 'scheduler.exe' -CmdArgs "$ws\scheduler.json" -WorkDir $bin -LogFile "$ws\scheduler.log"
$client = Start-E2EProcess -Exe 'client.exe' -CmdArgs "$ws\client.json" -WorkDir $bin -LogFile "$ws\client.log"

Start-Sleep -Seconds 3
$sched.StandardInput.WriteLine('run coverage dev')
Start-Sleep -Seconds 150
$sched.StandardInput.WriteLine('status')
$sched.StandardInput.WriteLine('quit')
$sched.WaitForExit(5000) | Out-Null
$client.StandardInput.WriteLine('quit')
$client.WaitForExit(5000) | Out-Null

Write-Host "workspace: $ws"
Write-Host "scheduler exit=$($sched.ExitCode) client exit=$($client.ExitCode)"

Assert-LogContains "$ws\scheduler.log" 'total coverage=' 'wave total coverage computed'
Assert-LogContains "$ws\scheduler.log" 'coverage plugin: run finished' 'run finished'

if ($usePg) {
    $psql = 'C:\WorkSpace\ThirdParty\pgsql\bin\psql.exe'
    $runId = (& $psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -t -A -c "SELECT run_id FROM coverage_run_summary ORDER BY finished_at DESC LIMIT 1").Trim()
    if (-not $runId) { throw 'no run summary row' }
    $summary = & $psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -t -A -c "SELECT count(*), min(total_lines), max(total_lines) FROM coverage_wave_summary WHERE run_id='$runId'"
    $covRows = (& $psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -t -A -c "SELECT count(*) FROM coverage_cov_files WHERE run_id='$runId'").Trim()
    $cumCov = (& $psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -t -A -c "SELECT count(*) FROM coverage_cumulative_cov WHERE run_id='$runId'").Trim()
    $pointsRows = (& $psql -h 127.0.0.1 -p 5432 -U postgres -d testtracking -t -A -c "SELECT count(*) FROM coverage_wave_points WHERE run_id='$runId'").Trim()
    Write-Host "wave summaries: $summary, cov files: $covRows, cumulative cov: $cumCov, wave points: $pointsRows"
    if ($summary -notmatch '^\d+\|\s*[1-9]') { throw 'wave summaries missing positive total_lines' }
    if ($covRows -ne '0') { throw 'per-wave cov files were not cleaned' }
    if ($cumCov -ne '1') { throw 'cumulative cov row missing' }
    if ($pointsRows -ne '0') { throw 'wave point rows were not cleaned' }
    Write-Host '[PASS] postgres wave summary + cleanup' -ForegroundColor Green
} else {
    $runDir = Get-ChildItem $storeDir -Directory | Select-Object -First 1
    if (-not $runDir) { throw 'no run dir in store' }
    $summaries = Get-ChildItem $runDir.FullName -Filter 'wave-summary-*.json'
    if ($summaries.Count -lt 2) { throw "expected >=2 wave summaries, got $($summaries.Count)" }
    foreach ($s in $summaries) {
        $json = Get-Content -Raw $s.FullName | ConvertFrom-Json
        if ($json.total_lines -le 0 -or $json.covered_lines -le 0) {
            throw "bad wave summary $($s.Name): covered=$($json.covered_lines) total=$($json.total_lines)"
        }
    }
    if (-not (Test-Path (Join-Path $runDir.FullName 'cumulative-cov.bin'))) {
        throw 'cumulative-cov.bin missing'
    }
    $waveDirs = Get-ChildItem $runDir.FullName -Directory -Filter 'wave-*'
    foreach ($w in $waveDirs) {
        if ((Get-ChildItem $w.FullName -Filter '*.cov').Count -eq 0) {
            throw "no cov files in $($w.Name)"
        }
    }
    $selected = Get-Content (Join-Path $runDir.FullName 'selected.txt') | Where-Object { $_ }
    if ($selected.Count -lt 2) { throw "selected.txt too small: $($selected.Count)" }
    Write-Host "[PASS] file-store wave summaries + staging kept: $($summaries.Count) waves, $($selected.Count) selected" -ForegroundColor Green
}

Write-Host 'RESULT: ALL PASS'
