<#
.SYNOPSIS
Send scripted input to the console app, or clear it so the physical controller has full control.
.DESCRIPTION
The runtime merges LocalState/pad.txt with the live controller: tokens are held only while the file
says so. Every mode except Clear ends by uploading an empty file, so nothing stays pressed and the
controller keeps working.
.EXAMPLE
./pad.ps1 cross                 # one Cross press (~350 ms)
./pad.ps1 down -Hold 0.2        # a short D-pad tap
./pad.ps1 'lx=255' -Hold 3      # hold the left stick right for 3 s
./pad.ps1 clear                 # release everything now
#>
#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string[]]$Tokens = @(),
    [double]$Hold = 0.35,
    # Sticks are deliberately opt-in: a held axis can leave the character walking after the
    # agent stops. The D-pad is discrete and safe.
    [switch]$Stick,
    [string]$Console = 'series-s',
    [string]$App = '',
    [switch]$Json
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Xbox.Common.ps1')

if (!$App) {
    $config = Get-XboxDeviceConfig $Console '' ''
    $base = [string]$config.url
    $packages = (Invoke-RestMethod ($base.TrimEnd('/') + '/api/app/packagemanager/packages') -SkipCertificateCheck).InstalledPackages |
        Where-Object { $_.PackageFullName -match 'NDCUWP' }
    $newest = $packages | Sort-Object { [int]$_.Version.Build * 100000 + [int]$_.Version.Revision } -Descending |
        Select-Object -First 1
    if (!$newest) { throw 'No Uncharted package installed on this console.' }
    $App = $newest.PackageFullName
}

$temp = Join-Path $root '.local/pad-inject.txt'
$upload = Join-Path $PSScriptRoot 'xbox-upload.ps1'
try {
    $text = ($Tokens -join ' ').Trim()
    if (-not $Stick -and $text -match '(^|\s)[lr][xy]=') {
        throw 'Stick tokens are disabled; use the D-pad (up/down/left/right) or pass -Stick.'
    }
    if ($text -and $text -ne 'clear') {
        [IO.File]::WriteAllText($temp, $text, [Text.ASCIIEncoding]::new())
        & $upload -Console $Console -App $App -File $temp -RemotePath 'LocalState/pad.txt' -Overwrite | Out-Null
        if ($Hold -gt 0) { Start-Sleep -Milliseconds ([int]($Hold * 1000)) }
    }
    # Always finish empty: the runtime releases everything and the controller owns the pad again.
    [IO.File]::WriteAllText($temp, '', [Text.ASCIIEncoding]::new())
    & $upload -Console $Console -App $App -File $temp -RemotePath 'LocalState/pad.txt' -Overwrite | Out-Null
    if ($Json) { @{ ok = $true; app = $App; tokens = $text } | ConvertTo-Json -Compress }
    else { "pad: '$text' sent and released on $App" }
} finally {
    Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue
}
