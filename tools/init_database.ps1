# Creates the database and role used by stp-test-tracking on the local server.
# Run once on the database host before the first scheduler start.
param(
    [string]$Psql = 'C:\WorkSpace\ThirdParty\pgsql\bin\psql.exe',
    [string]$DbHost = '127.0.0.1',
    [int]$DbPort = 5432,
    [string]$DbName = 'testtracking',
    [string]$DbUser = 'postgres',
    [string]$DbPassword = ''
)
$ErrorActionPreference = 'Stop'

function Invoke-Psql([string]$Db, [string]$Sql) {
    & $Psql -h $DbHost -p $DbPort -U $DbUser -d $Db -v ON_ERROR_STOP=1 -c $Sql 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "psql failed against database '$Db': $Sql"
    }
}

# Role is optional when using the existing postgres superuser. When a password
# is supplied, make sure the role exists with that password.
if ($DbPassword) {
    $roleSafe = $DbUser -replace "'", "''"
    $sql = "DO `$`$ BEGIN IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname='$roleSafe') THEN CREATE ROLE `"$roleSafe`" LOGIN; END IF; END `$`$; ALTER ROLE `"$roleSafe`" PASSWORD '$($DbPassword -replace "'", "''")';"
    Invoke-Psql 'postgres' $sql
}

$dbSafe = $DbName -replace "'", "''"
$exists = & $Psql -h $DbHost -p $DbPort -U $DbUser -d postgres -t -A -c "SELECT 1 FROM pg_database WHERE datname='$dbSafe';" 2>$null
if (($exists | Out-String).Trim() -ne '1') {
    $createSql = "CREATE DATABASE `"$dbSafe`""
    if ($DbUser -ne 'postgres') {
        $createSql += " OWNER `"$($DbUser -replace '"', '""')`""
    }
    & $Psql -h $DbHost -p $DbPort -U $DbUser -d postgres -v ON_ERROR_STOP=1 -c $createSql 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "failed to create database $DbName"
    }
    Write-Host "created database $DbName"
} else {
    Write-Host "database $DbName already exists"
}

Write-Host "Database initialization complete."
Write-Host "The scheduler/coverage plugin will create framework and coverage tables automatically on first start."
