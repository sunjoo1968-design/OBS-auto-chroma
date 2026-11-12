param(
    [ValidateSet(32,33)][int[]]$ObsMajor = @(32,33),
    [ValidateSet('Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskDist = Join-Path $taskRoot 'dist'
New-Item -ItemType Directory -Force -Path $taskDist | Out-Null
foreach ($taskMajor in $ObsMajor) {
    $taskBuild = Join-Path $taskRoot "build\obs$taskMajor"
    $taskStage = Join-Path $taskDist "obs$taskMajor"
    & cmake --install $taskBuild --config $Configuration --prefix $taskStage
    if ($LASTEXITCODE -ne 0) { throw "OBS $taskMajor install failed" }
    $taskSource = Join-Path $taskStage 'source'
    New-Item -ItemType Directory -Force -Path $taskSource | Out-Null
    foreach ($taskDirectory in @('native','scripts','docs')) {
        $taskSourceDirectory = Join-Path $taskSource $taskDirectory
        New-Item -ItemType Directory -Force -Path $taskSourceDirectory | Out-Null
        Copy-Item -Path (Join-Path (Join-Path $taskRoot $taskDirectory) '*') -Destination $taskSourceDirectory -Recurse -Force
    }
    foreach ($taskFile in @('CMakeLists.txt','README.md','LICENSE','.gitignore')) {
        Copy-Item -LiteralPath (Join-Path $taskRoot $taskFile) -Destination $taskSource -Force
    }
    $taskEntries = Get-ChildItem -LiteralPath $taskStage -Recurse -File |
        Where-Object Name -ne 'SHA256SUMS.txt' | Sort-Object FullName | ForEach-Object {
            $taskRelative = $_.FullName.Substring($taskStage.Length + 1).Replace('\','/')
            "{0}  {1}" -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $taskRelative
        }
    $taskEntries | Set-Content -LiteralPath (Join-Path $taskStage 'SHA256SUMS.txt') -Encoding ascii
    $taskZip = Join-Path $taskDist "OBS-auto-chroma-v1.4-obs$taskMajor-win64.zip"
    # Use standard ZIP '/' separators, including when packaged from Windows PowerShell 5.1.
    $taskZipStream = [System.IO.File]::Open($taskZip, [System.IO.FileMode]::Create)
    $taskArchive = [System.IO.Compression.ZipArchive]::new($taskZipStream, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($taskItem in (Get-ChildItem -LiteralPath $taskStage -Recurse -File | Sort-Object FullName)) {
            $taskEntryName = $taskItem.FullName.Substring($taskStage.Length + 1).Replace('\','/')
            $taskEntry = $taskArchive.CreateEntry($taskEntryName, [System.IO.Compression.CompressionLevel]::Optimal)
            $taskInput = [System.IO.File]::OpenRead($taskItem.FullName)
            $taskOutput = $taskEntry.Open()
            try { $taskInput.CopyTo($taskOutput) } finally { $taskOutput.Dispose(); $taskInput.Dispose() }
        }
    } finally { $taskArchive.Dispose(); $taskZipStream.Dispose() }
    Write-Output "Packaged SunjooAn v1.4 OBS $taskMajor : $taskZip"
    & (Join-Path $PSScriptRoot 'verify-package.ps1') -ZipPath $taskZip
}
Get-ChildItem -LiteralPath $taskDist -Filter '*.zip' -File | Get-FileHash -Algorithm SHA256 |
    ForEach-Object { "{0}  {1}" -f $_.Hash.ToLowerInvariant(), (Split-Path -Leaf $_.Path) } |
    Set-Content -LiteralPath (Join-Path $taskDist 'SHA256SUMS.txt') -Encoding ascii
