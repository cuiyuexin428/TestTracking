# Shared script boundaries for executable discovery, SQL, and atomic files.
function Resolve-ProjectTool {
    param([string]$Explicit, [string[]]$Candidates, [string]$Name)
    if ($Explicit) { $Candidates = @($Explicit) }
    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
        $command = Get-Command $candidate -CommandType Application -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    throw "$Name not found; supply its executable path explicitly"
}

function Resolve-ProjectPsql([string]$Explicit = '') {
    $root = Split-Path -Parent $PSScriptRoot
    $candidates = @((Join-Path $root 'third_party/pgsql/bin/psql.exe'), 'psql.exe', 'psql')
    if ($env:PG_ROOT) { $candidates = @((Join-Path $env:PG_ROOT 'bin/psql.exe')) + $candidates }
    return Resolve-ProjectTool $Explicit $candidates 'psql (-Psql)'
}

function Invoke-NativeCommand {
    param([string]$Executable, [string[]]$Arguments, [int[]]$SuccessCodes = @(0))
    $errorFile = [IO.Path]::GetTempFileName()
    try {
        $output = @(& $Executable @Arguments 2> $errorFile)
        $code = $LASTEXITCODE
        if ($code -notin $SuccessCodes) {
            $detail = [IO.File]::ReadAllText($errorFile).Trim()
            throw "$(Split-Path -Leaf $Executable) failed (exit $code): $detail"
        }
        return $output
    } finally { Remove-Item -LiteralPath $errorFile -Force -ErrorAction SilentlyContinue }
}

function Invoke-ProjectSql {
    param([string]$Psql, [string]$DbConn, [string]$Sql, [hashtable]$Parameters = @{})
    $sqlFile = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllText($sqlFile, $Sql, [Text.UTF8Encoding]::new($false))
        $arguments = @('-X', '--no-password', '--set=ON_ERROR_STOP=1', '-A', '-t',
                       '--dbname', $DbConn, '--file', $sqlFile)
        foreach ($key in $Parameters.Keys) {
            if ($key -notmatch '^[a-zA-Z_][a-zA-Z0-9_]*$') { throw "Invalid SQL parameter: $key" }
            $arguments += @('--set', "$key=$($Parameters[$key])")
        }
        return Invoke-NativeCommand $Psql $arguments
    } finally { Remove-Item -LiteralPath $sqlFile -Force -ErrorAction SilentlyContinue }
}

function Write-AtomicText {
    param([string]$Path, [AllowEmptyString()][string]$Text)
    $target = [IO.Path]::GetFullPath($Path)
    $parent = Split-Path -Parent $target
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    $temporary = Join-Path $parent ('.' + [IO.Path]::GetFileName($target) + '.' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($temporary, $Text, [Text.UTF8Encoding]::new($false))
        if ([IO.File]::Exists($target)) { [IO.File]::Replace($temporary, $target, [NullString]::Value) }
        else { [IO.File]::Move($temporary, $target) }
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

function Assert-OwnedPath([string]$Path, [string]$Parent) {
    $target = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $owner = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    if (-not $target.StartsWith($owner + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing path outside owned directory '$owner': $target"
    }
    $current = $target
    while ($current -ne $owner) {
        if (Test-Path -LiteralPath $current) {
            if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing reparse point in owned path: $current"
            }
        }
        $current = Split-Path -Parent $current
    }
    return $target
}

function New-PostgresConnection {
    param([string]$HostName, [int]$Port, [string]$Database, [string]$User, [string]$Password = '')
    function Quote-ConnectionValue([string]$Value) {
        return "'" + $Value.Replace('\', '\\').Replace("'", "\'") + "'"
    }
    $conn = "host=$(Quote-ConnectionValue $HostName) port=$Port dbname=$(Quote-ConnectionValue $Database) user=$(Quote-ConnectionValue $User) connect_timeout=5"
    if ($Password) { $conn += " password=$(Quote-ConnectionValue $Password)" }
    return $conn
}
