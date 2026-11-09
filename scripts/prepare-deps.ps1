param(
    [Parameter(Mandatory=$true)][ValidateSet(32,33)][int]$ObsMajor,
    [string]$RuntimeDirectory,
    [string]$Workspace = (Split-Path -Parent $PSScriptRoot)
)
$ErrorActionPreference = 'Stop'
$taskWorkspace = [System.IO.Path]::GetFullPath($Workspace)
$taskDeps = Join-Path $taskWorkspace '.deps'
$taskDownloads = Join-Path $taskDeps 'downloads'
$taskSdk = Join-Path $taskDeps "obs-sdk-$ObsMajor"
$taskQt = Join-Path $taskDeps "qt$ObsMajor"
$taskVersion = if ($ObsMajor -eq 32) { '32.0.0' } else { '33.0.1' }
$taskQtRelease = if ($ObsMajor -eq 32) { '2025-08-23' } else { '2026-10-23' }
$taskSourceHash = if ($ObsMajor -eq 32) { '24ab750ee0b26fd4cc8f14029e5660d564962ba62f8c615af4c83bff6fef544c' } else { 'c8d3a8f69a0f17970ac0a8f2a2462f1bc4dacd324f6da0c4b408d442d4798913' }
$taskQtHash = if ($ObsMajor -eq 32) { 'e60e68643bf8a73b395335fc86d7615c96f8fd9c83e6e61cb8246d55e9186684' } else { 'fb2512e4308ebd37a6c732c16c0f3f92fa9a9d58df97408f2acae53cab0623da' }
$taskSourceUrl = "https://github.com/obsproject/obs-studio/releases/download/$taskVersion/OBS-Studio-$taskVersion-Sources.tar.gz"
$taskQtUrl = "https://github.com/obsproject/obs-deps/releases/download/$taskQtRelease/windows-deps-qt6-$taskQtRelease-x64-RelWithDebInfo.zip"

function Get-VerifiedArchive([string]$Url, [string]$Path, [string]$Hash) {
    if (!(Test-Path -LiteralPath $Path)) {
        Write-Host "Downloading $Url"
        & curl.exe -sSL --fail --retry 2 -o $Path $Url
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $Url" }
    }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Hash) {
        throw "SHA-256 mismatch: $Path. Remove this archive and retry."
    }
}

New-Item -ItemType Directory -Path $taskDownloads, $taskSdk, "$taskSdk\lib" -Force | Out-Null
$taskSourceArchive = Join-Path $taskDownloads "obs$ObsMajor-sources.tar.gz"
Get-VerifiedArchive $taskSourceUrl $taskSourceArchive $taskSourceHash
$taskSourceRoot = Join-Path $taskSdk "obs-studio-$taskVersion-sources"
if (!(Test-Path -LiteralPath "$taskSourceRoot\libobs\obs.h")) {
    & tar.exe -xzf $taskSourceArchive -C $taskSdk "obs-studio-$taskVersion-sources/libobs" "obs-studio-$taskVersion-sources/frontend/api" "obs-studio-$taskVersion-sources/COPYING" "obs-studio-$taskVersion-sources/CMakePresets.json"
    if ($LASTEXITCODE -ne 0) { throw 'OBS header extraction failed' }
}
if (!(Test-Path -LiteralPath "$taskSdk\include\obs.h")) {
    Copy-Item -LiteralPath "$taskSourceRoot\libobs" -Destination "$taskSdk\include" -Recurse
}
Copy-Item -LiteralPath "$taskSourceRoot\frontend\api\obs-frontend-api.h" -Destination "$taskSdk\include\obs-frontend-api.h" -Force
Set-Content -LiteralPath "$taskSdk\include\obsconfig.h" -Encoding utf8 -Value @(
    '#pragma once',
    '#define OBS_DATA_PATH "data"',
    '#define OBS_PLUGIN_PATH "obs-plugins/64bit"',
    '#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"',
    '#define OBS_INSTALL_PREFIX ""',
    '#define OBS_RELEASE_CANDIDATE 0',
    '#define OBS_BETA 0'
)

# Qt archives contain release PDBs. A previously extracted SDK may omit only PDBs;
# range-extraction-manifest.json records the individual files used in that case.
if (!(Test-Path -LiteralPath "$taskQt\lib\cmake\Qt6\Qt6Config.cmake")) {
    $taskQtArchive = Join-Path $taskDownloads "qt$ObsMajor-sdk.zip"
    Get-VerifiedArchive $taskQtUrl $taskQtArchive $taskQtHash
    New-Item -ItemType Directory -Path $taskQt -Force | Out-Null
    Expand-Archive -LiteralPath $taskQtArchive -DestinationPath $taskQt -Force
}

if (!$RuntimeDirectory) {
    $RuntimeDirectory = if ($ObsMajor -eq 32) { "$taskSdk\runtime\bin\64bit" } else { 'C:\Program Files\obs-studio\bin\64bit' }
}
$taskRuntime = [System.IO.Path]::GetFullPath($RuntimeDirectory)
$taskObsDll = Join-Path $taskRuntime 'obs.dll'
if (!(Test-Path -LiteralPath $taskObsDll)) {
    throw "Provide -RuntimeDirectory pointing to OBS $taskVersion bin\64bit. The portable ZIP is available at https://github.com/obsproject/obs-studio/releases/tag/$taskVersion"
}
$taskRuntimeVersion = (Get-Item -LiteralPath $taskObsDll).VersionInfo.ProductVersion
if ($taskRuntimeVersion -notmatch "^$ObsMajor\.") {
    throw "OBS runtime version mismatch: $taskRuntimeVersion, requested $ObsMajor"
}
$taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$taskVsRoot = & $taskVsWhere -latest -products '*' -property installationPath
if (!$taskVsRoot) { throw 'Visual Studio Build Tools was not found' }
$taskMsvc = Get-ChildItem -LiteralPath "$taskVsRoot\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$taskBin = Join-Path $taskMsvc.FullName 'bin\Hostx64\x64'
$taskLibraries = @()
foreach ($taskName in 'obs','obs-frontend-api') {
    $taskDll = Join-Path $taskRuntime "$taskName.dll"
    $taskDump = & "$taskBin\dumpbin.exe" /nologo /exports $taskDll
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed: $taskDll" }
    $taskExports = $taskDump | ForEach-Object {
        if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)') { $Matches[1] }
    }
    if (!$taskExports) { throw "No exports found: $taskDll" }
    $taskDef = Join-Path $taskSdk "lib\$taskName.def"
    $taskLib = Join-Path $taskSdk "lib\$taskName.lib"
    Set-Content -LiteralPath $taskDef -Encoding ascii -Value (@("LIBRARY $taskName.dll",'EXPORTS') + $taskExports)
    & "$taskBin\lib.exe" /nologo /machine:x64 "/def:$taskDef" "/out:$taskLib"
    if ($LASTEXITCODE -ne 0) { throw "Import library generation failed: $taskName" }
    $taskLibraries += @{ name=$taskName; runtime=$taskDll; sha256=(Get-FileHash -LiteralPath $taskDll).Hash; exports=$taskExports.Count }
}
$taskQtCoreVersion = (Get-Item -LiteralPath "$taskQt\bin\Qt6Core.dll").VersionInfo.ProductVersion
$taskManifest = @{
    obsVersion=$taskVersion; runtimeVersion=$taskRuntimeVersion; qtVersion=$taskQtCoreVersion;
    source=@{url=$taskSourceUrl;sha256=$taskSourceHash};
    qt=@{url=$taskQtUrl;expectedArchiveSha256=$taskQtHash;rangeManifest=(Test-Path -LiteralPath "$taskQt\range-extraction-manifest.json")};
    libraries=$taskLibraries;
    include="$taskSdk\include"; library="$taskSdk\lib\obs.lib"; frontendLibrary="$taskSdk\lib\obs-frontend-api.lib"; qtPrefix=$taskQt
}
$taskManifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$taskSdk\sdk-manifest.json" -Encoding utf8
Write-Host "Prepared OBS $taskVersion / Qt $taskQtCoreVersion SDK in $taskSdk"
Write-Host "-DAC_OBS_INCLUDE_DIR=$taskSdk/include -DAC_FRONTEND_INCLUDE_DIR=$taskSdk/include -DAC_OBS_LIBRARY=$taskSdk/lib/obs.lib -DAC_FRONTEND_LIBRARY=$taskSdk/lib/obs-frontend-api.lib -DCMAKE_PREFIX_PATH=$taskQt"
