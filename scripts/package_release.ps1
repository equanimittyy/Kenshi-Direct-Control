# Builds the release zip: one mod folder that players extract into Kenshi\mods\.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$modName = 'WASDCombatPlugin'
$dll = Join-Path $repo 'dist\bin\Release\WASDCombatPlugin.dll'
$out = Join-Path $repo 'dist'

if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
    Write-Host "DLL not found: $dll. Build the plugin first (docs\plugin_build_setup.md, section 4)."
    exit 1
}

$versionLine = Get-Content -LiteralPath (Join-Path $repo 'mod\mod.info') | Where-Object { $_ -match '^\s*version\s*=' } | Select-Object -First 1
if (-not $versionLine) {
    Write-Host 'mod\mod.info has no version= line'
    exit 1
}
$version = ($versionLine -split '=', 2)[1].Trim()
Write-Host "Version  $version"

$text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dll))
# The plugin shares C++ types with Kenshi, so a DLL from a newer toolset crashes the game on load.
if ($text.IndexOf('msvcr100.dll', [StringComparison]::OrdinalIgnoreCase) -lt 0) {
    Write-Host "$dll was not built with the Visual C++ 2010 toolset"
    exit 1
}
$item = Get-Item -LiteralPath $dll
Write-Host "DLL      $dll"
Write-Host ('Built    {0:yyyy-MM-dd HH:mm}' -f $item.LastWriteTime)
Write-Host ('Size     {0:N0} KB' -f ($item.Length / 1KB))
Write-Host "SHA-256  $((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLower())"

$stageRoot = Join-Path $out 'stage'
$stage = Join-Path $stageRoot $modName
if (Test-Path -LiteralPath $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
# The plugin writes a default INI when none exists, so shipping one would only reset the player's settings on every update.
Get-ChildItem -Path (Join-Path $repo 'mod\*') -Exclude 'WASDCombatPlugin.ini' | Copy-Item -Destination $stage -Recurse
Copy-Item -LiteralPath $dll -Destination $stage

$zip = Join-Path $out "$modName-$version.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
# ZipFile writes forward-slash entry names; Compress-Archive in Windows PowerShell 5.1 writes backslashes, which some unzip tools reject.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
Write-Host ('Wrote {0} ({1:N0} KB)' -f $zip, ((Get-Item -LiteralPath $zip).Length / 1KB))
