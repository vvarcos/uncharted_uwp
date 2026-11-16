<#
.SYNOPSIS
Install the exact latest M2 package, launch it on Xbox, retain loader.log, and require the
Dozen/CoreWindow and bbgpu archive-link evidence. This script does not run guest code.
#>
#requires -Version 7.2
[CmdletBinding()]
param(
    [string]$Console = 'series-s',
    [string]$Url = '',
    [switch]$Insecure,
    [ValidateRange(1,3600)][int]$TimeoutSeconds = 300,
    [ValidateRange(1,120)][int]$SettleSeconds = 12
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$latestFile = Join-Path $root '.local/latest-package.txt'
if (!(Test-Path -LiteralPath $latestFile)) { throw "Missing $latestFile; run package.ps1 first." }
$package = (Get-Content -LiteralPath $latestFile -Raw).Trim()
if (!(Test-Path -LiteralPath $package)) { throw "Latest package does not exist: $package" }
$dependency = Get-ChildItem -LiteralPath (Join-Path (Split-Path $package -Parent) 'Dependencies') -Filter '*.appx' | Select-Object -First 1
if (!$dependency) { throw 'The package dependency APPX is missing.' }

function Invoke-JsonScript([string]$Name, [hashtable]$Arguments) {
    $script = Join-Path $PSScriptRoot $Name
    $Arguments.Json = $true
    $text = & $script @Arguments 2>&1
    if ($LASTEXITCODE) { throw "$Name failed ($LASTEXITCODE): $($text -join [Environment]::NewLine)" }
    try { $envelope = (($text -join [Environment]::NewLine) | ConvertFrom-Json) }
    catch { throw "$Name did not return valid JSON: $($text -join [Environment]::NewLine)" }
    if (!$envelope.ok) { throw "$Name reported failure: $($envelope.error.message)" }
    return $envelope.data
}

$common = @{ Console=$Console; Url=$Url; Insecure=$Insecure; TimeoutSeconds=$TimeoutSeconds }
$install = Invoke-JsonScript 'xbox-install.ps1' ($common + @{ Package=$package; Dependency=$dependency.FullName })
$fullName = [string]$install.package
if (!$install.installed -or !$fullName) { throw 'Exact package installation was not confirmed.' }
$launch = Invoke-JsonScript 'xbox-launch.ps1' ($common + @{ App=$fullName })
if (!$launch.running_observed) { throw 'The installed package was not observed running.' }
Start-Sleep -Seconds $SettleSeconds
$download = Invoke-JsonScript 'xbox-download.ps1' ($common + @{ App=$fullName; RemotePath='LocalState/loader.log'; MaxMiB=8 })
$sourceLog = [string]$download.file
if (!(Test-Path -LiteralPath $sourceLog)) { throw 'loader.log download was not retained.' }
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$evidenceDir = Join-Path $root ".local/evidence/m2-$stamp"
$null = New-Item -ItemType Directory -Path $evidenceDir -Force
$evidenceLog = Join-Path $evidenceDir 'loader.log'
Copy-Item -LiteralPath $sourceLog -Destination $evidenceLog
$text = Get-Content -LiteralPath $evidenceLog -Raw
$required = @(
    'Dozen device: Microsoft Direct3D12 (SraKmd_arden), Vulkan 1.2',
    'CoreWindow first frame presented',
    'Graphics probe: Vulkan clear -> shared D3D12 image -> CoreWindow PRESENTED',
    'M2 graphics probe returned 0',
    'M2 bbgpu archive linked: marker=0xbb02',
    'M2 bbgpu link smoke returned 0'
)
$missing = @($required | Where-Object { !$text.Contains($_) })
$result = [ordered]@{
    verified = !$missing.Count
    package = $fullName
    process_ids = @($launch.process_ids)
    evidence = $evidenceLog
    missing = $missing
}
$result | ConvertTo-Json -Depth 5
if ($missing.Count) { throw "M2 console evidence incomplete: $($missing -join '; ')" }
