param([string[]]$ZipPath)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (!$ZipPath) {
    $taskRoot = Split-Path -Parent $PSScriptRoot
    $ZipPath = @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'dist') -Filter '*.zip' -File | Select-Object -ExpandProperty FullName)
}
if (!$ZipPath) { throw 'No package ZIP found' }
foreach ($taskPath in $ZipPath) {
    $taskArchive = [System.IO.Compression.ZipFile]::OpenRead([System.IO.Path]::GetFullPath($taskPath))
    try {
        $taskEntries = @{}
        foreach ($taskEntry in $taskArchive.Entries) {
            $taskName = $taskEntry.FullName.Replace('\','/')
            if ($taskName.EndsWith('/')) { continue }
            if ($taskName.StartsWith('/') -or $taskName -match '(^|/)\.\.(/|$)' -or $taskEntries.ContainsKey($taskName)) { throw "Unsafe/duplicate ZIP entry: $taskName" }
            $taskEntries[$taskName] = $taskEntry
        }
        if (!$taskEntries.ContainsKey('SHA256SUMS.txt')) { throw 'Package checksum manifest is missing' }
        $taskReader = [System.IO.StreamReader]::new($taskEntries['SHA256SUMS.txt'].Open())
        try { $taskManifest = $taskReader.ReadToEnd() } finally { $taskReader.Dispose() }
        $taskCount = 0
        foreach ($taskLine in ($taskManifest -split "`n")) {
            if (!$taskLine.Trim()) { continue }
            if ($taskLine.Trim() -notmatch '^([0-9a-f]{64})  (.+)$') { throw 'Malformed checksum line' }
            $taskExpected = $Matches[1]
            $taskName = $Matches[2]
            if (!$taskEntries.ContainsKey($taskName)) { throw "Missing ZIP entry: $taskName" }
            $taskStream = $taskEntries[$taskName].Open()
            try { $taskActual = (Get-FileHash -InputStream $taskStream -Algorithm SHA256).Hash.ToLowerInvariant() } finally { $taskStream.Dispose() }
            if ($taskActual -ne $taskExpected) { throw "Checksum mismatch: $taskName" }
            $taskCount++
        }
        if ($taskCount + 1 -ne $taskEntries.Count) { throw 'ZIP contains unlisted files' }
        Write-Output "PASS $(Split-Path -Leaf $taskPath): $taskCount file hashes verified inside ZIP"
    } finally { $taskArchive.Dispose() }
}
