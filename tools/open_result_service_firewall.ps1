<# Run once as administrator on a LAN server. Uses the configured UI port. #>
[CmdletBinding()]
param(
    [string]$ConfigPath = '',
    [string[]]$RemoteAddress = @('LocalSubnet')
)
$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Administrator privileges are required to update Windows Firewall.'
}
if (-not $RemoteAddress -or @($RemoteAddress | Where-Object { $_ }).Count -eq 0) {
    throw 'RemoteAddress must contain LocalSubnet or one or more trusted LAN CIDR ranges.'
}
if (-not $ConfigPath) {
    $root = Split-Path -Parent $PSScriptRoot
    $ConfigPath = Join-Path $root 'config/result_service.json'
    if (-not (Test-Path -LiteralPath $ConfigPath)) {
        $ConfigPath = Join-Path $root 'result_service/config.example.json'
    }
}
$config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
$listenHost = if ($env:RESULT_SERVICE_HOST) { $env:RESULT_SERVICE_HOST } else { [string]$config.host }
$listenPort = if ($env:RESULT_SERVICE_PORT) { [int]$env:RESULT_SERVICE_PORT } else { [int]$config.port }
if ($listenHost -in @('127.0.0.1', 'localhost', '::1')) {
    throw 'Set the result service host to 0.0.0.0 or its LAN address and restart it first.'
}
if ($listenPort -lt 1 -or $listenPort -gt 65535) { throw 'Configure a fixed result service port first.' }
$ruleName = "TestTracking-ResultService-$listenPort"
$rule = Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue
if ($rule) {
    $rule | Set-NetFirewallRule -Enabled True -Action Allow -Direction Inbound -Profile Any -RemoteAddress $RemoteAddress
    $rule | Get-NetFirewallPortFilter | Set-NetFirewallPortFilter -Protocol TCP -LocalPort $listenPort
} else {
    New-NetFirewallRule -Name $ruleName -DisplayName "TestTracking result UI (LAN TCP $listenPort)" `
        -Direction Inbound -Action Allow -Enabled True -Protocol TCP -LocalPort $listenPort `
        -RemoteAddress $RemoteAddress -Profile Any | Out-Null
}
$scope = $RemoteAddress -join ', '
Write-Host "LAN access allowed on TCP $listenPort from $scope; DSH uses the same port."
Write-Host "Open http://<server-LAN-IP>:$listenPort/ from another machine."
