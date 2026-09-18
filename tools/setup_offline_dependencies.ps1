<#
Install the two portable dependency stacks used by TestTracking on an offline
Windows x64 server: Python analysis libraries and DeepSeek Harness (DSH).

Copy TestTracking and the prepared dsh-offline-bundle to the target LAN first.
The script never invokes a package registry. DSH still needs an OpenAI-compatible
model endpoint reachable inside the LAN.
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$DshBundleDir = '',
    [string]$LlmBaseUrl = '',
    [string]$LlmModel = '',
    [string]$LlmApiKey = '',
    [string]$DshInstallDir = '',
    [string]$DshHome = '',
    [string]$DshWorkspace = '',
    [switch]$SkipPythonAnalysis,
    [switch]$SkipDsh,
    [switch]$StartDsh
)
$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) {
    throw 'Offline dependencies require Windows x64.'
}
$ProjectRoot = if ($ProjectRoot) {
    [IO.Path]::GetFullPath($ProjectRoot)
} else {
    [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
}

function Resolve-DshBundle([string]$Requested) {
    $candidates = @()
    if ($Requested) {
        $candidates += $Requested
    } else {
        $parent = Split-Path -Parent $ProjectRoot
        $candidates += @(
            (Join-Path $parent 'dsh-inter\dsh-offline-bundle\dist'),
            (Join-Path $parent 'dsh-offline-bundle\dist'),
            (Join-Path $parent 'dsh\dist')
        )
    }
    foreach ($candidate in $candidates) {
        if (-not [IO.Path]::IsPathRooted($candidate)) {
            $candidate = Join-Path $ProjectRoot $candidate
        }
        $candidate = [IO.Path]::GetFullPath($candidate)
        if (Test-Path -LiteralPath (Join-Path $candidate 'deploy-offline.ps1') -PathType Leaf) {
            return $candidate
        }
    }
    throw 'DSH offline dist was not found. Pass -DshBundleDir <dsh-offline-bundle\dist>.'
}

Write-Host '=== TestTracking offline dependency setup ==='
Write-Host "Project: $ProjectRoot"

if (-not $SkipPythonAnalysis) {
    Write-Host ''
    Write-Host '=== Python analysis dependencies ==='
    $installer = Join-Path $ProjectRoot 'tools\install_python_analysis_offline.ps1'
    if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
        throw "Python analysis installer not found: $installer"
    }
    & $installer `
        -RuntimeDir (Join-Path $ProjectRoot 'runtime\python') `
        -Wheelhouse (Join-Path $ProjectRoot 'runtime\python-analysis-wheelhouse') `
        -SupportScript (Join-Path $ProjectRoot 'tools\python_analysis_support.py')
}

if (-not $SkipDsh) {
    Write-Host ''
    Write-Host '=== DSH offline runtime ==='
    if (-not $LlmBaseUrl) { throw 'Pass -LlmBaseUrl with the LAN OpenAI-compatible /v1 endpoint.' }
    if (-not $LlmModel) { throw 'Pass -LlmModel with the model id exposed by the LAN endpoint.' }
    $bundle = Resolve-DshBundle $DshBundleDir
    foreach ($required in @(
        'runtime\node_modules\@deepseek-ai\dsh\lib\bin.js',
        'runtime\VERSION.json',
        'deploy-offline.ps1'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $bundle $required) -PathType Leaf)) {
            throw "DSH bundle is incomplete; missing: $required"
        }
    }
    $node = @(Get-ChildItem -LiteralPath (Join-Path $bundle 'node') -Filter node.exe -File -Recurse)
    if ($node.Count -ne 1) { throw "DSH bundle must contain exactly one node.exe; found $($node.Count)." }

    $arguments = @{
        BundleDir = $bundle
        LlmBaseUrl = $LlmBaseUrl
        LlmModel = $LlmModel
        LlmApiKey = $LlmApiKey
        NoStart = (-not $StartDsh)
    }
    if ($DshInstallDir) { $arguments.InstallDir = [IO.Path]::GetFullPath($DshInstallDir) }
    if ($DshHome) { $arguments.DshHome = [IO.Path]::GetFullPath($DshHome) }
    if ($DshWorkspace) { $arguments.WorkspaceRoot = [IO.Path]::GetFullPath($DshWorkspace) }
    & (Join-Path $bundle 'deploy-offline.ps1') @arguments

    $resolvedInstall = if ($DshInstallDir) {
        [IO.Path]::GetFullPath($DshInstallDir)
    } else {
        Join-Path $env:LOCALAPPDATA 'dsh'
    }
    $dshCommand = Join-Path $resolvedInstall 'dsh.cmd'
    if (-not (Test-Path -LiteralPath $dshCommand -PathType Leaf)) {
        throw "DSH deployment did not create the CLI wrapper required by TestTracking: $dshCommand"
    }
    Write-Host "DSH CLI: $dshCommand"
    if ($DshInstallDir) {
        Write-Host 'For this custom installation, set providers.dsh.command in the result service config to the CLI path above.'
    }
}

Write-Host ''
Write-Host 'Offline dependency setup completed.'
