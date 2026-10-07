param(
    [string]$Configuration='Release',
    [ValidateSet('x64','x86','ARM64')][string]$Architecture='x64',
    [string]$BuildDirectory='',
    [string]$Generator='',
    [string[]]$Targets=@(),
    [switch]$SkipTests
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$buildRoot=if($BuildDirectory){[IO.Path]::GetFullPath($BuildDirectory)}elseif($Architecture -eq 'x64'){Join-Path $projectRoot 'build'}else{Join-Path $projectRoot "build-$Architecture"}
$platform=if($Architecture -eq 'x86'){'Win32'}else{$Architecture}
if(!$Generator -and (Test-Path -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt'))){
    $cached=Select-String -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$' | Select-Object -First 1
    if($cached){$Generator=$cached.Matches[0].Groups[1].Value}
}
if(!$Generator){
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if(!(Test-Path -LiteralPath $vswhere)){throw 'Visual Studio C++ tools were not found'}
    $installed=& $vswhere -latest -products '*' -prerelease -property installationVersion
    if(!$installed){throw 'Visual Studio C++ tools were not found'}
    $major=([Version]$installed).Major
    $Generator=switch($major){17{'Visual Studio 17 2022'}18{'Visual Studio 18 2026'}default{throw "Unsupported Visual Studio version: $installed; specify -Generator explicitly"}}
}
cmake -S $projectRoot -B $buildRoot -G $Generator -A $platform
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
$buildArguments=@('--build',$buildRoot,'--config',$Configuration,'--parallel','4')
if($Targets.Count){$buildArguments+=@('--target')+$Targets}
cmake @buildArguments
if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
if(!$SkipTests){
    ctest --test-dir $buildRoot -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}
