# Recursively builds the dataset manifest used as the scheduler's stp_manifest.
# Each JSONL row records path, byte size, and SHA-256. Only .stp files are
# included so version.txt / manifest.txt never become tasks. The scheduler
# still accepts the legacy "path,size" format for existing deployments.
param(
    [string]$DatasetDir = 'C:\Shared\dataset',
    [string]$OutFile = ''
)
$ErrorActionPreference = 'Stop'

function Get-Sha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        return ([System.BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

if (-not $OutFile) {
    $OutFile = Join-Path $DatasetDir 'manifest.txt'
}
$root = (Resolve-Path $DatasetDir).Path

$lines = Get-ChildItem $DatasetDir -File -Filter *.stp -Recurse | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
    $hash = Get-Sha256Hex -Path $_.FullName
    [ordered]@{ path = $rel; size_bytes = $_.Length; content_sha256 = $hash } |
        ConvertTo-Json -Compress
}
[System.IO.File]::WriteAllLines(
    $OutFile,
    $lines,
    (New-Object System.Text.UTF8Encoding($false)))

Write-Host "dataset manifest: $($lines.Count) stp files -> $OutFile"
