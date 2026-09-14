# Compatibility entry point for the isolated, real client/server regression.
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$BuildDir = '',
    [string]$Config = 'Debug',
    [string]$Python = '',
    [string]$PgBin = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ToolSupport.ps1')
if (-not $BuildDir) { $BuildDir = Join-Path $ProjectRoot 'build/rt' }
if (-not $PgBin) { $PgBin = Join-Path $ProjectRoot 'third_party/pgsql/bin' }
$Python = Resolve-ProjectTool $Python @('python.exe', 'python') 'Python 3'
$test = Join-Path $ProjectRoot 'test/test_runtime_integration.py'
$bin = Join-Path $BuildDir "bin/$Config"
Invoke-NativeCommand $Python @($test, '--bin-dir', $bin, '--pg-bin', $PgBin, '-v',
    'RuntimeIntegrationTests.test_pause_maintains_heartbeat_and_server_restart_recovers_session') | Out-Host
