# Nova Drift Mod-Loader

Inoffizieller Mod-Loader für [Nova Drift](https://store.steampowered.com/app/858210/Nova_Drift/) (Steam, Version 1.2.38.4).

- Balancing-Werte von Schiffen, Waffen, Schilden und Mods per INI-Datei ändern
- Neue Ausrüstung ins Spiel bringen, Beispiel-Profil **Skorpion**:
  - **FORM: SKORPION**: zusätzlicher 15. Body mit eigenem Modell, Icon und Werten
  - **WAFFE: STACHELSCHWANZ**: Stachel-Schüsse nach vorne und ein animierter Schwanzhieb gegen Verfolger
- Score-Uploads in die Steam-Bestenlisten werden gesperrt, solange der Loader installiert ist

![Skorpion](docs/skorpion_preview.png)

## Funktionsweise

Nova Drift ist mit GameMaker (YYC) kompiliert. Der Loader ist eine Proxy-`version.dll`, die das Spiel beim Start lädt. Sie hakt sich in die kompilierten GML-Funktionen ein (`initializeVariables`, `assignBaseStats`, `getUpgrade*` …) und sucht alle Adressen zur Laufzeit über Funktionsnamen und Bytemuster. Das Spiel selbst wird nicht verändert.

## Bauen

Benötigt [Zig](https://ziglang.org/) (als C-Compiler):

```bash
zig cc -O2 -shared -target x86_64-windows-gnu src/novadrift_mods.c src/version.def -o build/version.dll
```

## Installation

```powershell
.\install.ps1              # version.dll und novadrift_mods.ini in den Spielordner kopieren
.\profile.ps1 skorpion     # optional: Skorpion-Profil einschalten
```

Wieder entfernen:

```powershell
.\profile.ps1 skorpion -Disable
.\install.ps1 -Uninstall
```

## Benutzung

- `novadrift_mods.ini` im Spielordner bearbeiten, die Werte gelten ab dem nächsten Run.
- `[vars]`: Balancing-Variablen, siehe [variablen_liste.txt](variablen_liste.txt).
- `[weapon.Name]`, `[body.Name]`, `[shield.Name]`: Grundwerte einer Ausrüstung.
- `novadrift_mods.log` im Spielordner zeigt geänderte Werte und die Namen der gewählten Ausrüstung.

Profile liegen in `profiles/<name>/`: Werte und Texte (`<name>.ini`), Grafiken (`skin/`) und der Custom-Skin-Eintrag (`profile.json`). Die Grafiken erzeugen die Python-Skripte in `tools/` (benötigen `numpy`).

## Hinweise

- Die neuen Ausrüstungsteile funktionieren nur mit Spielversion 1.2.38.4. Nach einem Update meldet das Log, was nicht mehr passt.
- Fortgesetzte Runs kennen die neue Ausrüstung nicht mehr und laufen mit Standardform bzw. Blaster weiter.
- Nicht mit dem Entwickler von Nova Drift verbunden. Benutzung auf eigene Gefahr; vorher Spielstände sichern.
