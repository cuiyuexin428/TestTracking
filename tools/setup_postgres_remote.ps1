# Prepares the local PostgreSQL instance for remote client-package machines.
# The client writes coverage rows directly to this database, so PostgreSQL must
# listen on the LAN interface and pg_hba.conf must allow the client subnet.
param(
    [string]$PgData = 'C:\WorkSpace\ThirdParty\pgdata',
    [string]$PgBin = 'C:\WorkSpace\ThirdParty\pgsql\bin',
    [string]$AuthMethod = 'scram-sha-256',
    [string]$ClientCidr = '',
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'

if (-not $ClientCidr) {
    throw 'ClientCidr is required, e.g. "192.168.100.0/24" for your LAN or "0.0.0.0/0" for all hosts.'
}

$conf = Join-Path $PgData 'postgresql.conf'
$hba = Join-Path $PgData 'pg_hba.conf'
$pgCtl = Join-Path $PgBin 'pg_ctl.exe'
$psql = Join-Path $PgBin 'psql.exe'

if (-not (Test-Path $conf)) { throw "postgresql.conf not found: $conf" }
if (-not (Test-Path $hba)) { throw "pg_hba.conf not found: $hba" }
if (-not (Test-Path $pgCtl)) { throw "pg_ctl.exe not found: $pgCtl" }

$stamp = Get-Date -Format 'yyyyMMddHHmmss'
Copy-Item $conf "$conf.bak.$stamp"
Copy-Item $hba "$hba.bak.$stamp"

# 1. listen_addresses = '*'
$confText = Get-Content -Raw $conf
$pattern = '(?m)^\s*#?\s*listen_addresses\s*=.*$'
$replacement = "listen_addresses = '*'"
if ($confText -match $pattern) {
    $confText = [regex]::Replace($confText, $pattern, $replacement)
} else {
    $confText = $confText.TrimEnd() + "`r`n" + $replacement + "`r`n"
}
[System.IO.File]::WriteAllText($conf, $confText, (New-Object System.Text.UTF8Encoding($false)))

# 2. pg_hba.conf rule for remote clients.
$hbaText = Get-Content -Raw $hba
$rule = "host    all             all             $ClientCidr            $AuthMethod"
if ($hbaText -notmatch [regex]::Escape($rule)) {
    $hbaText = $hbaText.TrimEnd() + "`r`n" + $rule + "`r`n"
    [System.IO.File]::WriteAllText($hba, $hbaText, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Added pg_hba rule: $rule"
} else {
    Write-Host "pg_hba rule already present: $rule"
}
if ($AuthMethod -ne 'trust') {
    Write-Host "NOTE: existing earlier pg_hba rules are matched first; set a password for the DB user when AuthMethod is not trust."
}

if ($NoRestart) {
    Write-Host "PostgreSQL config updated. Restart PostgreSQL manually or run:"
    Write-Host "  & '$pgCtl' restart -D '$PgData' -m fast"
    exit 0
}

& $pgCtl restart -D $PgData -m fast
if ($LASTEXITCODE -ne 0) {
    throw "pg_ctl restart failed with exit code $LASTEXITCODE"
}
Write-Host "PostgreSQL restarted and is ready for remote connections on this machine."
Write-Host "Remember to set a password for the database user when AuthMethod is not trust."
