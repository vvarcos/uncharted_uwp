#requires -Version 7.2
<#
.SYNOPSIS
Prepare the Uncharted BBPROBE5 image and AppContent profile on the PC for the UWP M3 loader.
The dump may be the minimal four-file folder produced by the Xbox `stage` mode.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Dump,
    [string]$Out='',
    [ValidateSet('eboot.bin','big2-ps4_Shipping.elf','big3-ps4_Shipping.elf')][string]$Exe='eboot.bin',
    [ValidateSet('full','trial')][string]$Sku='full',
    [string]$Python='python'
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$repo=Split-Path $root -Parent
if(!(Test-Path -LiteralPath (Join-Path $Dump $Exe))) { throw "$Dump does not contain $Exe" }
foreach($required in @('sce_module/libc.prx','sce_module/libSceFios2.prx','sce_sys/param.sfo')) {
    if(!(Test-Path -LiteralPath (Join-Path $Dump $required))) { throw "$Dump does not contain $required" }
}
if(!$Out) { $Out=Join-Path $root '.local/prepared-u1' }
New-Item -ItemType Directory -Force $Out,(Join-Path $Out 'trophies') | Out-Null
& $Python (Join-Path $repo 'scripts/prepare.py') $Dump --out $Out --exe $Exe
if($LASTEXITCODE) { throw 'Uncharted executable preparation failed' }
& $Python (Join-Path $repo 'scripts/link_modules.py') $Dump --out $Out --exe $Exe
if($LASTEXITCODE) { throw 'Uncharted BBPROBE5 linking failed' }
& $Python (Join-Path $repo 'scripts/content_profile.py') $Dump --out $Out --sku $Sku
if($LASTEXITCODE) { throw 'Uncharted AppContent preparation failed' }
[IO.File]::WriteAllText((Join-Path $Out 'format.txt'),"uncharted-uwp-bbprobe5 1`n",[Text.Encoding]::ASCII)
Write-Output "Prepared Uncharted BBPROBE5 files written to $Out"
