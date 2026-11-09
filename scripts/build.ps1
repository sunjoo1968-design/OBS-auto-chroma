param(
    [ValidateSet(32,33)][int]$ObsMajor = 33,
    [ValidateSet('Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo',
    [string]$LuaLibrary = 'C:\Program Files\obs-studio\bin\64bit\lua51.dll'
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBuild = Join-Path $taskRoot "build\obs$ObsMajor"
$taskPackage = Join-Path $taskRoot "dist\obs$ObsMajor"
& cmake -S $taskRoot -B $taskBuild -G 'Visual Studio 17 2022' -A x64 "-DAC_OBS_MAJOR=$ObsMajor" "-DAC_LUA_LIBRARY=$LuaLibrary"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& cmake --build $taskBuild --config $Configuration -- /m:1 /nr:false
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& ctest --test-dir $taskBuild -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Regression tests failed' }
& cmake --install $taskBuild --config $Configuration --prefix $taskPackage
if ($LASTEXITCODE -ne 0) { throw 'Packaging failed' }
Write-Output "SunjooAn v1.3 OBS $ObsMajor package: $taskPackage"
