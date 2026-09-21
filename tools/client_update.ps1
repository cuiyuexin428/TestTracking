<#
.SYNOPSIS
Stages an immutable client deployment. The client Agent owns activation and rollback.
.DESCRIPTION
Requires only Windows PowerShell 5.1 and .NET. Relative configuration paths retain
their meaning against ClientRoot, the working directory used by client workers.
No active files are replaced and no processes are started or stopped here.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ClientRoot,
    [Parameter(Mandatory = $true)][string]$ConfigPath,
    [Parameter(Mandatory = $true)][string]$ResultPath,
    [string]$ReportPath = '',
    [string]$EffectiveConfigPath = '',
    [string]$CurrentRevision = '',
    [string]$PreparedRevision = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$script:Utf8 = New-Object System.Text.UTF8Encoding($false)
$script:Revision = $null
$script:ReservationId = ''
$script:Components = @()
$script:Endpoint = $null
$script:WorkerId = $null
$script:Lock = $null

function ConvertTo-Map($Value) {
    if ($null -eq $Value) { return $null }
    if ($Value -is [System.Collections.IDictionary]) {
        $map = @{}
        foreach ($key in $Value.Keys) { $map[$key] = ConvertTo-Map $Value[$key] }
        return $map
    }
    if ($Value -is [System.Management.Automation.PSCustomObject]) {
        $map = @{}
        foreach ($property in $Value.PSObject.Properties) {
            $map[$property.Name] = ConvertTo-Map $property.Value
        }
        return $map
    }
    if ($Value -is [System.Array]) {
        $items = New-Object 'System.Collections.Generic.List[object]'
        foreach ($item in $Value) { $items.Add((ConvertTo-Map $item)) }
        return ,$items.ToArray()
    }
    return $Value
}

function Read-Json([string]$Path) {
    return ConvertTo-Map ([IO.File]::ReadAllText($Path, $script:Utf8) | ConvertFrom-Json)
}

function Assert-NoLink([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if (([IO.File]::GetAttributes($cursor) -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Deployment paths cannot contain filesystem links: $cursor"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}

function New-SafeDirectory([string]$Path) {
    Assert-NoLink $Path
    [void][IO.Directory]::CreateDirectory($Path)
}

function Write-AtomicJson([string]$Path, $Value) {
    $parent = [IO.Path]::GetDirectoryName($Path)
    New-SafeDirectory $parent
    Assert-NoLink $Path
    $temporary = Join-Path $parent ('.write-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $backup = $temporary + '.previous'
    try {
        [IO.File]::WriteAllText($temporary, ($Value | ConvertTo-Json -Depth 100), $script:Utf8)
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary, $Path, $backup) }
        else { [IO.File]::Move($temporary, $Path) }
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
        if ([IO.File]::Exists($backup)) { [IO.File]::Delete($backup) }
    }
}

function Get-Sha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}

function Get-TextSha256([string]$Text) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($script:Utf8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}

function Test-DayAllowed($Window, [int]$Day) {
    return (-not $Window.ContainsKey('days') -or @($Window.days).Count -eq 0 -or $Day -in $Window.days)
}

function Test-UpdateWindow($Windows) {
    if ($null -eq $Windows -or @($Windows).Count -eq 0) { return $true }
    $now = Get-Date
    $day = [int]$now.DayOfWeek
    if ($day -eq 0) { $day = 7 }
    $previous = if ($day -eq 1) { 7 } else { $day - 1 }
    $minute = $now.Hour * 60 + $now.Minute
    foreach ($window in $Windows) {
        if ($window -isnot [System.Collections.IDictionary]) { continue }
        if ($window.start -notmatch '^([01][0-9]|2[0-3]):[0-5][0-9]$' -or
            $window.end -notmatch '^([01][0-9]|2[0-3]):[0-5][0-9]$') { continue }
        $start = [int]$window.start.Substring(0, 2) * 60 + [int]$window.start.Substring(3, 2)
        $end = [int]$window.end.Substring(0, 2) * 60 + [int]$window.end.Substring(3, 2)
        if ($start -eq $end -and (Test-DayAllowed $window $day)) { return $true }
        if ($start -lt $end -and (Test-DayAllowed $window $day) -and $minute -ge $start -and $minute -lt $end) { return $true }
        if ($start -gt $end -and (((Test-DayAllowed $window $day) -and $minute -ge $start) -or
                                  ((Test-DayAllowed $window $previous) -and $minute -lt $end))) { return $true }
    }
    return $false
}

function New-Request([string]$Url, [string]$Method = 'GET') {
    $request = [Net.HttpWebRequest]::Create($Url)
    $request.Method = $Method
    $request.Timeout = 30000
    $request.ReadWriteTimeout = 30000
    return $request
}

function Invoke-Json([string]$Url, [string]$Method = 'GET', [string]$Body = '') {
    $request = New-Request $Url $Method
    if ($Body) {
        $bytes = $script:Utf8.GetBytes($Body)
        $request.ContentType = 'application/json; charset=utf-8'
        $request.ContentLength = $bytes.Length
        $stream = $request.GetRequestStream()
        try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
    }
    $response = $request.GetResponse()
    try {
        $reader = New-Object IO.StreamReader($response.GetResponseStream(), $script:Utf8)
        try { return ConvertTo-Map ($reader.ReadToEnd() | ConvertFrom-Json) }
        finally { $reader.Dispose() }
    } finally { $response.Dispose() }
}

function Send-PendingReport {
    if (-not $ReportPath -or -not [IO.File]::Exists($ReportPath)) { return }
    try {
        Assert-NoLink $ReportPath
        $body = [IO.File]::ReadAllText($ReportPath, $script:Utf8)
        $digest = Get-TextSha256 $body
        $sentPath = $ReportPath + '.sent'
        if ([IO.File]::Exists($sentPath) -and (Read-Json $sentPath).sha256 -eq $digest) { return }
        try {
            $null = Invoke-Json ($script:Endpoint + '/api/deployments/' + $script:WorkerId + '/reported') 'PUT' $body
        } catch [Net.WebException] {
            if ($null -eq $_.Exception.Response -or [int]$_.Exception.Response.StatusCode -ne 409) { throw }
            $_.Exception.Response.Dispose()
        }
        # Keep the source file: the Agent may have atomically replaced it while HTTP
        # was in flight. A digest acknowledgement cannot lose the newer report.
        Write-AtomicJson $sentPath @{ sha256 = $digest }
    } catch {
        Write-Warning ('Deployment report will be retried: ' + $_.Exception.Message)
    }
}

function Merge-Config($Base, $Patch) {
    if ($Patch -isnot [System.Collections.IDictionary]) { return ,$Patch }
    if ($Base -isnot [System.Collections.IDictionary]) { $Base = @{} }
    foreach ($key in $Patch.Keys) { $Base[$key] = Merge-Config $Base[$key] $Patch[$key] }
    return $Base
}

function Assert-EnabledPlugins($Config) {
    if ($Config -isnot [System.Collections.IDictionary] -or
        $Config.plugins -isnot [System.Collections.IDictionary]) {
        throw 'Client deployment requires at least one enabled plugin'
    }
    $enabledCount = 0
    foreach ($name in $Config.plugins.Keys) {
        $plugin = $Config.plugins[$name]
        if ($plugin -isnot [System.Collections.IDictionary]) { throw "Plugin configuration must be an object: $name" }
        if ($plugin.ContainsKey('enabled') -and $plugin.enabled -isnot [bool]) {
            throw "Plugin enabled must be a boolean: $name"
        }
        if ($plugin.enabled -eq $true) { $enabledCount++ }
    }
    if ($enabledCount -lt 1 -and -not $Config.client.deployment_endpoint) {
        throw "Client deployment requires at least one enabled plugin; found $enabledCount."
    }
}

function ConvertTo-CanonicalJson($Value) {
    function Sort-Json($Node) {
        if ($Node -is [System.Collections.IDictionary]) {
            $result = [ordered]@{}
            foreach ($key in @($Node.Keys | Sort-Object)) { $result[$key] = Sort-Json $Node[$key] }
            return $result
        }
        if ($Node -is [System.Array]) {
            $items = New-Object 'System.Collections.Generic.List[object]'
            foreach ($item in $Node) { $items.Add((Sort-Json $item)) }
            return ,$items.ToArray()
        }
        return $Node
    }
    return (Sort-Json $Value | ConvertTo-Json -Depth 100 -Compress)
}

function Resolve-ConfigPaths($Value, [string]$Key, $Slots) {
    if ($Value -is [System.Collections.IDictionary]) {
        foreach ($child in @($Value.Keys)) { $Value[$child] = Resolve-ConfigPaths $Value[$child] $child $Slots }
        return $Value
    }
    if ($Value -is [System.Array]) {
        $items = New-Object 'System.Collections.Generic.List[object]'
        foreach ($item in $Value) { $items.Add((Resolve-ConfigPaths $item $Key $Slots)) }
        return ,$items.ToArray()
    }
    if ($Value -isnot [string] -or -not $Value) { return $Value }
    $resolved = [regex]::Replace($Value, '\{artifact:([A-Za-z0-9][A-Za-z0-9._-]{0,127})\}', {
        param($match)
        $slot = $match.Groups[1].Value
        if (-not $Slots.ContainsKey($slot)) { throw "Unknown artifact slot in config_patch: $slot" }
        return $Slots[$slot]
    })
    if ($resolved -match '\{artifact:') { throw "Invalid artifact placeholder: $resolved" }
    # These field shapes form the generic client configuration path convention.
    # Plugin-specific names are deliberately not consulted by the updater.
    if ($Key -match '(^dll$|^executable$|^executables$|^path$|^paths$|^directory$|^root$|_(path|paths|dir|dirs|root|dll|dlls|exe|file|files)$)' -and
        $resolved -notmatch '^[A-Za-z][A-Za-z0-9+.-]*://' -and $resolved -notmatch '\{[^}]+\}') {
        if (-not [IO.Path]::IsPathRooted($resolved)) { $resolved = Join-Path $ClientRoot $resolved }
        return [IO.Path]::GetFullPath($resolved)
    }
    return $resolved
}

function Get-SafeEntryPath([string]$Root, [string]$Name) {
    $relative = $Name.Replace('\', '/')
    if (-not $relative -or $relative.StartsWith('/') -or $relative.Contains(':')) { throw "Unsafe archive entry: $Name" }
    $parts = $relative.TrimEnd('/').Split('/')
    foreach ($part in $parts) {
        if (-not $part -or $part -eq '.' -or $part -eq '..' -or $part -match '[<>"|?*\x00-\x1f]' -or
            $part.EndsWith('.') -or $part.EndsWith(' ') -or $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
            throw "Unsafe archive entry: $Name"
        }
    }
    $full = [IO.Path]::GetFullPath((Join-Path $Root ($parts -join [IO.Path]::DirectorySeparatorChar)))
    if (-not $full.StartsWith($Root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Archive entry escapes content directory: $Name"
    }
    return $full
}

function Expand-SafeZip([string]$Archive, [string]$Destination) {
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        $entries = New-Object 'System.Collections.Generic.List[object]'
        foreach ($entry in $zip.Entries) {
            $path = Get-SafeEntryPath $Destination $entry.FullName
            if (-not $seen.Add($path)) { throw "Duplicate archive path: $($entry.FullName)" }
            $unixType = ($entry.ExternalAttributes -shr 16) -band 0xF000
            if ($unixType -eq 0xA000 -or ($entry.ExternalAttributes -band 0x400) -ne 0) {
                throw "Archive links are not allowed: $($entry.FullName)"
            }
            $isDirectory = $entry.FullName.EndsWith('/') -or $entry.FullName.EndsWith('\')
            $entries.Add(@{ entry = $entry; path = $path; directory = $isDirectory })
        }
        foreach ($item in $entries) {
            if ($item.directory) { New-SafeDirectory $item.path; continue }
            New-SafeDirectory ([IO.Path]::GetDirectoryName($item.path))
            $inputStream = $item.entry.Open()
            try {
                $outputStream = [IO.File]::Open($item.path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
            } finally { $inputStream.Dispose() }
        }
    } finally { $zip.Dispose() }
}

function Test-CachedContent([string]$Content, $Manifest) {
    if (-not [IO.Directory]::Exists($Content)) { return $false }
    foreach ($entry in $Manifest.files) {
        $path = Get-SafeEntryPath $Content $entry.path
        Assert-NoLink $path
        if (-not [IO.File]::Exists($path) -or (Get-Item -LiteralPath $path).Length -ne $entry.size_bytes -or
            (Get-Sha256 $path) -ne $entry.sha256) { return $false }
    }
    return $true
}

function Stage-Artifact($Artifact) {
    $sha = [string]$Artifact.sha256
    if ($sha -cnotmatch '^[0-9a-f]{64}$') { throw 'Artifact SHA-256 is invalid' }
    $filename = [string]$Artifact.filename
    $objectRoot = Join-Path $script:ObjectsRoot $sha
    New-SafeDirectory $objectRoot
    $archive = Join-Path $objectRoot 'archive'
    $isZip = [IO.Path]::GetExtension($filename) -ieq '.zip'
    # Archive identity is its bytes, while a plain file's materialization also
    # includes its filename. Identical bytes may legitimately be published as
    # first.exe and second.exe, or as a ZIP and as an opaque ordinary file.
    $cacheRoot = $objectRoot
    if (-not $isZip) {
        # Avoid stacking two 64-character identities in the Windows path.
        $materialization = Get-TextSha256 ($sha + "`n" + $filename)
        $cacheRoot = Join-Path (Join-Path $script:ObjectsRoot 'raw') $materialization
        New-SafeDirectory $cacheRoot
    }
    $content = Join-Path $cacheRoot 'content'
    $manifestPath = Join-Path $cacheRoot 'content.json'
    $null = Get-SafeEntryPath $content $filename
    if ($filename.Contains('/') -or $filename.Contains('\')) { throw 'Artifact filename must be a plain filename' }
    if ([IO.File]::Exists($archive)) {
        Assert-NoLink $archive
        if ((Get-Sha256 $archive) -ne $sha) { throw "Cached archive SHA-256 mismatch: $sha" }
    } else {
        $temporary = Join-Path $objectRoot ('download-' + [Guid]::NewGuid().ToString('N') + '.tmp')
        try {
            $url = New-Object Uri ([Uri]($script:Endpoint + '/')), ([string]$Artifact.download_url)
            if ($url.Scheme -notin @('http', 'https')) { throw 'Artifact download requires HTTP or HTTPS' }
            $response = (New-Request $url.AbsoluteUri).GetResponse()
            try {
                $inputStream = $response.GetResponseStream()
                try {
                    $outputStream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                    try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
                } finally { $inputStream.Dispose() }
            } finally { $response.Dispose() }
            if ((Get-Sha256 $temporary) -ne $sha) { throw "Downloaded artifact SHA-256 mismatch: $sha" }
            if ($Artifact.ContainsKey('size_bytes') -and (Get-Item -LiteralPath $temporary).Length -ne $Artifact.size_bytes) {
                throw 'Downloaded artifact length does not match metadata'
            }
            [IO.File]::Move($temporary, $archive)
        } finally {
            if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
        }
    }
    if ([IO.File]::Exists($manifestPath) -and [IO.Directory]::Exists($content)) {
        $manifest = Read-Json $manifestPath
        if ($manifest.sha256 -ne $sha -or $manifest.zip -ne $isZip -or
            (-not $isZip -and $manifest.filename -cne $filename) -or
            -not (Test-CachedContent $content $manifest)) { throw "Cached artifact content is inconsistent: $sha" }
        return $content
    }
    if ([IO.Directory]::Exists($content)) { throw "Incomplete cached artifact content: $sha" }
    # Keep temporary extraction outside the 64-character cache identity. Nesting
    # another GUID below it exceeds Windows PowerShell's path limit for ordinary
    # program bundles such as OpenCppCoverage. Both directories remain on the
    # same volume, so publication below the full SHA identity is still atomic.
    # Only this newly allocated staging directory can be recursively cleaned.
    $stage = Join-Path $script:ObjectsRoot ('stage-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    if (Test-Path -LiteralPath $stage) { throw 'Staging directory already exists' }
    New-SafeDirectory $stage
    try {
        if ($isZip) { Expand-SafeZip $archive $stage }
        else { [IO.File]::Copy($archive, (Get-SafeEntryPath $stage $filename), $false) }
        $files = @(Get-ChildItem -LiteralPath $stage -File -Recurse | ForEach-Object {
            @{ path = $_.FullName.Substring($stage.Length + 1).Replace('\', '/');
               size_bytes = $_.Length; sha256 = Get-Sha256 $_.FullName }
        })
        Write-AtomicJson $manifestPath @{ sha256 = $sha; filename = $filename; zip = $isZip; files = $files }
        [IO.Directory]::Move($stage, $content)
    } finally {
        if ([IO.Directory]::Exists($stage)) {
            $resolved = [IO.Path]::GetFullPath($stage)
            if ([IO.Path]::GetDirectoryName($resolved) -ne $script:ObjectsRoot -or
                [IO.Path]::GetFileName($resolved) -notmatch '^stage-[0-9a-f]{32}\.tmp$') { throw 'Unsafe staging cleanup path' }
            Assert-NoLink $resolved
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
    return $content
}

try {
    $ClientRoot = [IO.Path]::GetFullPath($ClientRoot)
    $ConfigPath = [IO.Path]::GetFullPath($ConfigPath)
    $ResultPath = [IO.Path]::GetFullPath($ResultPath)
    if ($ReportPath) { $ReportPath = [IO.Path]::GetFullPath($ReportPath) }
    $base = Read-Json $ConfigPath
    if ($base -isnot [System.Collections.IDictionary] -or $base.client -isnot [System.Collections.IDictionary]) {
        throw 'Client configuration requires a client object'
    }
    $script:Endpoint = ([string]$base.client.deployment_endpoint).TrimEnd('/')
    $script:WorkerId = [string]$base.client.worker_id
    $effectiveWindows = $base.client.update_windows
    if ($EffectiveConfigPath) {
        $effective = Read-Json ([IO.Path]::GetFullPath($EffectiveConfigPath))
        $effectiveWindows = $effective.client.update_windows
    }
    if (-not $script:Endpoint) {
        Write-AtomicJson $ResultPath @{ status = 'current'; revision = $null; components = @() }
        exit 0
    }
    if ($script:Endpoint -notmatch '^https?://' -or $script:WorkerId -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') {
        throw 'deployment_endpoint must be HTTP(S) and worker_id must be a deployment identifier'
    }
    $deploymentRoot = Join-Path $ClientRoot 'deployments'
    New-SafeDirectory $deploymentRoot
    $script:Lock = [IO.File]::Open((Join-Path $deploymentRoot 'update.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    Send-PendingReport
    try {
        $state = Invoke-Json ($script:Endpoint + '/api/deployments/' + $script:WorkerId)
    } catch [Net.WebException] {
        $response = $_.Exception.Response
        if ($null -ne $response -and [int]$response.StatusCode -in @(409, 503)) {
            $status = [int]$response.StatusCode
            $response.Dispose()
            Write-AtomicJson $ResultPath @{ status = 'waiting_reservation'; revision = $null; components = @();
                message = "Deployment reservation unavailable (HTTP $status); keeping the active configuration" }
            exit 0
        }
        throw
    }
    if (-not $state.desired) {
        Write-AtomicJson $ResultPath @{ status = 'current'; revision = $null; components = @() }
        exit 0
    }
    $desired = $state.desired
    $script:Revision = [string]$desired.revision
    $script:ReservationId = [string]$desired.reservation_id
    if ($script:Revision -cnotmatch '^[0-9a-f]{64}$') { throw 'Deployment revision is invalid' }
    # A terminal failure releases the scheduler reservation. Even if a broken
    # download later becomes available, retrying this generation could activate
    # it without a reservation. Only a newly published revision may retry.
    if ($state.state -eq 'failed') {
        Write-AtomicJson $ResultPath @{ status = 'rejected'; revision = $script:Revision;
            reservation_id = $script:ReservationId; components = @();
            message = 'This deployment revision failed; publish a new desired revision to retry' }
        exit 0
    }
    $script:ObjectsRoot = Join-Path $deploymentRoot 'objects'
    New-SafeDirectory $script:ObjectsRoot
    $slots = @{}
    foreach ($component in $desired.components) {
        if ($component.slot -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' -or $slots.ContainsKey($component.slot)) {
            throw 'Deployment contains an invalid or repeated slot'
        }
        $slots[$component.slot] = $null
        $script:Components += @{ slot = $component.slot; artifact_id = $component.artifact_id; state = 'pending' }
    }
    # Partial updates retain the other enabled plugins. Validate the merged
    # configuration before downloading or reusing a release.
    $originalWorker = $base.client.worker_id
    $originalEndpoint = $base.client.deployment_endpoint
    $config = Merge-Config $base $(if ($desired.config_patch) { $desired.config_patch } else { @{} })
    if ($config.client -isnot [System.Collections.IDictionary]) { throw 'config_patch must preserve the client object' }
    Assert-EnabledPlugins $config
    # The Agent supplies this only for its successfully activated local generation.
    # Polling a large current dataset must not re-hash all of its files every time.
    if ($CurrentRevision -and $CurrentRevision -ceq $script:Revision) {
        foreach ($component in $script:Components) { $component.state = 'active' }
        Write-AtomicJson $ResultPath @{ status = 'current'; revision = $script:Revision; reservation_id = $script:ReservationId; components = $script:Components }
        exit 0
    }
    # The Agent also retains a verified staging generation while draining long
    # tasks or after rejecting activation. Poll it without re-reading large data.
    $preparedConfig = Join-Path (Join-Path (Join-Path $deploymentRoot 'releases') $script:Revision) 'client.json'
    if ($PreparedRevision -and $PreparedRevision -ceq $script:Revision -and [IO.File]::Exists($preparedConfig)) {
        Assert-NoLink $preparedConfig
        $prepared = Read-Json $preparedConfig
        if ($prepared -isnot [System.Collections.IDictionary] -or $prepared.client -isnot [System.Collections.IDictionary]) {
            throw 'Prepared deployment configuration is invalid'
        }
        Assert-EnabledPlugins $prepared
        foreach ($component in $script:Components) { $component.state = 'staged' }
        $status = if (Test-UpdateWindow $effectiveWindows) { 'staged' } else { 'waiting_window' }
        Write-AtomicJson $ResultPath @{ status = $status; revision = $script:Revision; reservation_id = $script:ReservationId; config_path = $preparedConfig; components = $script:Components }
        exit 0
    }
    # Management heartbeat and reports remain available outside download windows.
    if (-not (Test-UpdateWindow $effectiveWindows)) {
        Write-AtomicJson $ResultPath @{ status = 'waiting_window'; revision = $script:Revision; reservation_id = $script:ReservationId; components = $script:Components }
        exit 0
    }
    $index = 0
    foreach ($component in $desired.components) {
        if (-not (Test-UpdateWindow $effectiveWindows)) {
            Write-AtomicJson $ResultPath @{ status = 'waiting_window'; revision = $script:Revision; reservation_id = $script:ReservationId; components = $script:Components }
            exit 0
        }
        $script:Components[$index].state = 'downloading'
        $slots[$component.slot] = Stage-Artifact $component.artifact
        $script:Components[$index].state = 'staged'
        $index++
    }
    $config = Resolve-ConfigPaths $config '' $slots
    $config.client.worker_id = $originalWorker
    $config.client.deployment_endpoint = $originalEndpoint
    $releaseRoot = Join-Path (Join-Path $deploymentRoot 'releases') $script:Revision
    New-SafeDirectory $releaseRoot
    $releaseConfig = Join-Path $releaseRoot 'client.json'
    if ([IO.File]::Exists($releaseConfig)) {
        $existing = Read-Json $releaseConfig
        # Compare semantic JSON by resolving both through the same map conversion.
        $expected = ConvertTo-CanonicalJson $config
        $actual = ConvertTo-CanonicalJson $existing
        if ($actual -cne $expected) { throw 'An immutable release already exists with different base configuration; publish a new desired revision' }
    } else { Write-AtomicJson $releaseConfig $config }
    $status = if (Test-UpdateWindow $effectiveWindows) { 'staged' } else { 'waiting_window' }
    Write-AtomicJson $ResultPath @{ status = $status; revision = $script:Revision; reservation_id = $script:ReservationId; config_path = $releaseConfig; components = $script:Components }
    exit 0
} catch {
    $message = $_.Exception.Message
    foreach ($component in $script:Components) {
        if ($component.state -eq 'downloading') { $component.state = 'failed'; $component.error = $message }
    }
    Write-AtomicJson $ResultPath @{ status = 'failed'; revision = $script:Revision; reservation_id = $script:ReservationId; message = $message; components = $script:Components }
    Write-Error $message -ErrorAction Continue
    exit 1
} finally {
    if ($script:Lock) { $script:Lock.Dispose() }
}
