<#
.SYNOPSIS
Install the exact latest M4 package and collect independent Xbox equeue, memory, init and
controlled-entry logs. No success is asserted without console logs from that installed package.
#>
#requires -Version 7.2
[CmdletBinding()]
param(
    [string]$Console = 'series-s',
    [string]$Url = '',
    [switch]$Insecure,
    [ValidateRange(1,3600)][int]$TimeoutSeconds = 300,
    [ValidateRange(1,120)][int]$SettleSeconds = 20,
    [string]$Dump = 'E:\dumps\CUSA02320'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$latest = Join-Path $root '.local/latest-package.txt'
if (!(Test-Path -LiteralPath $latest)) { throw 'Build and package M4 first.' }
$package = (Get-Content -LiteralPath $latest -Raw).Trim()
if (!(Test-Path -LiteralPath $package)) { throw "Missing package $package" }
$dependencies = @(Get-ChildItem -LiteralPath (Join-Path (Split-Path $package -Parent) 'Dependencies') -Filter '*.appx')
if (!$dependencies.Count) { throw 'Missing packaged dependency APPX.' }
$prepared = Join-Path $root '.local/prepared-u1'
foreach ($name in @('boot-linked.bin','content.bin')) {
    if (!(Test-Path -LiteralPath (Join-Path $prepared $name))) { throw "Missing prepared $name" }
}
function Invoke-JsonScript([string]$Name, [hashtable]$Arguments) {
    $Arguments.Json = $true
    $text = & (Join-Path $PSScriptRoot $Name) @Arguments 2>&1
    if ($LASTEXITCODE) { throw "$Name failed ($LASTEXITCODE): $($text -join [Environment]::NewLine)" }
    try { $envelope = (($text -join [Environment]::NewLine) | ConvertFrom-Json) }
    catch { throw "$Name returned invalid JSON: $($text -join [Environment]::NewLine)" }
    if (!$envelope.ok) { throw "$Name failed: $($envelope.error.message)" }
    return $envelope.data
}
$common = @{ Console=$Console; Url=$Url; Insecure=$Insecure; TimeoutSeconds=$TimeoutSeconds }
$install = Invoke-JsonScript 'xbox-install.ps1' ($common + @{
    Package=$package; Dependency=@($dependencies | ForEach-Object FullName)
})
$app = [string]$install.package
if (!$install.installed -or !$app) { throw 'The exact M4 package was not installed.' }
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$evidenceDir = Join-Path $root ".local/evidence/m4-$stamp"
New-Item -ItemType Directory -Force -Path $evidenceDir | Out-Null
foreach ($name in @('boot-linked.bin','content.bin')) {
    $null = Invoke-JsonScript 'xbox-upload.ps1' ($common + @{
        # Device Portal cannot create LocalState/Prepared; main.cpp accepts this flat layout.
        App=$app; File=(Join-Path $prepared $name); RemotePath="LocalState/$name"; Overwrite=$true
    })
}
$checks = @{
    equeue = @('PASS: real shadPS4 equeue create, timed wait, user trigger, timer and delete',
               'M4 real kernel equeue self-test returned 0')
    memory = @('UWP memory contract self-test starting','Memory contract self-test returned 0','corrupt=0')
    init = @('BBPROBE5 loaded: modules=2','M4: real kernel equeue contracts registered: 17 symbols',
             'Module 0 initializer returned 0','Module 1 initializer returned 0',
             'M3 INIT READY: BBPROBE5 modules initialized; guest entry withheld','M4 Uncharted loader returned 0')
    entry = @('M4: real kernel equeue contracts registered: 17 symbols',
              'Entering original x86-64 code at guest offset 0x350d70',
              'Original guest entry instructions executed; game initialization is incomplete.',
              'UWP memory: commit faults=','STOP: first unsupported PS4 import:')
}
$result = [ordered]@{ verified=$false; package=$app; evidence=$evidenceDir; runs=@() }
foreach ($mode in @('equeue','memory','init','entry')) {
    $runFile = Join-Path $evidenceDir "$mode-run.txt"
    [IO.File]::WriteAllText($runFile,"$mode`n$Dump`n",[Text.Encoding]::ASCII)
    $null = Invoke-JsonScript 'xbox-upload.ps1' ($common + @{
        App=$app; File=$runFile; RemotePath='LocalState/run.txt'; Overwrite=$true
    })
    $stop = Invoke-JsonScript 'xbox-stop.ps1' ($common + @{ App=$app })
    if (!$stop.stopped_observed) { throw "Could not stop app before $mode" }
    $launch = Invoke-JsonScript 'xbox-launch.ps1' ($common + @{ App=$app })
    if (!$launch.running_observed) { throw "Did not observe running app in $mode" }
    Start-Sleep -Seconds $SettleSeconds
    $download = Invoke-JsonScript 'xbox-download.ps1' ($common + @{
        App=$app; RemotePath='LocalState/loader.log'; MaxMiB=8
    })
    if (!(Test-Path -LiteralPath $download.file)) { throw "Missing console loader.log in $mode" }
    $log = Join-Path $evidenceDir "$mode-loader.log"
    Copy-Item -LiteralPath $download.file -Destination $log
    $text = Get-Content -LiteralPath $log -Raw
    $missing = @($checks[$mode] | Where-Object { !$text.Contains($_) })
    if (!$text.Contains('UNCHARTED UWP loader milestone 4')) { $missing += 'UWP host milestone label' }
    if ($text.Contains('STOP: UWP') -or $text -match 'corrupt=[1-9]') { $missing += 'guest memory STOP/corruption' }
    if ($mode -eq 'init' -and $text.Contains('Entering original')) { $missing += 'init-only entered guest code' }
    if ($mode -eq 'entry' -and $text.Contains('API: sceKernelCreateEqueue')) { $missing += 'old equeue frontier remains' }
    $result.runs += [ordered]@{ mode=$mode; process_ids=@($launch.process_ids); log=$log; missing=$missing }
    if ($missing.Count) { $result | ConvertTo-Json -Depth 8; throw "$mode console evidence incomplete: $($missing -join '; ')" }
}
$result.verified = $true
$result | ConvertTo-Json -Depth 8
