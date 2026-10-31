# Schaltet ein Mod-Profil ein oder aus.
#   .\profile.ps1 skorpion            einschalten
#   .\profile.ps1 skorpion -Disable   ausschalten
param([Parameter(Mandatory)][string]$Name, [switch]$Disable, [string]$GameDir)

$ErrorActionPreference = 'Stop'
if (-not $GameDir) {
    $steam = (Get-ItemProperty HKCU:\Software\Valve\Steam).SteamPath
    $libs = @($steam) + (Select-String -Path "$steam/steamapps/libraryfolders.vdf" -Pattern '"path"\s+"(.+)"' |
        ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' })
    $GameDir = $libs | ForEach-Object { Join-Path $_ 'steamapps\common\Nova Drift' } |
        Where-Object { Test-Path (Join-Path $_ 'NovaDrift.exe') } | Select-Object -First 1
}
if (-not $GameDir) { throw 'Nova Drift nicht gefunden. Pfad mit -GameDir angeben.' }
if (Get-Process NovaDrift -ErrorAction SilentlyContinue) { throw 'Bitte Nova Drift zuerst beenden.' }

$src = Join-Path $PSScriptRoot "profiles\$Name"
$skinLine = (Get-Content (Join-Path $src 'profile.json') -Raw -Encoding UTF8 | ConvertFrom-Json).skin
$skinName = ($skinLine -split ',')[0]
$skinDir = Join-Path $GameDir 'custom_skins'
$skinCsv = Join-Path $skinDir 'custom_skins.csv'
$iniPath = Join-Path $GameDir 'novadrift_mods.ini'
$utf8 = New-Object System.Text.UTF8Encoding($false)

# profile=<name> in [settings] setzen oder entfernen
function Set-ProfileLine($value) {
    $lines = [Collections.Generic.List[string]](Get-Content $iniPath)
    $lines.RemoveAll([Predicate[string]] { param($l) $l -match '^\s*profile\s*=' }) | Out-Null
    if ($value) {
        $i = $lines.FindIndex([Predicate[string]] { param($l) $l -match '^\s*\[settings\]' })
        if ($i -lt 0) { $lines.Insert(0, '[settings]'); $i = 0 }
        $lines.Insert($i + 1, "profile=$value")
    }
    Set-Content $iniPath $lines -Encoding Ascii
}

# Skin-Zeile entfernen und optional neu eintragen
function Set-SkinLine($line) {
    $kept = @(Get-Content $skinCsv | Where-Object { $_.Trim() -and -not $_.StartsWith("$skinName,") })
    if ($line) { $kept += $line }
    [IO.File]::WriteAllText($skinCsv, ($kept -join "`r`n"), $utf8)
}

if ($Disable) {
    Set-SkinLine $null
    Set-ProfileLine $null
    Write-Host "Profil '$Name' ausgeschaltet."
    return
}

$profDir = Join-Path $GameDir 'novadrift_profiles'
New-Item -ItemType Directory -Force $profDir | Out-Null
Copy-Item (Join-Path $src "$Name.ini") $profDir -Force
Copy-Item (Join-Path $src 'skin\*.png') $skinDir -Force
Set-SkinLine $skinLine
Set-ProfileLine $Name
Write-Host "Profil '$Name' eingeschaltet."
