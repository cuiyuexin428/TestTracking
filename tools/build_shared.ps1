# Builds the authoritative shared content under a shared root:
#   <SharedDir>/program    StepTest + OCCT DLLs/PDBs + OpenCppCoverage +
#                          occtlib resources + manifest.txt (name,size,sha256)
#   <SharedDir>/dataset    the stp dataset + manifest.txt (name,size)
# The scheduler serves these from this directory (or its network share) and
# clients pull them during registration. The copyable client package is
# produced separately by tools/pack_client.ps1.
param(
    [string]$SharedDir = 'C:\Shared',
    [int]$StpCount = 0,
    [string[]]$OcctDlls = @('TKernel.dll', 'TKBRep.dll', 'TKDESTEP.dll', 'TKTopAlgo.dll'),
    [string]$StepTestExe = 'C:\WorkSpace\occt_demo\build\Debug\StepTest.exe',
    [string]$OcctSrcDir = 'C:\WorkSpace\occt_demo\occtlib\src',
    [string]$DatasetDir = 'C:\WorkSpace\occt_demo\step',
    [string]$OpenCppCoverageDir = 'C:\Program Files\OpenCppCoverage'
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $StepTestExe)) {
    throw "StepTest.exe not found: $StepTestExe"
}
if (-not (Test-Path $OcctSrcDir)) {
    throw "OCCT resources not found: $OcctSrcDir"
}
if (-not (Test-Path (Join-Path $OpenCppCoverageDir 'OpenCppCoverage.exe'))) {
    throw "OpenCppCoverage.exe not found under $OpenCppCoverageDir"
}

$sharedProgram = Join-Path $SharedDir 'program'
$sharedDataset = Join-Path $SharedDir 'dataset'
foreach ($d in @($sharedProgram, $sharedDataset)) {
    if (Test-Path $d) {
        Write-Host "Removing existing dir: $d"
        Remove-Item -LiteralPath $d -Recurse -Force
    }
}
New-Item -ItemType Directory -Force $sharedProgram | Out-Null
New-Item -ItemType Directory -Force $sharedDataset | Out-Null

# Program package.
Copy-Item -LiteralPath $StepTestExe -Destination $sharedProgram
$stepPdb = [System.IO.Path]::ChangeExtension($StepTestExe, '.pdb')
if (Test-Path $stepPdb) {
    Copy-Item -LiteralPath $stepPdb -Destination $sharedProgram
}
$stepDir = Split-Path $StepTestExe -Parent
foreach ($dll in $OcctDlls) {
    $pdbPath = [System.IO.Path]::ChangeExtension((Join-Path $stepDir $dll), '.pdb')
    if (Test-Path $pdbPath) {
        Copy-Item -LiteralPath $pdbPath -Destination $sharedProgram
    }
}
# StepTest loads all OCCT dependencies transitively; ship every DLL. PDBs are
# only needed for the modules that will be instrumented (copied above).
Get-ChildItem $stepDir -Filter '*.dll' | Copy-Item -Destination $sharedProgram
Copy-Item -LiteralPath $OpenCppCoverageDir -Destination $sharedProgram -Recurse
Get-ChildItem (Join-Path $sharedProgram 'OpenCppCoverage') -Filter 'unins*' | Remove-Item -Force
Copy-Item -Path $OcctSrcDir -Destination (Join-Path $sharedProgram 'occtlib') -Recurse

# Dataset (all stp files by default; -StpCount > 0 limits the copy).
$stpFiles = Get-ChildItem $DatasetDir -File -Filter *.stp | Sort-Object Name
if ($StpCount -gt 0) {
    $stpFiles = $stpFiles | Select-Object -First $StpCount
}
if ($stpFiles.Count -lt 2) {
    throw "Need at least 2 stp files under $DatasetDir"
}
if ($StpCount -eq 0 -and (Get-Command robocopy -ErrorAction SilentlyContinue)) {
    & robocopy $DatasetDir $sharedDataset *.stp /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) {
        throw "robocopy failed with exit code $LASTEXITCODE"
    }
} else {
    foreach ($f in $stpFiles) {
        Copy-Item -LiteralPath $f.FullName -Destination $sharedDataset
    }
}

# Program manifest: relative path,size,sha256 for every packaged file.
$programManifest = @()
foreach ($f in Get-ChildItem $sharedProgram -Recurse -File | Sort-Object FullName) {
    $rel = $f.FullName.Substring($sharedProgram.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLower()
    $programManifest += "$rel,$($f.Length),$hash"
}
[System.IO.File]::WriteAllLines(
    (Join-Path $sharedProgram 'manifest.txt'),
    $programManifest,
    (New-Object System.Text.UTF8Encoding($false)))

$datasetManifest = Get-ChildItem $sharedDataset -File -Filter *.stp | Sort-Object Name | ForEach-Object {
    "$($_.Name),$($_.Length)"
}
[System.IO.File]::WriteAllLines(
    (Join-Path $sharedDataset 'manifest.txt'),
    $datasetManifest,
    (New-Object System.Text.UTF8Encoding($false)))

Write-Host "Shared dir : $SharedDir"
Write-Host "  program  : $($programManifest.Count) files"
Write-Host "  dataset  : $($stpFiles.Count) files"
Write-Host "Next       : tools/pack_client.ps1 -OutDir $SharedDir\client_package ..."
