# Shared helpers for the e2e scripts under test/e2e.

function Get-BinDir {
    param([string]$BinDir)
    if ($BinDir) {
        return $BinDir
    }
    $candidates = @(
        (Join-Path $PSScriptRoot '..\..\out\build\x64-Debug\bin\Debug'),
        (Join-Path $PSScriptRoot '..\..\out\build\Release\bin\Release'),
        (Join-Path $PSScriptRoot '..\..\build\bin\Release')
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c 'scheduler.exe')) {
            return $c
        }
    }
    throw "Cannot locate scheduler.exe; pass -BinDir <bin>"
}

function New-E2EWorkspace {
    param([string]$Name)
    $dir = Join-Path $env:TEMP ("tt_e2e_" + $Name + "_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force $dir | Out-Null
    return $dir
}

function Start-E2EProcess {
    param([string]$Exe, [string]$CmdArgs, [string]$WorkDir, [string]$LogFile)
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo.FileName = 'cmd.exe'
    $p.StartInfo.Arguments = "/c `"$Exe $CmdArgs > `"$LogFile`" 2>&1`""
    $p.StartInfo.UseShellExecute = $false
    $p.StartInfo.RedirectStandardInput = $true
    $p.StartInfo.CreateNoWindow = $true
    $p.StartInfo.WorkingDirectory = $WorkDir
    [void]$p.Start()
    return $p
}

function Assert-LogContains {
    param([string]$LogFile, [string]$Pattern, [string]$What)
    $content = Get-Content -Raw $LogFile -ErrorAction SilentlyContinue
    if (-not $content -or $content -notmatch $Pattern) {
        Write-Host "[FAIL] ${What}: missing '$Pattern' in $LogFile" -ForegroundColor Red
        exit 1
    }
    Write-Host "[PASS] $What" -ForegroundColor Green
}
