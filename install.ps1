# Installiert oder entfernt den Nova Drift Mod-Loader.
#   .\install.ps1            -> installieren
#   .\install.ps1 -Uninstall -> entfernen
param([switch]$Uninstall, [string]$GameDir)

if (-not $GameDir) {
    $steam = (Get-ItemProperty HKCU:\Software\Valve\Steam).SteamPath
    $libs = @($steam) + (Select-String -Path "$steam/steamapps/libraryfolders.vdf" -Pattern '"path"\s+"(.+)"' |
        ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' })
    $GameDir = $libs | ForEach-Object { Join-Path $_ 'steamapps\common\Nova Drift' } |
        Where-Object { Test-Path (Join-Path $_ 'NovaDrift.exe') } | Select-Object -First 1
}
if (-not $GameDir) { throw 'Nova Drift nicht gefunden. Pfad mit -GameDir angeben.' }

$dll = Join-Path $GameDir 'version.dll'
if ($Uninstall) {
    if (Test-Path $dll) { Remove-Item $dll; Write-Host "Entfernt: $dll" }
    Write-Host 'novadrift_mods.ini bleibt erhalten.'
    return
}

Copy-Item (Join-Path $PSScriptRoot 'build\version.dll') $dll -Force
Write-Host "Installiert: $dll"
$ini = Join-Path $GameDir 'novadrift_mods.ini'
if (-not (Test-Path $ini)) {
    Copy-Item (Join-Path $PSScriptRoot 'novadrift_mods.ini') $ini
    Write-Host "Angelegt: $ini"
} else {
    Write-Host "Vorhanden (nicht ueberschrieben): $ini"
}
