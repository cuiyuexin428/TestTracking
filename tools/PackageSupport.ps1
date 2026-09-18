# Runtime assets shared by the Windows x64 release package builders.
function Resolve-PackageBinaryDir {
    param([string]$ProjectRoot, [string]$Explicit, [string]$Executable, [string]$Config = 'Release')
    # Match setup_offline_server.ps1. Never silently use an older development
    # build copied with the source tree; other outputs must be explicit.
    $candidate = if ($Explicit) {
        if ([IO.Path]::IsPathRooted($Explicit)) { [IO.Path]::GetFullPath($Explicit) }
        else { [IO.Path]::GetFullPath((Join-Path $ProjectRoot $Explicit)) }
    } else { Join-Path $ProjectRoot "build\offline_verify\bin\$Config" }
    if (Test-Path -LiteralPath (Join-Path $candidate $Executable) -PathType Leaf) { return $candidate }
    if ($Explicit) {
        $nested = Join-Path $candidate "bin\$Config"
        if (Test-Path -LiteralPath (Join-Path $nested $Executable) -PathType Leaf) { return $nested }
    }
    throw "$Executable not found under $candidate; pass -BinDir with the output of the current build."
}

function Copy-VCRuntime {
    param([Parameter(Mandatory = $true)][string]$Destination, [string]$RuntimeDir = '')
    $required = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')
    $candidate = $null
    if ($RuntimeDir) {
        $candidate = [System.IO.Path]::GetFullPath($RuntimeDir)
        if (-not (Test-Path -LiteralPath $candidate -PathType Container)) {
            throw "VC++ runtime directory not found: $candidate"
        }
    } elseif (@($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Destination $_) -PathType Leaf) }).Count -eq 0) {
        return $Destination
    } else {
        $installations = @()
        if ($env:VSINSTALLDIR) { $installations += $env:VSINSTALLDIR }
        $vswhere = if (${env:ProgramFiles(x86)}) {
            Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        } else { '' }
        if ($vswhere -and (Test-Path -LiteralPath $vswhere)) {
            $installations += @(& $vswhere -products '*' -property installationPath)
        }
        foreach ($installation in ($installations | Select-Object -Unique)) {
            $redist = Join-Path $installation 'VC\Redist\MSVC'
            if (-not (Test-Path -LiteralPath $redist -PathType Container)) { continue }
            $versions = Get-ChildItem -LiteralPath $redist -Directory | Sort-Object Name -Descending
            foreach ($version in $versions) {
                $x64 = Join-Path $version.FullName 'x64'
                if (-not (Test-Path -LiteralPath $x64 -PathType Container)) { continue }
                foreach ($crt in (Get-ChildItem -LiteralPath $x64 -Directory -Filter 'Microsoft.VC*.CRT')) {
                    if (@($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $crt.FullName $_) -PathType Leaf) }).Count -eq 0) {
                        $candidate = $crt.FullName
                        break
                    }
                }
                if ($candidate) { break }
            }
            if ($candidate) { break }
        }
    }
    if (-not $candidate) {
        throw 'VC++ x64 release runtime not found; pass -VCRuntimeDir <Microsoft.VC*.CRT directory> from the VS redist x64 directory'
    }
    $missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $candidate $_) -PathType Leaf) })
    if ($missing.Count) { throw "VC++ runtime directory is missing: $($missing -join ', ')" }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    if ([System.IO.Path]::GetFullPath($Destination).TrimEnd('\') -ne $candidate.TrimEnd('\')) {
        Get-ChildItem -LiteralPath $candidate -File -Filter '*.dll' | Copy-Item -Destination $Destination -Force
    }
    return $candidate
}
