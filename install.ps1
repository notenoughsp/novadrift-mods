# Installs or removes the Nova Drift mod loader.
#   .\install.ps1              install
#   .\install.ps1 -Uninstall   remove
param([switch]$Uninstall, [string]$GameDir)

if (-not $GameDir) {
    $steam = (Get-ItemProperty HKCU:\Software\Valve\Steam).SteamPath
    $libs = @($steam) + (Select-String -Path "$steam/steamapps/libraryfolders.vdf" -Pattern '"path"\s+"(.+)"' |
        ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' })
    $GameDir = $libs | ForEach-Object { Join-Path $_ 'steamapps\common\Nova Drift' } |
        Where-Object { Test-Path (Join-Path $_ 'NovaDrift.exe') } | Select-Object -First 1
}
if (-not $GameDir) { throw 'Nova Drift not found. Pass the folder with -GameDir.' }

$dll = Join-Path $GameDir 'version.dll'
if ($Uninstall) {
    if (Test-Path $dll) { Remove-Item $dll; Write-Host "Removed: $dll" }
    Write-Host 'novadrift_mods.ini was kept.'
    return
}

# Prebuilt DLL from a release sits next to this script, a local build in build\
$src = @((Join-Path $PSScriptRoot 'version.dll'), (Join-Path $PSScriptRoot 'build\version.dll')) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $src) { throw 'version.dll not found. Download a release or build it (see README).' }
Copy-Item $src $dll -Force
Write-Host "Installed: $dll"
$ini = Join-Path $GameDir 'novadrift_mods.ini'
if (-not (Test-Path $ini)) {
    Copy-Item (Join-Path $PSScriptRoot 'novadrift_mods.ini') $ini
    Write-Host "Created: $ini"
} else {
    Write-Host "Kept existing: $ini"
}
