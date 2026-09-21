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
#   -OnlyProgram  replace testenv\program and bump its version
#   -OnlyDataset  replace testenv\dataset and bump its version
param(
    [string]$OutDir = '',
    [string]$BinDir = $null,
    [string]$Endpoint = '',
    [string]$DeploymentEndpoint = '',
    [string]$WorkerId = 'worker-1',
    [ValidateSet('coverage', 'stpcompare')][string]$Plugin = 'coverage',
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
if ($Config -ne 'Release') {
    throw 'Deployment packages require -Config Release; Debug runtimes cannot be redistributed'
}

$OutDir = if ($OutDir) { Resolve-ProjectPath $OutDir } else { Join-Path $root 'dist\client_package' }
$clientConfigPath = Join-Path $OutDir 'config\client.json'
$previousConfig = $null
if (($OnlyClient -or $OnlyProgram -or $OnlyDataset) -and (Test-Path -LiteralPath $clientConfigPath)) {
    $previousConfig = Get-Content -LiteralPath $clientConfigPath -Raw | ConvertFrom-Json
    if (-not $PSBoundParameters.ContainsKey('Plugin')) {
        $enabled = @($previousConfig.plugins.PSObject.Properties | Where-Object { $_.Value.enabled })
        if ($enabled.Count -eq 1) { $Plugin = $enabled[0].Name }
    }
}
$preserveClientConfig = $OnlyClient -and $null -ne $previousConfig

if ($Plugin -eq 'stpcompare') {
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

if (-not $OnlyClient -and -not $OnlyDataset -and $Plugin -eq 'coverage' -and -not $StepTestExe) {
        throw 'StepTest executable is required; pass -StepTestExe <path> (the package stores it under testenv/program)'
}
if (-not $OnlyClient -and -not $OnlyProgram -and -not $DatasetDir) {
        throw 'Dataset directory is required; pass -DatasetDir <path> (the package stores it under testenv/dataset)'
}
if (-not $OnlyClient -and -not $OnlyDataset -and $Plugin -eq 'coverage' -and -not $OpenCppCoverageDir) {
        throw 'OpenCppCoverage directory is required for coverage; pass -OpenCppCoverageDir <path>'
}

$testenv = Join-Path $OutDir 'testenv'
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
    if (Test-Path $progDir) {
        Assert-OwnedPath $progDir $OutDir | Out-Null
        Remove-Item -LiteralPath $progDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force $progDir | Out-Null

    function Copy-ProgramBundle([string]$Executable, [string]$Destination, [string]$Version) {
        if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
            throw "Program executable not found: $Executable"
        }
        New-Item -ItemType Directory -Force $Destination | Out-Null
        Copy-Item -LiteralPath $Executable -Destination $Destination
        $sourceDir = Split-Path $Executable -Parent
        $pdb = [System.IO.Path]::ChangeExtension($Executable, '.pdb')
        if (Test-Path -LiteralPath $pdb -PathType Leaf) { Copy-Item -LiteralPath $pdb -Destination $Destination }
        Get-ChildItem $sourceDir -File -Filter '*.dll' | Copy-Item -Destination $Destination
        Copy-VCRuntime -Destination $Destination -RuntimeDir $VCRuntimeDir | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $Destination 'version.txt'), $Version,
            (New-Object System.Text.UTF8Encoding($false)))
    }

    if ($Plugin -eq 'coverage') {
        Copy-ProgramBundle $StepTestExe $progDir $ProgramVersion
        $stepDir = Split-Path $StepTestExe -Parent
        foreach ($dll in $EngineDlls) {
            $pdbPath = [System.IO.Path]::ChangeExtension((Join-Path $stepDir $dll), '.pdb')
            if (Test-Path $pdbPath) { Copy-Item -LiteralPath $pdbPath -Destination $progDir }
        }
        if ($EngineSrcDir) {
            if (-not (Test-Path $EngineSrcDir)) { throw "engine source dir not found: $EngineSrcDir" }
            Copy-Item -Path $EngineSrcDir -Destination (Join-Path $progDir 'engine_src') -Recurse
        }
    } else {
        Copy-ProgramBundle $BaselineConverterExe (Join-Path $progDir 'baseline') $BaselineProgramVersion
        Copy-ProgramBundle $CandidateConverterExe (Join-Path $progDir 'candidate') $CandidateProgramVersion
    }
    if ($OnlyProgram -and (Test-Path -LiteralPath $clientConfigPath)) {
        $existingConfig = Get-Content -LiteralPath $clientConfigPath -Raw | ConvertFrom-Json
        $existingPlugin = $existingConfig.plugins.$Plugin
        if ($existingPlugin) {
            if ($Plugin -eq 'coverage') {
                $existingPlugin.target_exe = 'testenv/program/' + $programFileName
            } else {
                $existingPlugin.programs.baseline.executable = 'testenv/program/baseline/' + $baselineFileName
                $existingPlugin.programs.candidate.executable = 'testenv/program/candidate/' + $candidateFileName
            }
            Write-AtomicText $clientConfigPath ($existingConfig | ConvertTo-Json -Depth 10)
        }
    }

    if ($Plugin -eq 'coverage') {
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
    } else {
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
        program_dir = 'testenv/program'
        stp_root = 'testenv/dataset'
        export_dir = 'reports'
        opencppcoverage = 'testenv/opencppcoverage/OpenCppCoverage.exe'
        target_exe = 'testenv/program/' + $programFileName
        engine_dlls = $EngineDlls | ForEach-Object { "testenv/program/$_" }
    }
    if ($DbConn) {
        $coverageCfg['db_conn'] = $DbConn
    }
    $selectedPlugin = $coverageCfg
    if ($Plugin -eq 'stpcompare') {
        $selectedPlugin = @{
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
    }
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
        plugins = @{ $Plugin = $selectedPlugin }
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
            if (-not $clientCfg.plugins.$Plugin) {
                $oldEnabled = @($clientCfg.plugins.PSObject.Properties | Where-Object { $_.Value.enabled })
                if ($oldEnabled.Count -eq 1) {
                    foreach ($name in @('store_type', 'db_conn', 'store_dir', 'program_dir', 'stp_root')) {
                        if ($oldEnabled[0].Value.PSObject.Properties[$name]) { $selectedPlugin[$name] = $oldEnabled[0].Value.$name }
                    }
                }
                $clientCfg.plugins | Add-Member -NotePropertyName $Plugin -NotePropertyValue ([pscustomobject]$selectedPlugin)
            }
            foreach ($property in $clientCfg.plugins.PSObject.Properties) {
                $property.Value | Add-Member -NotePropertyName enabled -NotePropertyValue ($property.Name -eq $Plugin) -Force
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
                if ($PSBoundParameters.ContainsKey('EngineDlls')) { $pluginConfig.engine_dlls = @($EngineDlls | ForEach-Object { "testenv/program/$_" }) }
                if ($PSBoundParameters.ContainsKey('StepTestExe')) { $pluginConfig.target_exe = 'testenv/program/' + $programFileName }
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

Write-Host "done. copy '$OutDir' to the target machine and run start_client.bat"
if (-not $OnlyProgram -and -not $OnlyDataset) {
    Write-Host 'Check config/client.json endpoint/db_conn for the deployment server before starting the client.'
}
