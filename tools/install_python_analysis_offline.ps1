# Installs the pinned analysis stack into a portable embedded Python runtime.
[CmdletBinding()]
param(
    [string]$RuntimeDir = '',
    [string]$Wheelhouse = '',
    [string]$SupportScript = ''
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
if (-not $RuntimeDir) { $RuntimeDir = Join-Path $root 'runtime\python' }
if (-not $Wheelhouse) { $Wheelhouse = Join-Path $root 'runtime\python-analysis-wheelhouse' }
if (-not $SupportScript) { $SupportScript = Join-Path $PSScriptRoot 'python_analysis_support.py' }
$RuntimeDir = [IO.Path]::GetFullPath($RuntimeDir)
$Wheelhouse = [IO.Path]::GetFullPath($Wheelhouse)
$SupportScript = [IO.Path]::GetFullPath($SupportScript)
$python = Join-Path $RuntimeDir 'python.exe'
$pip = Join-Path $Wheelhouse 'pip.pyz'
$requirements = Join-Path $Wheelhouse 'requirements.txt'
$installedManifest = Join-Path $RuntimeDir 'analysis-runtime-manifest.json'

foreach ($required in @($python, $pip, $requirements, $SupportScript)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required file not found: $required" }
}
$pathFile = Get-ChildItem -LiteralPath $RuntimeDir -File -Filter 'python*._pth' | Select-Object -First 1
if (-not $pathFile -or -not (Select-String -LiteralPath $pathFile.FullName -Pattern '^import site\s*$' -Quiet)) {
    throw "Embedded Python must enable site-packages with 'import site' in its ._pth file: $RuntimeDir"
}

& $python -B -I $SupportScript verify-target
if ($LASTEXITCODE -ne 0) { throw "Python target verification failed (exit $LASTEXITCODE)" }
& $python -B -I $SupportScript verify-wheelhouse --wheelhouse $Wheelhouse
if ($LASTEXITCODE -ne 0) { throw "Analysis wheelhouse verification failed (exit $LASTEXITCODE)" }
& $python -B -I $pip install --disable-pip-version-check --no-warn-script-location --no-index --no-compile `
    --find-links $Wheelhouse --requirement $requirements
if ($LASTEXITCODE -ne 0) { throw "Offline analysis package installation failed (exit $LASTEXITCODE)" }
& $python -B -I $SupportScript verify-runtime --requirements $requirements --out $installedManifest
if ($LASTEXITCODE -ne 0) { throw "Analysis runtime verification failed (exit $LASTEXITCODE)" }
Write-Host "Installed and verified offline analysis runtime: $installedManifest"
