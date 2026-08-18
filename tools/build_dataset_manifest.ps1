# Recursively builds the dataset manifest used as the scheduler's stp_manifest.
# Lines are "<relative path with / separators>,<size>". Only .stp files are
# included so version.txt / manifest.txt never become tasks.
param(
    [string]$DatasetDir = 'C:\Shared\dataset',
    [string]$OutFile = ''
)
$ErrorActionPreference = 'Stop'

if (-not $OutFile) {
    $OutFile = Join-Path $DatasetDir 'manifest.txt'
}
$root = (Resolve-Path $DatasetDir).Path

$lines = Get-ChildItem $DatasetDir -File -Filter *.stp -Recurse | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
    "$rel,$($_.Length)"
}
[System.IO.File]::WriteAllLines(
    $OutFile,
    $lines,
    (New-Object System.Text.UTF8Encoding($false)))

Write-Host "dataset manifest: $($lines.Count) stp files -> $OutFile"
