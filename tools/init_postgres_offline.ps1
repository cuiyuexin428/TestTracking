<#
Initializes and starts a project-local PostgreSQL server for offline use.

The server binaries come from third_party\pgsql when present, otherwise the
bundled postgresql-*.zip in third_party_src\downloads is extracted to
runtime\pgsql. The data directory defaults to runtime\pgdata.

Idempotent: an existing data directory and a running server are reused.
Run as administrator to register the Windows service and open the firewall
port; without elevation pg_ctl is used and firewall setup is skipped with a
warning.

Usage:
  powershell -ExecutionPolicy Bypass -File .\tools\init_postgres_offline.ps1
  powershell -ExecutionPolicy Bypass -File .\tools\init_postgres_offline.ps1 -DbPort 5433 -DbName testtracking -NoService
#>
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$PgRoot = '',
    [string]$PgData = '',
    [int]$DbPort = 5432,
    [string]$DbName = 'testtracking',
    [string]$DbUser = 'postgres',
    [string]$DbPassword = '',
    [string]$AuthMethod = 'trust',
    [string]$ClientCidr = '0.0.0.0/0',
    [switch]$NoService,
    [switch]$NoFirewall
)
$ErrorActionPreference = 'Stop'
$serviceName = 'testtracking-postgres'

function Resolve-PgRoot([string]$Root, [string]$Requested) {
    if ($Requested) {
        if (-not (Test-Path (Join-Path $Requested 'bin\initdb.exe'))) {
            throw "initdb.exe not found under $Requested"
        }
        return $Requested
    }
    $bundled = Join-Path $Root 'third_party\pgsql'
    if (Test-Path (Join-Path $bundled 'bin\initdb.exe')) {
        return $bundled
    }
    $target = Join-Path $Root 'runtime\pgsql'
    if (Test-Path (Join-Path $target 'bin\initdb.exe')) {
        return $target
    }
    $downloads = Join-Path $Root 'third_party_src\downloads'
    $pgZip = Get-ChildItem $downloads -Filter 'postgresql-*.zip' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $pgZip) {
        throw 'PostgreSQL server binaries not found; keep third_party\pgsql or postgresql-*.zip inside the package'
    }
    $tmp = Join-Path $Root 'runtime\_pg_extract'
    if (Test-Path $tmp) {
        Remove-Item -LiteralPath $tmp -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Write-Host "Extracting PostgreSQL server: $($pgZip.Name)"
    & tar.exe -xf $pgZip.FullName -C $tmp
    if ($LASTEXITCODE -ne 0) {
        Expand-Archive -Path $pgZip.FullName -DestinationPath $tmp -Force
    }
    $inner = Get-ChildItem $tmp -Directory |
        Where-Object { $_.Name -eq 'pgsql' } |
        Select-Object -First 1
    if (-not $inner) {
        $inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
    }
    if (-not $inner) {
        throw 'Cannot locate the pgsql directory inside the PostgreSQL zip'
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    Move-Item -LiteralPath $inner.FullName -Destination $target
    Remove-Item -LiteralPath $tmp -Recurse -Force
    Write-Host "PostgreSQL server installed: $target"
    return $target
}

function Set-ConfValue([string]$Text, [string]$Key, [string]$Value) {
    $pattern = '(?m)^\s*#?\s*' + [regex]::Escape($Key) + '\s*=.*$'
    $line = "$Key = $Value"
    if ($Text -match $pattern) {
        return [regex]::Replace($Text, $pattern, { param($match) $line })
    }
    return $Text.TrimEnd() + "`r`n" + $line + "`r`n"
}

function Invoke-Psql([string]$PsqlExe, [int]$Port, [string]$User, [string]$Database, [string]$Sql) {
    & $PsqlExe -h 127.0.0.1 -p $Port -U $User -d $Database -v ON_ERROR_STOP=1 -c $Sql 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "psql failed against database '$Database': $Sql"
    }
}

$PgRoot = Resolve-PgRoot $ProjectRoot $PgRoot
if (-not $PgData) {
    $PgData = Join-Path $ProjectRoot 'runtime\pgdata'
}
$bin = Join-Path $PgRoot 'bin'
$initdb = Join-Path $bin 'initdb.exe'
$pgCtl = Join-Path $bin 'pg_ctl.exe'
$psql = Join-Path $bin 'psql.exe'
foreach ($exe in @($initdb, $pgCtl, $psql)) {
    if (-not (Test-Path $exe)) {
        throw "Missing PostgreSQL tool: $exe"
    }
}
Write-Host "PostgreSQL root: $PgRoot"
Write-Host "PostgreSQL data: $PgData"

if (-not (Test-Path (Join-Path $PgData 'PG_VERSION'))) {
    New-Item -ItemType Directory -Force -Path $PgData | Out-Null
    Write-Host 'Initializing PostgreSQL data directory ...'
    & $initdb -D $PgData -U $DbUser -A trust -E UTF8 --locale=C
    if ($LASTEXITCODE -ne 0) {
        throw 'initdb failed'
    }
} else {
    Write-Host 'PostgreSQL data directory already initialized.'
}

$conf = Join-Path $PgData 'postgresql.conf'
$hba = Join-Path $PgData 'pg_hba.conf'
$confText = Get-Content -Raw $conf
$confText = Set-ConfValue $confText 'port' ([string]$DbPort)
$confText = Set-ConfValue $confText 'listen_addresses' "'*'"
[System.IO.File]::WriteAllText($conf, $confText, (New-Object System.Text.UTF8Encoding($false)))

$hbaText = Get-Content -Raw $hba
$rule = "host    all             all             $ClientCidr            $AuthMethod"
if ($hbaText -notmatch [regex]::Escape($rule)) {
    $hbaText = $hbaText.TrimEnd() + "`r`n" + $rule + "`r`n"
    [System.IO.File]::WriteAllText($hba, $hbaText, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Added pg_hba rule: $rule"
} else {
    Write-Host "pg_hba rule already present: $rule"
}

$logFile = Join-Path $PgData 'server.log'
function Test-ServerRunning {
    & $pgCtl status -D $PgData 2>$null | Out-Null
    return ($LASTEXITCODE -eq 0)
}

$wasRunning = Test-ServerRunning
$configChanged = $false
$portLine = Get-Content $conf | Where-Object { $_ -match '^\s*port\s*=' } | Select-Object -First 1
if ($portLine -and $portLine -notmatch [regex]::Escape("port = $DbPort")) {
    $configChanged = $true
}
if ($wasRunning) {
    if ($configChanged) {
        Write-Host 'Restarting PostgreSQL to apply configuration changes ...'
        & $pgCtl restart -D $PgData -m fast -l $logFile -w
        if ($LASTEXITCODE -ne 0) {
            throw 'PostgreSQL restart failed'
        }
    } else {
        Write-Host 'PostgreSQL is already running.'
    }
} else {
    $service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
    $started = $false
    if ($service -and $service.Status -eq 'Stopped') {
        Write-Host "Starting Windows service: $serviceName"
        & net start $serviceName | Out-Host
        if ($LASTEXITCODE -eq 0) {
            $started = $true
        }
    }
    if (-not $started -and $service) {
        Write-Host 'Windows service exists but could not be started; using pg_ctl.'
        & $pgCtl start -D $PgData -l $logFile -w
        if ($LASTEXITCODE -ne 0) {
            throw 'PostgreSQL pg_ctl start failed'
        }
        $started = $true
    }
    if (-not $started -and -not $NoService) {
        Write-Host "Registering Windows service: $serviceName"
        & $pgCtl register -N $serviceName -D $PgData 2>&1 | Out-Host
        if ($LASTEXITCODE -eq 0) {
            & net start $serviceName | Out-Host
            if ($LASTEXITCODE -eq 0) {
                $started = $true
            } else {
                Write-Host 'WARN: service start failed; falling back to pg_ctl.'
            }
        } else {
            Write-Host 'WARN: service registration failed (run as administrator to register); using pg_ctl.'
        }
    }
    if (-not $started) {
        & $pgCtl start -D $PgData -l $logFile -w
        if ($LASTEXITCODE -ne 0) {
            throw 'PostgreSQL pg_ctl start failed'
        }
    }
}

$ready = $false
for ($i = 0; $i -lt 30; $i++) {
    & $psql -h 127.0.0.1 -p $DbPort -U $DbUser -d postgres -t -A -c 'SELECT 1' 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) {
        $ready = $true
        break
    }
    Start-Sleep -Seconds 1
}
if (-not $ready) {
    throw "PostgreSQL did not become ready on port $DbPort"
}
Write-Host "PostgreSQL is ready on 127.0.0.1:$DbPort"

if ($AuthMethod -ne 'trust' -and -not $DbPassword) {
    Write-Host 'WARN: AuthMethod is not trust but no DbPassword was set; remote clients cannot authenticate.'
}

if ($DbPassword) {
    $roleSafe = $DbUser -replace "'", "''"
    $sql = "DO `$`$ BEGIN IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname='$roleSafe') THEN CREATE ROLE `"$roleSafe`" LOGIN; END IF; END `$`$; ALTER ROLE `"$roleSafe`" PASSWORD '$($DbPassword -replace "'", "''")';"
    Invoke-Psql $psql $DbPort $DbUser 'postgres' $sql
}

$dbSafe = $DbName -replace "'", "''"
$exists = & $psql -h 127.0.0.1 -p $DbPort -U $DbUser -d postgres -t -A -c "SELECT 1 FROM pg_database WHERE datname='$dbSafe';" 2>$null
if (($exists | Out-String).Trim() -ne '1') {
    $createSql = "CREATE DATABASE `"$dbSafe`""
    if ($DbUser -ne 'postgres') {
        $createSql += " OWNER `"$($DbUser -replace '"', '""')`""
    }
    & $psql -h 127.0.0.1 -p $DbPort -U $DbUser -d postgres -v ON_ERROR_STOP=1 -c $createSql 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "failed to create database $DbName"
    }
    Write-Host "Created database: $DbName"
} else {
    Write-Host "Database already exists: $DbName"
}

if (-not $NoFirewall) {
    & netsh advfirewall firewall delete rule name="TestTracking PostgreSQL $DbPort" 2>$null | Out-Null
    & netsh advfirewall firewall add rule name="TestTracking PostgreSQL $DbPort" dir=in action=allow protocol=TCP localport=$DbPort | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'WARN: firewall rule failed; run as administrator or open the port manually.'
    } else {
        Write-Host "Firewall: inbound TCP $DbPort allowed."
    }
}

Write-Host 'Database initialization complete.'
Write-Host "Connection: host=127.0.0.1 port=$DbPort dbname=$DbName user=$DbUser"
Write-Host 'The scheduler/coverage plugin will create framework and coverage tables automatically on first start.'