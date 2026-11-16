<#
.SYNOPSIS
Validate only the M5 diagnostic VideoOut contract and capability audit on the Xbox.
This script never claims the M5 gameplay/10-minute memory milestone is achieved.
#>
#requires -Version 7.2
[CmdletBinding()]
param(
    [string]$Console='series-s', [string]$Url='', [switch]$Insecure,
    [ValidateRange(1,3600)][int]$TimeoutSeconds=300,
    [ValidateRange(1,120)][int]$SettleSeconds=20,
    [string]$Dump='E:\dumps\CUSA02320',
    [switch]$HeadlessInstance, [switch]$HeadlessDevice
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$package=(Get-Content -LiteralPath (Join-Path $root '.local/latest-package.txt') -Raw).Trim()
if(!(Test-Path -LiteralPath $package)) { throw 'Package not found; build -VideoContract and package first.' }
$dependencies=@(Get-ChildItem -LiteralPath (Join-Path (Split-Path $package -Parent) Dependencies) -Filter '*.appx')
if(!$dependencies.Count) { throw 'Package dependencies missing.' }
$prepared=Join-Path $root '.local/prepared-u1'
foreach($name in @('boot-linked.bin','content.bin')) {
    if(!(Test-Path -LiteralPath (Join-Path $prepared $name))) { throw "Missing BBPROBE5 artifact $name" }
}
function Invoke-PortalScript([string]$Name,[hashtable]$Parameters) {
    $Parameters.Json=$true
    $output=& (Join-Path $PSScriptRoot $Name) @Parameters 2>&1
    if($LASTEXITCODE) { throw "$Name exited $LASTEXITCODE`: $($output -join [Environment]::NewLine)" }
    try { $response=(($output -join [Environment]::NewLine) | ConvertFrom-Json) }
    catch { throw "$Name returned invalid JSON: $($output -join [Environment]::NewLine)" }
    if(!$response.ok) { throw "$Name error: $($response.error.message)" }
    return $response.data
}
$common=@{Console=$Console;Url=$Url;Insecure=$Insecure;TimeoutSeconds=$TimeoutSeconds}
$install=Invoke-PortalScript 'xbox-install.ps1' ($common + @{
    Package=$package;Dependency=@($dependencies | ForEach-Object FullName)})
$app=[string]$install.package
if(!$install.installed -or !$app) { throw 'Exact diagnostic package installation not confirmed.' }
$stamp=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$evidence=Join-Path $root ".local/evidence/m5-contract-$stamp"
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
foreach($name in @('boot-linked.bin','content.bin')) {
    $null=Invoke-PortalScript 'xbox-upload.ps1' ($common + @{
        # Device Portal cannot create LocalState/Prepared; main.cpp accepts this flat layout.
        App=$app;File=(Join-Path $prepared $name);RemotePath="LocalState/$name";Overwrite=$true})
}
$required=@{
    'renderer-instance'=@('M5 renderer instance smoke: packaged Dozen, headless Vulkan 1.2; guest renderer disabled',
                          'M5 renderer CreateInstance PASS:','M5 renderer Vulkan instance smoke returned 0')
    'renderer-swapchain'=@('M5 swapchain: created over the CoreWindow bridge',
                           'M5 swapchain: image acquired',
                           'CoreWindow first frame presented',
                           'M5 swapchain PASS: renderer swapchain presented a synthetic frame to CoreWindow',
                           'M5 renderer swapchain smoke returned 0')
    'renderer-hostpass'=@('M5 host pass created: layout, pipeline and descriptor sets',
                          'M5 host pass recorded: descriptor set bound and draw issued',
                          'M5 host pass PASS: descriptor sets, push constants and dynamic rendering on device;',
                          'M5 renderer host pass smoke returned 0')
    'renderer-compute-empty'=@('M5 empty compute dispatch: vkCmdDispatch returned',
                                'M5 empty compute dispatch PASS:',
                                'M5 renderer empty compute smoke returned 0')
    'renderer-descriptor'=@('M5 renderer descriptor readback: 10203040 50607080 90A0B0C0 00000000',
                             'M5 renderer descriptor sets PASS: two command buffers, fence-then-update/reuse;',
                             'M5 renderer descriptor smoke returned 0')
    'renderer-offscreen'=@('M5 renderer offscreen readback: left=00FF00FF right=FF0000FF',
                           'M5 renderer offscreen draw/readback PASS:',
                           'M5 renderer offscreen smoke returned 0')
    'renderer-class-device'=@('M5 renderer Instance::CreateDevice:',
                              'M5 renderer class device PASS:',
                              'M5 Vulkan::Instance class device returned 0')
    'renderer-device'=@('M5 renderer logical device PASS:',
                         'M5 renderer Vulkan device smoke returned 0')
    'video-contract'=@('PASS: logical VideoOut Open/Close/reopen, flip equeue add/delete/cleanup, buffer attribute (no synthetic frame), invalid bus/index and resolution; rendering withheld',
                      'M5 logical VideoOut contract self-test returned 0')
    'gnm-event'=@('PASS: real GNM GraphicsCore equeue/IRQ callback, explicit/queue-delete cleanup (self-test IRQ only); guest GPU disabled',
                  'M5 GNM event contract self-test returned 0')
    'equeue'=@('PASS: real shadPS4 equeue create, timed wait, user trigger, timer and delete',
               'M4 real kernel equeue self-test returned 0')
    'memory'=@('Memory contract self-test returned 0','corrupt=0')
    'init'=@('M5: HLE registered: 25 symbols (17 real equeue + 6 logical VideoOut + 2 GNM IRQ; renderer absent)',
             'BBPROBE5 loaded: modules=2','Module 0 initializer returned 0','Module 1 initializer returned 0',
             'guest entry withheld','M5 diagnostic Uncharted loader returned 0')
    'entry'=@('M5 logical VideoOut port opened: handle=1','M5 VideoOut flip event registered: equeue=',
              'M5 GNM equeue IRQ registered: eq=',
              'M5 VideoOut buffer attribute: format=',
              'Entering original x86-64 code at guest offset 0x350d70',
              'UWP app memory report: used=','Original guest entry instructions executed; game initialization is incomplete.')
    'probe'=@('M5 renderer capability audit:','M2 graphics probe returned 0',
              'Graphics probe: Vulkan clear -> shared D3D12 image -> CoreWindow PRESENTED')
}
$result=[ordered]@{verified_contract=$false;milestone5_complete=$false;package=$app;evidence=$evidence;runs=@()}
$modes=@('video-contract','gnm-event','equeue','memory','init','entry','probe')
if($HeadlessDevice) { $HeadlessInstance=$true }
if($HeadlessInstance) { $modes=@('renderer-instance')+$modes }
if($HeadlessDevice) { $modes=@('renderer-swapchain','renderer-hostpass','renderer-compute-empty','renderer-descriptor','renderer-offscreen','renderer-class-device','renderer-device')+$modes }
foreach($mode in $modes) {
    $runFile=Join-Path $evidence "$mode-run.txt"
    [IO.File]::WriteAllText($runFile,"$mode`n$Dump`n",[Text.Encoding]::ASCII)
    $null=Invoke-PortalScript 'xbox-upload.ps1' ($common + @{
        App=$app;File=$runFile;RemotePath='LocalState/run.txt';Overwrite=$true})
    $stop=Invoke-PortalScript 'xbox-stop.ps1' ($common + @{App=$app})
    if(!$stop.stopped_observed) { throw "App not stopped before $mode" }
    $launch=Invoke-PortalScript 'xbox-launch.ps1' ($common + @{App=$app})
    if(!$launch.running_observed) { throw "App not running for $mode" }
    Start-Sleep -Seconds $SettleSeconds
    $download=Invoke-PortalScript 'xbox-download.ps1' ($common + @{
        App=$app;RemotePath='LocalState/loader.log';MaxMiB=8})
    if(!(Test-Path -LiteralPath $download.file)) { throw "$mode console log missing" }
    $log=Join-Path $evidence "$mode-loader.log"
    Copy-Item -LiteralPath $download.file -Destination $log
    $text=Get-Content -LiteralPath $log -Raw
    $missing=@($required[$mode] | Where-Object { !$text.Contains($_) })
    if(!$text.Contains('UNCHARTED UWP M5 diagnostic:')) { $missing+='M5 diagnostic banner' }
    if($text.Contains('STOP: UWP') -or $text -match 'corrupt=[1-9]') { $missing+='UWP memory STOP/corruption' }
    if($mode -eq 'init' -and $text.Contains('Entering original')) { $missing+='guest entry in init-only' }
    if($mode -eq 'entry' -and ($text.Contains('API: sceVideoOutOpen') -or
        $text.Contains('API: sceVideoOutAddFlipEvent') -or
        $text.Contains('API: sceGnmAddEqEvent') -or
        $text.Contains('API: sceVideoOutSetBufferAttribute'))) { $missing+='old attribute/event frontier remains' }
    $result.runs += [ordered]@{mode=$mode;process_ids=@($launch.process_ids);log=$log;missing=$missing}
    if($missing.Count) { $result | ConvertTo-Json -Depth 8; throw "$mode diagnostic incomplete: $($missing -join '; ')" }
}
$result.verified_contract=$true
$result | ConvertTo-Json -Depth 8
