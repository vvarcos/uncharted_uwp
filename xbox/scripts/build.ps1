#requires -Version 7.2
[CmdletBinding()]
param([switch]$Graphics,[switch]$LinkRenderer,[switch]$Guest,[switch]$VideoContract,[switch]$HeadlessInstance,[switch]$HeadlessDevice)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$local=Join-Path $root '.local'
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
& "$vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$llvm=if(Test-Path "$vs/VC/Tools/Llvm/x64/bin/clang-cl.exe") { "$vs/VC/Tools/Llvm/x64/bin" } else { 'C:/Program Files/LLVM/bin' }
if(!(Test-Path "$llvm/clang-cl.exe")) { throw 'clang-cl is missing (Visual Studio LLVM or C:\Program Files\LLVM)' }
$env:PATH="$llvm;$vs/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin;C:/Program Files/CMake/bin;C:/Program Files/Ninja;$env:PATH"
$build=Join-Path $local 'build'
if($HeadlessDevice) { $HeadlessInstance=$true }
if($HeadlessInstance) { $VideoContract=$true }
if($VideoContract) { $Guest=$true }
if($Guest) { $LinkRenderer=$true }
if($LinkRenderer) { $Graphics=$true }
$options=@("-DUNCHARTED_BUILD_GRAPHICS=$($Graphics.IsPresent)","-DUNCHARTED_LINK_RENDERER=$($LinkRenderer.IsPresent)","-DUNCHARTED_BUILD_GUEST=$($Guest.IsPresent)","-DUNCHARTED_M5_VIDEO_CONTRACT=$($VideoContract.IsPresent)","-DUNCHARTED_M5_HEADLESS_INSTANCE=$($HeadlessInstance.IsPresent)","-DUNCHARTED_M5_HEADLESS_DEVICE=$($HeadlessDevice.IsPresent)")
if($Graphics) {
    $config=Get-Content -LiteralPath (Join-Path $local 'settings.json') -Raw | ConvertFrom-Json
    $options += "-DUNCHARTED_DOZEN_DLL=$($config.dozen_dll)"
    $options += "-DUNCHARTED_DXIL_DLL=$($config.dxil_dll)"
}
& cmake -S $root -B $build -G Ninja "-DCMAKE_C_COMPILER=$llvm/clang-cl.exe" "-DCMAKE_CXX_COMPILER=$llvm/clang-cl.exe" `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo @options
if($LASTEXITCODE) { throw 'UWP configure failed' }
& cmake --build $build --parallel 4 *> (Join-Path $local 'build.log')
if($LASTEXITCODE) { Get-Content (Join-Path $local 'build.log') -Tail 50; throw 'UWP build failed; details in xbox/.local/build.log' }
Write-Output 'Uncharted UWP loader built; logs and layout are in xbox/.local.'
