# Enables or disables a mod profile.
#   .\profile.ps1 scorpion            enable
#   .\profile.ps1 scorpion -Disable   disable
param([Parameter(Mandatory)][string]$Name, [switch]$Disable, [string]$GameDir)

$ErrorActionPreference = 'Stop'
if (-not $GameDir) {
    $steam = (Get-ItemProperty HKCU:\Software\Valve\Steam).SteamPath
    $libs = @($steam) + (Select-String -Path "$steam/steamapps/libraryfolders.vdf" -Pattern '"path"\s+"(.+)"' |
        ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' })
    $GameDir = $libs | ForEach-Object { Join-Path $_ 'steamapps\common\Nova Drift' } |
        Where-Object { Test-Path (Join-Path $_ 'NovaDrift.exe') } | Select-Object -First 1
}
if (-not $GameDir) { throw 'Nova Drift not found. Pass the folder with -GameDir.' }
if (Get-Process NovaDrift -ErrorAction SilentlyContinue) { throw 'Please close Nova Drift first.' }

$src = Join-Path $PSScriptRoot "profiles\$Name"
$skinLine = (Get-Content (Join-Path $src 'profile.json') -Raw -Encoding UTF8 | ConvertFrom-Json).skin
$skinName = ($skinLine -split ',')[0]
$skinDir = Join-Path $GameDir 'custom_skins'
$skinCsv = Join-Path $skinDir 'custom_skins.csv'
$iniPath = Join-Path $GameDir 'novadrift_mods.ini'
$utf8 = New-Object System.Text.UTF8Encoding($false)

# Set or remove profile=<name> in [settings]
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

# Remove the skin row and optionally add it again
function Set-SkinLine($line) {
    $kept = @(Get-Content $skinCsv | Where-Object { $_.Trim() -and -not $_.StartsWith("$skinName,") })
    if ($line) { $kept += $line }
    [IO.File]::WriteAllText($skinCsv, ($kept -join "`r`n"), $utf8)
}

if ($Disable) {
    Set-SkinLine $null
    Set-ProfileLine $null
    Write-Host "Profile '$Name' disabled."
    return
}

$profDir = Join-Path $GameDir 'novadrift_profiles'
New-Item -ItemType Directory -Force $profDir | Out-Null
Copy-Item (Join-Path $src "$Name.ini") $profDir -Force
Copy-Item (Join-Path $src 'skin\*.png') $skinDir -Exclude 'preview.png' -Force
Set-SkinLine $skinLine
Set-ProfileLine $Name
Write-Host "Profile '$Name' enabled."
