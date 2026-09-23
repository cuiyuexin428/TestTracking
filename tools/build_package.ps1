# Builds the full deployment package (or just one component) under OutDir:
#
#   <OutDir>\
#     client.exe / common.dll / coverage.dll / runtime DLLs
#     config\client.json
#     start_client.bat
#     testenv\
#       program\            coverage program, or baseline/candidate STP programs
#       dataset\            stp files + version.txt
#       opencppcoverage\    OpenCppCoverage.exe + dependencies
#
# Versions are tracked beside each packaged program and in
# testenv\dataset\version.txt. Update only the component that changed:
#   -OnlyClient   update binaries; preserve deployment config unless explicitly overridden
#   -OnlyProgram  replace only the selected plugin's programs and bump their versions
#   -OnlyDataset  replace testenv\dataset and bump its version
param(
    [string]$OutDir = '',
    [string]$BinDir = $null,
    [string]$Endpoint = '',
    [string]$DeploymentEndpoint = '',
    [string]$WorkerId = 'worker-1',
    [ValidateSet('coverage', 'stpcompare', 'all')][string]$Plugin = 'coverage',
    [string]$BaselineConverterExe = '',
    [string]$CandidateConverterExe = '',
    [int]$TaskThreads = 2,
    [string]$StoreType = 'postgres',
    [string]$DbConn = '',
    [string]$ServerIp = '',
    [int]$ServerPort = 5555,
    [int]$DbPort = 5432,
    [string]$DbName = 'testtracking',
    [string]$DbUser = 'postgres',
    [string]$DbPassword = '',
    [switch]$StoreCov,
    [switch]$OnlyClient,
    [switch]$OnlyProgram,
    [switch]$OnlyDataset,
    [switch]$Bootstrap,
    [string]$ProgramVersion = '1.0',
    [string]$BaselineProgramVersion = '1.0',
    [string]$CandidateProgramVersion = '1.0',
    [string]$DatasetVersion = 'd1',
    [string[]]$EngineDlls = @('TKernel.dll', 'TKBRep.dll', 'TKDESTEP.dll', 'TKTopAlgo.dll'),
    # These inputs belong to the deployment being packaged. Keep them empty
    # by default so a package build never silently captures a developer's
    # workstation paths.
    [string]$StepTestExe = '',

    [string]$EngineSrcDir = '',
    [string]$DatasetDir = '',
    [string]$OpenCppCoverageDir = '',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [string]$VCRuntimeDir = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ToolSupport.ps1')
. (Join-Path $PSScriptRoot 'PackageSupport.ps1')
$programInputProvided = [bool]($StepTestExe -or $BaselineConverterExe -or $CandidateConverterExe)
$root = [System.IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))

function Resolve-ProjectPath([string]$Path) {
    if (-not $Path) { return '' }
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $root $Path))
}

if (@($OnlyClient, $OnlyProgram, $OnlyDataset).Where({ $_ }).Count -gt 1) {
    throw 'Choose at most one of -OnlyClient, -OnlyProgram, -OnlyDataset'
}
if ($Bootstrap -and ($OnlyClient -or $OnlyProgram -or $OnlyDataset)) {
    throw '-Bootstrap creates a fresh base client; do not combine it with component update switches.'
}
if ($Config -ne 'Release') {
    throw 'Deployment packages require -Config Release; Debug runtimes cannot be redistributed'
}

$OutDir = if ($OutDir) { Resolve-ProjectPath $OutDir } else { Join-Path $root 'dist\client_package' }
if ($Bootstrap) {
    $serviceUri = $null
    if (-not [Uri]::TryCreate($DeploymentEndpoint, [UriKind]::Absolute, [ref]$serviceUri) -or
        $serviceUri.Scheme -notin @('http', 'https')) {
        throw '-Bootstrap requires -DeploymentEndpoint with the HTTP(S) result service address.'
    }
    if ((Test-Path -LiteralPath $OutDir) -and @(Get-ChildItem -LiteralPath $OutDir -Force).Count -gt 0) {
        throw '-Bootstrap requires an empty output directory; existing client configurations must be preserved.'
    }
    $BinDir = Resolve-PackageBinaryDir -ProjectRoot $root -Explicit $BinDir -Executable 'client.exe' -Config $Config
    if (-not (Test-Path -LiteralPath (Join-Path $BinDir 'common.dll') -PathType Leaf)) {
        throw "Client framework runtime not found: $BinDir\common.dll"
    }
    $VCRuntimeDir = Resolve-ProjectPath $VCRuntimeDir
    New-Item -ItemType Directory -Force (Join-Path $OutDir 'config'), (Join-Path $OutDir 'tools') | Out-Null
    Copy-Item -LiteralPath (Join-Path $BinDir 'client.exe') -Destination $OutDir
    # Bootstrap contains framework dependencies only. Business plugin DLLs and
    # programs arrive through the deployment service after client registration.
    Get-ChildItem -LiteralPath $BinDir -File -Filter '*.dll' | Where-Object {
        $_.Name -match '^(common|libprotobuf|abseil_dll|libpq|libssl-.*|libcrypto-.*|libiconv.*|libintl.*|libwinpthread.*|libzmq.*|zlib.*)\.dll$'
    } | Copy-Item -Destination $OutDir
    Copy-VCRuntime -Destination $OutDir -RuntimeDir $VCRuntimeDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'client_update.ps1') -Destination (Join-Path $OutDir 'tools')
    $bootstrapHost = if ($ServerIp) { $ServerIp } else { $serviceUri.Host }
    if ($bootstrapHost.Contains(':') -and -not $bootstrapHost.StartsWith('[')) { $bootstrapHost = '[' + $bootstrapHost + ']' }
    $bootstrapEndpoint = if ($Endpoint) { $Endpoint } else { "tcp://${bootstrapHost}:$ServerPort" }
    $bootstrapId = if ($PSBoundParameters.ContainsKey('WorkerId')) { $WorkerId } else { 'auto' }
    $bootstrapConfig = @{
        scheduler = @{ endpoint = $bootstrapEndpoint; poll_timeout_ms = 50 }
        client = @{ worker_id = $bootstrapId; deployment_endpoint = $DeploymentEndpoint.TrimEnd('/');
                    deployment_poll_seconds = 3; deployment_stage_timeout_seconds = 0;
                    heartbeat_interval_ms = 1000; task_threads = $TaskThreads;
                    execution_windows = @(); update_windows = @() }
        plugins = @{}
    }
    Write-AtomicText (Join-Path $OutDir 'config\client.json') ($bootstrapConfig | ConvertTo-Json -Depth 10)
    @"
@echo off
cd /d "%~dp0"
client.exe config\client.json
pause
"@ | Set-Content -LiteralPath (Join-Path $OutDir 'start_client.bat') -Encoding ASCII
    @"
# Bootstrap client

Copy this directory before first startup to each client and run start_client.bat.
The first startup replaces worker_id=auto with a persistent identity unique to that copy.
The Agent is initially online without plugins; configure its plugins and data from the server interface.

The scheduler address defaults to the DeploymentEndpoint host with TCP port $ServerPort.
For a custom scheduler router port, build with -ServerPort <port>; -Endpoint overrides the complete address.
The deployment service address and scheduler address remain editable in config/client.json.
"@ | Set-Content -LiteralPath (Join-Path $OutDir 'README.md') -Encoding UTF8
    Write-Host "built bootstrap client: $OutDir; first start creates a persistent client identity"
    return
}
$clientConfigPath = Join-Path $OutDir 'config\client.json'
$previousConfig = $null
if (($OnlyClient -or $OnlyProgram -or $OnlyDataset) -and (Test-Path -LiteralPath $clientConfigPath)) {
    $previousConfig = Get-Content -LiteralPath $clientConfigPath -Raw | ConvertFrom-Json
    if (-not $PSBoundParameters.ContainsKey('Plugin')) {
        $enabled = @($previousConfig.plugins.PSObject.Properties | Where-Object { $_.Value.enabled })
        if ($enabled.Count -eq 1) { $Plugin = $enabled[0].Name }
        elseif ($enabled.Count -gt 1) {
            if ($OnlyProgram) { throw 'Multiple plugins are enabled; pass -Plugin coverage, stpcompare or all for a program update.' }
            $Plugin = 'all'
        }
    }
}
$preserveClientConfig = $OnlyClient -and $null -ne $previousConfig

if (($OnlyProgram -or $OnlyDataset) -and $null -ne $previousConfig) {
    $ignoredParameters = @('Endpoint', 'DeploymentEndpoint', 'ServerIp', 'ServerPort',
        'DbConn', 'DbPort', 'DbName', 'DbUser', 'DbPassword') |
        Where-Object { $PSBoundParameters.ContainsKey($_) }
    if ($ignoredParameters.Count -gt 0) {
        throw "Component updates do not rewrite deployment endpoints or database settings; remove: $($ignoredParameters -join ', ')"
    }
}

$selectedPluginNames = if ($Plugin -eq 'all') { @('coverage', 'stpcompare') } else { @($Plugin) }
$includeCoverage = 'coverage' -in $selectedPluginNames
$includeStpCompare = 'stpcompare' -in $selectedPluginNames
# Program-only updates retain the deployed module list unless explicitly
# overridden. Use the same list for symbols and the rewritten config paths.
$effectiveEngineDlls = if ($OnlyProgram -and -not $PSBoundParameters.ContainsKey('EngineDlls') -and
    $previousConfig.plugins.coverage.engine_dlls) {
    @($previousConfig.plugins.coverage.engine_dlls | ForEach-Object { [IO.Path]::GetFileName([string]$_) })
} else { $EngineDlls }
if ($includeStpCompare) {
    if ((-not $BaselineConverterExe -or -not $CandidateConverterExe) -and
        -not $OnlyClient -and -not $OnlyDataset) {
        throw 'Pass both -BaselineConverterExe and -CandidateConverterExe for a stpcompare program package'
    }
}
function Get-LocalIPv4 {
    $candidates = @()
    foreach ($nic in [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
        if ($nic.OperationalStatus -ne [System.Net.NetworkInformation.OperationalStatus]::Up) { continue }
        foreach ($addr in $nic.GetIPProperties().UnicastAddresses) {
            if ($addr.Address.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) { continue }
            $ip = $addr.Address.ToString()
            if ($ip -eq '127.0.0.1') { continue }
            if ($ip.StartsWith('169.254.')) { continue }
            $candidates += $ip
        }
    }
    if ($candidates.Count -gt 0) { return $candidates }
    throw 'Cannot auto-detect server IPv4 address; pass -ServerIp or -Endpoint.'
}

if (-not $preserveClientConfig -and -not $OnlyProgram -and -not $OnlyDataset -and -not $ServerIp -and (-not $Endpoint -or (-not $DbConn -and $StoreType -eq 'postgres'))) {
    $candidateIps = @(Get-LocalIPv4)
    $ServerIp = $candidateIps[0]
    Write-Host "detected IPv4 candidates: $($candidateIps -join ', '); using $ServerIp (override with -ServerIp)"
}
if (-not $Endpoint -and -not $preserveClientConfig) { $Endpoint = "tcp://${ServerIp}:$ServerPort" }
if (-not $DbConn -and $StoreType -eq 'postgres' -and -not $preserveClientConfig) {
    $DbConn = New-PostgresConnection $ServerIp $DbPort $DbName $DbUser $DbPassword
}

$StepTestExe = Resolve-ProjectPath $StepTestExe
$BaselineConverterExe = Resolve-ProjectPath $BaselineConverterExe
$CandidateConverterExe = Resolve-ProjectPath $CandidateConverterExe
$EngineSrcDir = Resolve-ProjectPath $EngineSrcDir
$DatasetDir = Resolve-ProjectPath $DatasetDir
$OpenCppCoverageDir = Resolve-ProjectPath $OpenCppCoverageDir
$VCRuntimeDir = Resolve-ProjectPath $VCRuntimeDir
if (-not $OnlyProgram -and -not $OnlyDataset) {
    $BinDir = Resolve-PackageBinaryDir -ProjectRoot $root -Explicit $BinDir -Executable 'client.exe' -Config $Config
}

if (-not $OnlyClient -and -not $OnlyDataset -and $includeCoverage -and -not $StepTestExe) {
        throw 'StepTest executable is required; pass -StepTestExe <path> (the package stores it under testenv/program)'
}
if (-not $OnlyClient -and -not $OnlyProgram -and -not $DatasetDir) {
        throw 'Dataset directory is required; pass -DatasetDir <path> (the package stores it under testenv/dataset)'
}
if (-not $OnlyClient -and -not $OnlyDataset -and $includeCoverage -and -not $OpenCppCoverageDir) {
        throw 'OpenCppCoverage directory is required for coverage; pass -OpenCppCoverageDir <path>'
}

$testenv = Join-Path $OutDir 'testenv'
$coverageRelativeDir = 'testenv/program'
if ($Plugin -eq 'all' -or $previousConfig.plugins.coverage.program_dir -eq 'testenv/program/coverage' -or
    ($OnlyProgram -and ($previousConfig.plugins.stpcompare -or (Test-Path -LiteralPath (Join-Path $testenv 'program/baseline')) -or
        (Test-Path -LiteralPath (Join-Path $testenv 'program/candidate'))))) {
    $coverageRelativeDir = 'testenv/program/coverage'
}
if ($preserveClientConfig -and $previousConfig.plugins.coverage.program_dir) {
    $coverageRelativeDir = [string]$previousConfig.plugins.coverage.program_dir
}
function Assert-SeparateInput([string]$Source, [string]$Destination) {
    if (-not $Source) { return }
    $inputPath = [IO.Path]::GetFullPath($Source).TrimEnd('\', '/')
    $outputPath = [IO.Path]::GetFullPath($Destination).TrimEnd('\', '/')
    if ($inputPath -eq $outputPath -or $inputPath.StartsWith($outputPath + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Input is inside the directory being replaced; preserve it outside the package first: $inputPath"
    }
}
# Validate all component inputs before replacing any deployed files.
if (-not $OnlyClient -and -not $OnlyDataset) {
    Assert-SeparateInput $StepTestExe (Join-Path $testenv 'program')
    Assert-SeparateInput $BaselineConverterExe (Join-Path $testenv 'program')
    Assert-SeparateInput $CandidateConverterExe (Join-Path $testenv 'program')
    Assert-SeparateInput $EngineSrcDir (Join-Path $testenv 'program')
    Assert-SeparateInput $OpenCppCoverageDir (Join-Path $testenv 'opencppcoverage')
}
if (-not $OnlyClient -and -not $OnlyProgram) {
    Assert-SeparateInput $DatasetDir (Join-Path $testenv 'dataset')
}
$programFileName = if ($StepTestExe) { Split-Path -Leaf $StepTestExe } else { 'StepTest.exe' }
$baselineFileName = if ($BaselineConverterExe) { Split-Path -Leaf $BaselineConverterExe } else { 'BaselineConverter.exe' }
$candidateFileName = if ($CandidateConverterExe) { Split-Path -Leaf $CandidateConverterExe } else { 'CandidateConverter.exe' }
if ($preserveClientConfig -and -not $programInputProvided) {
    $previousPlugin = $previousConfig.plugins.$Plugin
    if ($Plugin -eq 'coverage' -and $previousPlugin.target_exe) {
        $programFileName = [System.IO.Path]::GetFileName([string]$previousPlugin.target_exe)
    } elseif ($Plugin -eq 'stpcompare' -and $previousPlugin.programs) {
        $baselineFileName = [System.IO.Path]::GetFileName([string]$previousPlugin.programs.baseline.executable)
        $candidateFileName = [System.IO.Path]::GetFileName([string]$previousPlugin.programs.candidate.executable)
    }
}

if (-not $OnlyClient -and -not $OnlyDataset) {
    $progDir = Join-Path $testenv 'program'
    $coverageProgDir = Join-Path $OutDir $coverageRelativeDir
    $baselineProgDir = Join-Path $progDir 'baseline'
    $candidateProgDir = Join-Path $progDir 'candidate'
    # Validate every selected executable before replacing any component.
    $selectedExecutables = @()
    if ($includeCoverage) { $selectedExecutables += $StepTestExe }
    if ($includeStpCompare) { $selectedExecutables += @($BaselineConverterExe, $CandidateConverterExe) }
    foreach ($executable in $selectedExecutables) {
        if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Program executable not found: $executable" }
    }
    if ($includeCoverage -and -not (Test-Path -LiteralPath (Join-Path $OpenCppCoverageDir 'OpenCppCoverage.exe') -PathType Leaf)) {
        throw "OpenCppCoverage executable not found: $OpenCppCoverageDir"
    }
    if ($includeCoverage -and $EngineSrcDir -and -not (Test-Path -LiteralPath $EngineSrcDir -PathType Container)) {
        throw "engine source dir not found: $EngineSrcDir"
    }
    function Reset-ProgramDirectory([string]$Destination) {
        Assert-OwnedPath $Destination $OutDir | Out-Null
        if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
        New-Item -ItemType Directory -Force $Destination | Out-Null
    }
    # Full builds own the entire tree; component updates own only selected paths.
    if (-not $OnlyProgram) { Reset-ProgramDirectory $progDir }

    function Copy-ProgramBundle([string]$Executable, [string]$Destination, [string]$Version) {
        Reset-ProgramDirectory $Destination
        Copy-Item -LiteralPath $Executable -Destination $Destination
        $sourceDir = Split-Path $Executable -Parent
        $pdb = [System.IO.Path]::ChangeExtension($Executable, '.pdb')
        if (Test-Path -LiteralPath $pdb -PathType Leaf) { Copy-Item -LiteralPath $pdb -Destination $Destination }
        Get-ChildItem $sourceDir -File -Filter '*.dll' | Copy-Item -Destination $Destination
        Copy-VCRuntime -Destination $Destination -RuntimeDir $VCRuntimeDir | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $Destination 'version.txt'), $Version,
            (New-Object System.Text.UTF8Encoding($false)))
    }

    if ($includeCoverage) {
        Copy-ProgramBundle $StepTestExe $coverageProgDir $ProgramVersion
        $stepDir = Split-Path $StepTestExe -Parent
        foreach ($dll in $effectiveEngineDlls) {
            $pdbPath = [System.IO.Path]::ChangeExtension((Join-Path $stepDir $dll), '.pdb')
            if (Test-Path $pdbPath) { Copy-Item -LiteralPath $pdbPath -Destination $coverageProgDir }
        }
        if ($EngineSrcDir) {
            Copy-Item -LiteralPath $EngineSrcDir -Destination (Join-Path $coverageProgDir 'engine_src') -Recurse
        }
    }
    if ($includeStpCompare) {
        Copy-ProgramBundle $BaselineConverterExe $baselineProgDir $BaselineProgramVersion
        Copy-ProgramBundle $CandidateConverterExe $candidateProgDir $CandidateProgramVersion
    }
    if ($OnlyProgram -and (Test-Path -LiteralPath $clientConfigPath)) {
        $existingConfig = Get-Content -LiteralPath $clientConfigPath -Raw | ConvertFrom-Json
        foreach ($pluginName in $selectedPluginNames) {
            $existingPlugin = $existingConfig.plugins.$pluginName
            if (-not $existingPlugin) { continue }
            if ($pluginName -eq 'coverage') {
                $existingPlugin | Add-Member -NotePropertyName target_exe -NotePropertyValue ($coverageRelativeDir + '/' + $programFileName) -Force
                $existingPlugin | Add-Member -NotePropertyName program_dir -NotePropertyValue $coverageRelativeDir -Force
                $existingPlugin | Add-Member -NotePropertyName engine_dlls -NotePropertyValue @($effectiveEngineDlls | ForEach-Object { "$coverageRelativeDir/$_" }) -Force
            } else {
                $existingPlugin.programs.baseline.executable = 'testenv/program/baseline/' + $baselineFileName
                $existingPlugin.programs.candidate.executable = 'testenv/program/candidate/' + $candidateFileName
            }
        }
        Write-AtomicText $clientConfigPath ($existingConfig | ConvertTo-Json -Depth 20)
    }

    if ($includeCoverage) {
    # testenv\opencppcoverage (static tooling).
    $occDir = Join-Path $testenv 'opencppcoverage'
    if (Test-Path $occDir) {
        Assert-OwnedPath $occDir $OutDir | Out-Null
        Remove-Item -LiteralPath $occDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force $occDir | Out-Null
    if (Get-Command robocopy -ErrorAction SilentlyContinue) {
        & robocopy $OpenCppCoverageDir $occDir /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) {
            throw "robocopy opencppcoverage failed with exit code $LASTEXITCODE"
        }
    } else {
        Get-ChildItem $OpenCppCoverageDir | Copy-Item -Destination $occDir -Recurse
    }
    Get-ChildItem $occDir -Filter 'unins*' | Remove-Item -Force
    Write-Host "built testenv\program (version $ProgramVersion) + opencppcoverage"
    }
    if ($includeStpCompare) {
        Write-Host "built baseline ($BaselineProgramVersion) and candidate ($CandidateProgramVersion) programs"
    }
}

if (-not $OnlyClient -and -not $OnlyProgram) {
    $dataDir = Join-Path $testenv 'dataset'
    if (Test-Path $dataDir) {
        Assert-OwnedPath $dataDir $OutDir | Out-Null
        Remove-Item -LiteralPath $dataDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force $dataDir | Out-Null
    if (Get-Command robocopy -ErrorAction SilentlyContinue) {
        & robocopy $DatasetDir $dataDir *.stp /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) {
            throw "robocopy failed with exit code $LASTEXITCODE"
        }
    } else {
        Get-ChildItem $DatasetDir -File -Filter *.stp -Recurse | ForEach-Object {
            $rel = $_.FullName.Substring((Resolve-Path $DatasetDir).Path.Length + 1)
            $dest = Join-Path $dataDir $rel
            New-Item -ItemType Directory -Force (Split-Path $dest -Parent) | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $dest
        }
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $dataDir 'version.txt'),
        $DatasetVersion,
        (New-Object System.Text.UTF8Encoding($false)))
    & (Join-Path $PSScriptRoot 'build_dataset_manifest.ps1') `
        -DatasetDir $dataDir `
        -OutFile (Join-Path $dataDir 'manifest.txt')
    $count = (Get-ChildItem $dataDir -File -Filter *.stp -Recurse | Measure-Object).Count
    Write-Host "built testenv\dataset ($count files, version $DatasetVersion)"
}

if (-not $OnlyProgram -and -not $OnlyDataset) {
    New-Item -ItemType Directory -Force (Join-Path $OutDir 'config') | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $OutDir 'tools') | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'client_update.ps1') -Destination (Join-Path $OutDir 'tools')
    Copy-Item -LiteralPath (Join-Path $BinDir 'client.exe') -Destination $OutDir
    Get-ChildItem $BinDir -Filter '*.dll' | Copy-Item -Destination $OutDir
    Copy-VCRuntime -Destination $OutDir -RuntimeDir $VCRuntimeDir | Out-Null

    $coverageCfg = @{
        dll = 'coverage.dll'
        enabled = $true
        dry_run = $false
        store_type = $StoreType
        store_cov = [bool]$StoreCov
        store_dir = 'store'
        program_dir = $coverageRelativeDir
        stp_root = 'testenv/dataset'
        export_dir = 'reports'
        opencppcoverage = 'testenv/opencppcoverage/OpenCppCoverage.exe'
        target_exe = $coverageRelativeDir + '/' + $programFileName
        engine_dlls = @($EngineDlls | ForEach-Object { "$coverageRelativeDir/$_" })
    }
    if ($DbConn) {
        $coverageCfg['db_conn'] = $DbConn
    }
    $stpCompareCfg = @{
            dll = 'stpcompare.dll'; enabled = $true; dry_run = $false
            store_type = $StoreType; db_conn = $DbConn; store_dir = 'store'
            programs = [ordered]@{
                baseline = [ordered]@{
                    executable = ('testenv/program/baseline/' + $baselineFileName)
                    arguments = @('--input', '{stp}', '--output-dir', '{outdir}')
                }
                candidate = [ordered]@{
                    executable = ('testenv/program/candidate/' + $candidateFileName)
                    arguments = @('--input', '{stp}', '--output-dir', '{outdir}')
                }
            }
            stp_root = 'testenv/dataset'; work_dir = 'stp_compare_work'
            table = 'stp_compare_old'
    }
    $pluginDefaults = @{ coverage = $coverageCfg; stpcompare = $stpCompareCfg }
    $packagedPlugins = @{}
    foreach ($pluginName in $selectedPluginNames) { $packagedPlugins[$pluginName] = $pluginDefaults[$pluginName] }
    $clientCfg = @{
        scheduler = @{
            endpoint = $Endpoint
            poll_timeout_ms = 50
        }
        client = @{
            worker_id = $WorkerId
            heartbeat_interval_ms = 1000
            task_threads = $TaskThreads
            request_backoff_initial_ms = 200
            request_backoff_max_ms = 30000
            register_timeout_ms = 5000
            register_max_attempts = 5
            deployment_endpoint = $DeploymentEndpoint
            deployment_poll_seconds = 30
            deployment_stage_timeout_seconds = 0
            execution_windows = @()
            update_windows = @()
        }
        plugins = $packagedPlugins
    }
    if ($preserveClientConfig) {
        $clientCfg = $previousConfig
        if ($PSBoundParameters.ContainsKey('Endpoint')) { $clientCfg.scheduler.endpoint = $Endpoint }
        elseif ($PSBoundParameters.ContainsKey('ServerIp') -or $PSBoundParameters.ContainsKey('ServerPort')) {
            $oldEndpoint = [string]$clientCfg.scheduler.endpoint
            if ($oldEndpoint -notmatch '^tcp://(.+):(\d+)$') { throw 'Pass -Endpoint to replace this scheduler endpoint.' }
            $endpointHost = if ($PSBoundParameters.ContainsKey('ServerIp')) { $ServerIp } else { $Matches[1] }
            $endpointPort = if ($PSBoundParameters.ContainsKey('ServerPort')) { $ServerPort } else { $Matches[2] }
            $clientCfg.scheduler.endpoint = "tcp://${endpointHost}:$endpointPort"
        }
        if ($PSBoundParameters.ContainsKey('WorkerId')) { $clientCfg.client.worker_id = $WorkerId }
        if ($PSBoundParameters.ContainsKey('DeploymentEndpoint')) {
            $clientCfg.client | Add-Member -NotePropertyName deployment_endpoint -NotePropertyValue $DeploymentEndpoint -Force
        }
        if ($PSBoundParameters.ContainsKey('TaskThreads')) { $clientCfg.client.task_threads = $TaskThreads }
        if ($PSBoundParameters.ContainsKey('Plugin')) {
            $oldEnabled = @($clientCfg.plugins.PSObject.Properties | Where-Object { $_.Value.enabled })
            foreach ($pluginName in $selectedPluginNames) {
                if (-not $clientCfg.plugins.$pluginName) {
                    $pluginDefault = $pluginDefaults[$pluginName]
                    if ($oldEnabled.Count -ge 1) {
                        foreach ($name in @('store_type', 'db_conn', 'store_dir', 'stp_root')) {
                            if ($oldEnabled[0].Value.PSObject.Properties[$name]) { $pluginDefault[$name] = $oldEnabled[0].Value.$name }
                        }
                    }
                    $clientCfg.plugins | Add-Member -NotePropertyName $pluginName -NotePropertyValue ([pscustomobject]$pluginDefault)
                }
            }
            foreach ($property in $clientCfg.plugins.PSObject.Properties) {
                $property.Value | Add-Member -NotePropertyName enabled -NotePropertyValue ($property.Name -in $selectedPluginNames) -Force
            }
        }
        $targetPlugins = @($clientCfg.plugins.PSObject.Properties | Where-Object { $_.Value.enabled })
        foreach ($property in $targetPlugins) {
            $pluginConfig = $property.Value
            if ($PSBoundParameters.ContainsKey('StoreType')) {
                $pluginConfig | Add-Member -NotePropertyName store_type -NotePropertyValue $StoreType -Force
            }
            if ($PSBoundParameters.ContainsKey('DbConn')) {
                $pluginConfig | Add-Member -NotePropertyName db_conn -NotePropertyValue $DbConn -Force
            } else {
                # libpq uses the last value for repeated connection keywords,
                # so an explicit field override preserves all other settings.
                $connection = [string]$pluginConfig.db_conn
                foreach ($field in @(@('ServerIp', 'host'), @('DbPort', 'port'), @('DbName', 'dbname'), @('DbUser', 'user'), @('DbPassword', 'password'))) {
                    if ($PSBoundParameters.ContainsKey($field[0])) {
                        $value = ([string]$PSBoundParameters[$field[0]]).Replace('\', '\\').Replace("'", "\'")
                        $connection += " $($field[1])='$value'"
                    }
                }
                if ($connection -ne [string]$pluginConfig.db_conn) {
                    $pluginConfig | Add-Member -NotePropertyName db_conn -NotePropertyValue $connection.Trim() -Force
                }
            }
            if ($property.Name -eq 'coverage') {
                if ($PSBoundParameters.ContainsKey('StoreCov')) { $pluginConfig.store_cov = [bool]$StoreCov }
                if ($PSBoundParameters.ContainsKey('EngineDlls')) { $pluginConfig.engine_dlls = @($EngineDlls | ForEach-Object { "$coverageRelativeDir/$_" }) }
                if ($PSBoundParameters.ContainsKey('StepTestExe')) { $pluginConfig.target_exe = $coverageRelativeDir + '/' + $programFileName }
            } elseif ($property.Name -eq 'stpcompare') {
                if ($PSBoundParameters.ContainsKey('BaselineConverterExe')) {
                    $pluginConfig.programs.baseline.executable = 'testenv/program/baseline/' + $baselineFileName
                }
                if ($PSBoundParameters.ContainsKey('CandidateConverterExe')) {
                    $pluginConfig.programs.candidate.executable = 'testenv/program/candidate/' + $candidateFileName
                }
            }
        }
    }
    Write-AtomicText $clientConfigPath ($clientCfg | ConvertTo-Json -Depth 20)

    @"
@echo off
cd /d "%~dp0"
client.exe config\client.json
pause
"@ | Set-Content -Path (Join-Path $OutDir 'start_client.bat') -Encoding ASCII

    Write-Host "built client package: $OutDir"
}

if ($OnlyProgram -or $OnlyDataset) {
    Write-Host "component update complete: copy '$OutDir' to the target package directory"
} else {
    Write-Host "done. copy '$OutDir' to the target machine and run start_client.bat"
    Write-Host 'Check config/client.json endpoint/db_conn for the deployment server before starting the client.'
}
