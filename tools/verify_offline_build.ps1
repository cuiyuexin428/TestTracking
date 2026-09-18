# Verifies that the project can be configured and built on a machine with no
# internet access, using only what ships with the project folder.
#
# Checks, in order:
#   1. cmake + MSVC toolchain availability
#   2. third-party installs (or rebuilds them offline from third_party_src)
#   3. PostgreSQL client libpq (project-local third_party/pgsql, PG_ROOT env,
#      or common install paths)
#   4. cmake configure + build (Release by default)
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$BuildDir = "",
    [string]$PgRoot = "",
    [switch]$SkipThirdParty,
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [switch]$SkipBuild
)
$ErrorActionPreference = "Stop"

if (-not $BuildDir) {
    $BuildDir = Join-Path $ProjectRoot "build\offline_verify"
}

$failures = 0
function Check([string]$Title, [bool]$Ok, [string]$Detail) {
    if ($Ok) { Write-Host "[PASS] $Title" } else { Write-Host "[FAIL] $Title - $Detail"; $script:failures++ }
}

Write-Host "== Project root: $ProjectRoot =="
Write-Host "== 1. toolchain =="
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
Check "cmake available" ($null -ne $cmake) "cmake not found on PATH"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsFound = $false
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    $vsFound = -not [string]::IsNullOrWhiteSpace($vsPath)
}
Check "MSVC C++ tools installed" $vsFound "Visual Studio C++ desktop workload not found"

Write-Host "== 2. third-party installs =="
$tp = Join-Path $ProjectRoot "third_party"
$tpChecks = @(
    "libzmq-4.3.5/CMake/ZeroMQConfig.cmake",
    "libzmq-4.3.5/CMake/ZeroMQTargets-debug.cmake",
    "abseil-cpp-20230802.3/lib/cmake/absl/abslConfig.cmake",
    "protobuf-25.3/cmake/protobuf-config.cmake",
    "concurrentqueue/concurrentqueue.h",
    "nlohmann/json.hpp",
    "tinyxml2/tinyxml2.h"
)
$tpComplete = $true
$missingThirdParty = @()
foreach ($rel in $tpChecks) {
    $ok = Test-Path (Join-Path $tp $rel)
    if (-not $ok) { $tpComplete = $false; $missingThirdParty += $rel }
    Check $rel $ok "missing under third_party"
}
if (-not $tpComplete -and -not $SkipThirdParty) {
    Write-Host "== rebuilding third-party offline =="
    & (Join-Path $PSScriptRoot "build_third_party.ps1") -ProjectRoot $ProjectRoot
    # Replace preliminary missing-file failures with the post-build checks.
    $failures -= $missingThirdParty.Count
    foreach ($rel in $tpChecks) {
        Check $rel (Test-Path (Join-Path $tp $rel)) "still missing after rebuild"
    }
}

Write-Host "== 3. PostgreSQL client (libpq) =="
if (-not $PgRoot) { $PgRoot = $env:PG_ROOT }
if (-not $PgRoot) {
    $candidates = @(
        (Join-Path $tp "pgsql"),
        "C:/Program Files/PostgreSQL",
        "C:/pgsql",
        "C:/WorkSpace/ThirdParty/pgsql"
    )
    foreach ($cand in $candidates) {
        if (Test-Path (Join-Path $cand "include\libpq-fe.h")) { $PgRoot = $cand; break }
        $subs = Get-ChildItem $cand -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName "include\libpq-fe.h") } |
            Select-Object -First 1
        if ($subs) { $PgRoot = $subs.FullName; break }
    }
}
Check "libpq found" (Test-Path (Join-Path $PgRoot "include\libpq-fe.h")) "PG_ROOT not found; copy third_party/pgsql or set PG_ROOT"

if ($SkipBuild) {
    if ($failures -eq 0) { Write-Host "RESULT: ALL PASS (checks only)" } else { Write-Host "RESULT: $failures FAILED" }
    exit $failures
}

Write-Host "== 4. configure + build =="
$cfgArgs = @("-S", $ProjectRoot, "-B", $BuildDir, "-A", "x64")
if ($PgRoot) { $cfgArgs += "-DPG_ROOT=$PgRoot" }
$bundledPython = Join-Path $ProjectRoot 'runtime\python\python.exe'
if (Test-Path -LiteralPath $bundledPython -PathType Leaf) {
    $cfgArgs += "-DPython3_EXECUTABLE=$bundledPython"
}
& cmake @cfgArgs
if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] cmake configure failed"; exit 1 }
& cmake --build $BuildDir --config $Config --parallel 8
if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] cmake build failed"; exit 1 }
Write-Host "[PASS] configure + build ($Config) succeeded"
if ($failures -eq 0) { Write-Host "RESULT: ALL PASS" } else { Write-Host "RESULT: $failures FAILED" }
exit $failures
