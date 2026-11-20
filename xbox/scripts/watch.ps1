<#
.SYNOPSIS
Watch one console run until the app exits, capturing the screen at a fixed interval.
.DESCRIPTION
Stops by itself when the package is no longer running, when the timeout is reached, or on Ctrl+C.
Frames are written to <OutDir>/<elapsed>s.png and a one-line summary is printed at the end, so a run
that dies at 40 s is not mistaken for a loading screen at 30 s.
.EXAMPLE
./watch.ps1 -Interval 5 -MaxMinutes 15
#>
#requires -Version 7.2
[CmdletBinding()]
param(
    [string]$Console = 'series-s',
    [string]$App = '',
    [ValidateRange(1, 120)][int]$Interval = 5,
    [ValidateRange(1, 240)][int]$MaxMinutes = 20,
    [string]$OutDir = '',
    [switch]$NoCapture
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Xbox.Common.ps1')

$portal = Open-XboxPortal $Console '' $false 60
$base = (Get-XboxDeviceConfig $Console '' '').url.TrimEnd('/')
if (!$App) {
    $packages = (Invoke-RestMethod "$base/api/app/packagemanager/packages" -SkipCertificateCheck).InstalledPackages
    $App = ($packages | Where-Object { $_.PackageFullName -match 'NDCUWP' } |
        Sort-Object { [int]$_.Version.Build * 100000 + [int]$_.Version.Revision } -Descending |
        Select-Object -First 1).PackageFullName
    if (!$App) { throw 'No Uncharted package installed on this console.' }
}
if (!$OutDir) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutDir = Join-Path $root ".local/watch/$stamp"
}
New-Item -ItemType Directory -Force $OutDir | Out-Null

$started = Get-Date
$deadline = $started.AddMinutes($MaxMinutes)
$frames = 0
$shotScript = Join-Path $PSScriptRoot 'xbox-screenshot.ps1'
$result = 'timeout'

while ((Get-Date) -lt $deadline) {
    $processes = @((Invoke-RestMethod ((Get-XboxDeviceConfig $Console '' '').url.TrimEnd('/') +
        '/api/resourcemanager/processes') -SkipCertificateCheck).Processes |
        Where-Object { $_.PSObject.Properties.Name -contains 'PackageFullName' } |
        Where-Object { $_.PackageFullName -eq $App -and $_.IsRunning })
    if (!$processes) {
        $result = 'app-exited'
        break
    }
    $elapsed = [int]((Get-Date) - $started).TotalSeconds
    if (!$NoCapture) {
        $before = @(Get-ChildItem (Join-Path $root '.local/transfers') -Directory |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1)
        & $shotScript -Console $Console *> $null
        $after = @(Get-ChildItem (Join-Path $root '.local/transfers') -Directory |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1)
        if ($after.Count -and $after[0].FullName -ne $before[0].FullName -and
            (Test-Path (Join-Path $after[0].FullName 'screenshot.png'))) {
            Copy-Item (Join-Path $after[0].FullName 'screenshot.png') `
                (Join-Path $OutDir ("{0:d4}s.png" -f $elapsed)) -Force
            $frames++
        }
    }
    Start-Sleep -Seconds $Interval
}

$minutes = [math]::Round(((Get-Date) - $started).TotalMinutes, 1)
"watch: $result after $minutes min, $frames frames in $OutDir"
