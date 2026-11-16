#requires -Version 7.2
[CmdletBinding()]
param([string]$Out='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$local=Join-Path $root '.local'
$layout=Join-Path $local 'build/layout'
& (Join-Path $PSScriptRoot 'check-imports.ps1') -Folder $layout
& (Join-Path $PSScriptRoot 'assets.ps1')
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
& "$vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$sdk=Join-Path $env:WindowsSdkVerBinPath 'x64'
$vclibs="${env:ProgramFiles(x86)}\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64\Microsoft.VCLibs.x64.14.00.appx"
if(!(Test-Path -LiteralPath $vclibs)) { throw "Missing framework package: $vclibs" }
if(!$Out) { $Out=Join-Path $local ('packages/'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')) }
$stage=Join-Path $Out 'layout'
New-Item -ItemType Directory -Force $stage | Out-Null
foreach($name in @('AppxManifest.xml','uncharted_uwp.exe','libwinpthread-1.dll','Assets','ThirdParty','CREDITS.md','LICENSE.txt')) {
    Copy-Item -LiteralPath (Join-Path $layout $name) -Destination $stage -Recurse
}
foreach($name in @('vulkan_dzn.dll','dxil.dll','bbprobe.dll','bbgpu_dll.dll')) {
    if(Test-Path -LiteralPath (Join-Path $layout $name)) {
        Copy-Item -LiteralPath (Join-Path $layout $name) -Destination $stage
    }
}
$now=[DateTime]::UtcNow
$version='1.0.{0}.{1}' -f ($now.Year%100*1000+$now.DayOfYear),([int]$now.ToString('HHmm'))
$manifest=Join-Path $stage 'AppxManifest.xml'
$content=(Get-Content -LiteralPath $manifest -Raw).Replace('Version="1.0.0.0"',"Version=`"$version`"")
[IO.File]::WriteAllText($manifest,$content,[Text.UTF8Encoding]::new($false))
$package=Join-Path $Out "UnchartedNDCUWP_$version`_x64.appx"
& "$sdk/makeappx.exe" pack /d $stage /p $package /o *> (Join-Path $Out 'pack.log')
if($LASTEXITCODE) { Get-Content (Join-Path $Out 'pack.log') -Tail 20; throw 'Package creation failed' }
$pfx=Join-Path $root '.local/signing.pfx'
if(!(Test-Path -LiteralPath $pfx)) { throw "Missing signing key: $pfx (see BUILD.md, section 6)" }
& "$sdk/signtool.exe" sign /q /fd SHA256 /f $pfx $package
if($LASTEXITCODE) { throw 'Signing failed' }
Remove-Item -LiteralPath $stage -Recurse -Force
New-Item -ItemType Directory -Force (Join-Path $Out 'Dependencies') | Out-Null
Copy-Item -LiteralPath $vclibs -Destination (Join-Path $Out 'Dependencies')
$zip=Join-Path $Out "UnchartedNDCUWP-$version.zip"
Compress-Archive -Path $package,(Join-Path $Out 'Dependencies') -DestinationPath $zip
[IO.File]::WriteAllText((Join-Path $local 'latest-package.txt'),$package,[Text.UTF8Encoding]::new($false))
Write-Output "Signed package created: $version."
Write-Output "version=$version"
Write-Output "zip=$zip"
