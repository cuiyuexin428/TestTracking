# Launch the web console and scheduler from a source checkout prepared by
# setup_offline_server.ps1. This uses only the bundled offline Python runtime.
[CmdletBinding()]
param(
    [ValidateSet('start_server', 'start_result_service', 'stop_result_service')]
    [string]$Action = 'start_server',
    [string]$RuntimeDir = '',
    [switch]$NoWait
)
$ErrorActionPreference = 'Stop'
if (-not $RuntimeDir) { $RuntimeDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'runtime' }
$RuntimeDir = [IO.Path]::GetFullPath($RuntimeDir)
$project = Split-Path -Parent $RuntimeDir
$configPath = Join-Path $RuntimeDir 'result_service.json'
$launcher = Join-Path $project 'result_service\launcher.py'
$python = Join-Path $RuntimeDir 'python\python.exe'
$pidFile = Join-Path $RuntimeDir 'result_service.pid'
$log = Join-Path $RuntimeDir 'logs\result_service.log'
$errorLog = Join-Path $RuntimeDir 'logs\result_service.err.log'
$settings = $null

# Only restart the exact local database initialized by setup. A deployment
# prepared with -SkipDatabase leaves database lifecycle to its administrator.
if ($Action -eq 'start_server') {
    $settings = Get-Content -LiteralPath (Join-Path $RuntimeDir 'source_launch.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($settings.managed_database) {
        $pgCtl = [string]$settings.managed_database.pg_ctl
        $pgData = [string]$settings.managed_database.data_dir
        if (-not (Test-Path -LiteralPath $pgCtl -PathType Leaf) -or
            -not (Test-Path -LiteralPath (Join-Path $pgData 'PG_VERSION') -PathType Leaf)) {
            throw "Managed PostgreSQL files are missing. Check $pgCtl and $pgData; initialization is not performed by the start script."
        }
        & $pgCtl status -D $pgData 2>$null | Out-Null
        if ($LASTEXITCODE -eq 3) {
            [IO.Directory]::CreateDirectory((Split-Path -Parent $log)) | Out-Null
            & $pgCtl start -D $pgData -l (Join-Path $RuntimeDir 'logs\postgres.log') -w -t 30
            if ($LASTEXITCODE -ne 0) { throw "Managed PostgreSQL startup failed: $pgData" }
        } elseif ($LASTEXITCODE -ne 0) {
            throw "Cannot determine managed PostgreSQL status: $pgData (exit $LASTEXITCODE)"
        }
        Write-Host "Managed PostgreSQL ready: $pgData"
    }
}

function Get-TrackedResultProcess {
    if (-not (Test-Path -LiteralPath $pidFile -PathType Leaf)) { return $null }
    $processId = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
    $details = Get-CimInstance Win32_Process -Filter "ProcessId = $processId" -ErrorAction SilentlyContinue
    if (-not $details) {
        Remove-Item -LiteralPath $pidFile -Force
        return $null
    }
    if ($details.Name -notmatch '^python(?:w)?\.exe$' -or
        ([string]$details.CommandLine).IndexOf($launcher, [StringComparison]::OrdinalIgnoreCase) -lt 0 -or
        ([string]$details.CommandLine).IndexOf($configPath, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "PID file does not belong to this source result service: $processId"
    }
    return Get-Process -Id $processId -ErrorAction SilentlyContinue
}

if ($Action -eq 'stop_result_service') {
    $process = Get-TrackedResultProcess
    if ($process) {
        & taskkill.exe /PID $process.Id /T /F 2>$null | Out-Null
        Wait-Process -Id $process.Id -Timeout 15 -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
    Write-Host 'Result service stopped; scheduler and database remain available.'
    return
}

foreach ($required in @($configPath, $launcher, $python)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required source deployment file not found: $required" }
}
$config = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
$listenHost = if ($env:RESULT_SERVICE_HOST) { $env:RESULT_SERVICE_HOST } else { [string]$config.host }
$listenPort = if ($env:RESULT_SERVICE_PORT) { [int]$env:RESULT_SERVICE_PORT } else { [int]$config.port }
if ($listenPort -lt 1 -or $listenPort -gt 65535) { throw "Invalid result service port: $listenPort" }
$probeHost = if ($listenHost -in @('', '*', '0.0.0.0')) { '127.0.0.1' } else { $listenHost }
if ($probeHost.Contains(':') -and -not $probeHost.StartsWith('[')) { $probeHost = "[$probeHost]" }
$healthUrl = "http://${probeHost}:$listenPort/healthz"
function Test-ServiceHealth {
    try {
        $request = [Net.WebRequest]::Create($healthUrl)
        $request.Proxy = $null
        $request.Timeout = 1000
        $response = $request.GetResponse()
        try { return [int]$response.StatusCode -eq 200 } finally { $response.Close() }
    } catch { return $false }
}

$process = Get-TrackedResultProcess
if ($process) {
    if (-not (Test-ServiceHealth)) { throw "Existing result service is not healthy: $healthUrl; inspect $errorLog" }
} else {
    [IO.Directory]::CreateDirectory((Split-Path -Parent $log)) | Out-Null
    $process = Start-Process -FilePath $python -ArgumentList @('-B', ('"' + $launcher + '"'), '--config', ('"' + $configPath + '"')) `
        -WorkingDirectory $project -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError $errorLog -PassThru
    [IO.File]::WriteAllText($pidFile, [string]$process.Id)
    $healthy = $false
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($process.HasExited) { break }
        if (Test-ServiceHealth) { $healthy = $true; break }
    }
    if (-not $healthy) {
        if (-not $process.HasExited) { & taskkill.exe /PID $process.Id /T /F 2>$null | Out-Null }
        Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
        $details = if (Test-Path -LiteralPath $errorLog) { (Get-Content -LiteralPath $errorLog -Tail 10) -join "`n" } else { '' }
        throw "Result service failed health check at $healthUrl. $details"
    }
}
Write-Host "Web console: http://127.0.0.1:$listenPort/ (LAN: http://<server-IP>:$listenPort/)"
Write-Host "Result service pid: $($process.Id); logs: $log"

if ($Action -eq 'start_server') {
    $scheduler = Join-Path ([string]$settings.bin_dir) 'scheduler.exe'
    if (-not (Test-Path -LiteralPath $scheduler -PathType Leaf)) { throw "Scheduler executable missing: $scheduler" }
    Push-Location ([string]$settings.bin_dir)
    try { & $scheduler ([string]$settings.scheduler_config_path) }
    finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { throw "Scheduler exited with code $LASTEXITCODE; inspect runtime\logs\scheduler.log" }
} elseif (-not $NoWait) {
    Wait-Process -Id $process.Id
}
