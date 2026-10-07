param(
    [string]$Configuration='Release',
    [ValidateSet('x64','x86','ARM64')][string]$Architecture='x64',
    [string]$BuildDirectory='',
    [string[]]$Targets=@(),
    [switch]$SkipTests
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$buildRoot=if($BuildDirectory){[IO.Path]::GetFullPath($BuildDirectory)}elseif($Architecture -eq 'x64'){Join-Path $projectRoot 'build'}else{Join-Path $projectRoot "build-$Architecture"}
$platform=if($Architecture -eq 'x86'){'Win32'}else{$Architecture}
cmake -S $projectRoot -B $buildRoot -G 'Visual Studio 17 2022' -A $platform
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
$buildArguments=@('--build',$buildRoot,'--config',$Configuration,'--parallel','4')
if($Targets.Count){$buildArguments+=@('--target')+$Targets}
cmake @buildArguments
if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
if(!$SkipTests){
    ctest --test-dir $buildRoot -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}
