# Diagnose zu einem Spiel aus Lumoras games.json.
#   ./gamecheck.ps1 -Name "Assetto"
param([Parameter(Mandatory)][string]$Name)

$gj = "$env:APPDATA\lumora\games.json"
if (-not (Test-Path $gj)) { Write-Host "games.json nicht gefunden: $gj"; return }
$g = (Get-Content $gj -Raw | ConvertFrom-Json) | Where-Object { $_.name -like "*$Name*" } | Select-Object -First 1
if (-not $g) { Write-Host "Kein Spiel passend zu '$Name'."; return }

Write-Host "Spiel:     $($g.name)"
Write-Host "Pfad:      $($g.path)"
Write-Host "Spielzeit: $($g.playtime) s    Zuletzt: $($g.lastPlayed)"
$exe = Split-Path $g.path -Leaf
$dir = Split-Path $g.path

# Launcher-Typ
$type = "?"
if ($g.path -match 'steamapps\\common') { $type = "Steam" }
elseif ($g.path -match '\\XboxGames\\' -or $g.path -match '\\WindowsApps\\') { $type = "Xbox/UWP" }
elseif ($g.path -match 'EA Games|Origin') { $type = "EA" }
elseif ($g.path -match 'Rockstar') { $type = "Rockstar" }
elseif ($g.path -match 'GOG') { $type = "GOG" }
elseif ($g.path -match 'Ubisoft') { $type = "Ubisoft" }
elseif ($g.path -match 'Epic') { $type = "Epic" }
Write-Host "Launcher:  $type"

# Exe direkt startbar?
try { $fs = [IO.File]::OpenRead($g.path); $fs.Close(); Write-Host "Exe direkt lesbar/startbar: ja" }
catch { Write-Host "Exe direkt lesbar/startbar: NEIN (UWP/geschuetzt -> ueber AUMID/Protokoll starten)" }

# Steam: AppID + Running-Flag (Hauptbibliothek)
if ($type -eq "Steam") {
  $installdir = ($g.path -split '\\common\\')[-1].Split('\')[0]
  $appid = $null
  Get-ChildItem "C:\Program Files (x86)\Steam\steamapps\appmanifest_*.acf" -ErrorAction SilentlyContinue | ForEach-Object {
    $c = Get-Content $_.FullName -Raw
    if ($c -match ('"installdir"\s+"' + [regex]::Escape($installdir) + '"') -and $c -match '"appid"\s+"(\d+)"') { $appid = $matches[1] }
  }
  Write-Host "Steam AppID: $appid   (Start ueber steam://rungameid/$appid)"
  if ($appid) {
    $r = (cmd /c "reg query `"HKCU\Software\Valve\Steam\Apps\$appid`" /v Running 2>nul") -join "`n"
    $flag = if ($r -match '0x1') { "1 (laeuft)" } elseif ($r -match '0x0') { "0  (ACHTUNG: oft unzuverlaessig)" } else { "?" }
    Write-Host "Steam Running-Flag: $flag"
  }
}

# Xbox: AUMID
if ($type -eq "Xbox/UWP" -and $g.path -match '\\XboxGames\\([^\\]+)\\') {
  $folder = $matches[1]
  $aumid = Get-StartApps | Where-Object { $_.Name -like "$folder*" } | Sort-Object { $_.Name.Length } | Select-Object -First 1 -ExpandProperty AppID
  Write-Host "Xbox AUMID: $aumid   (Start ueber explorer.exe shell:appsFolder\$aumid)"
}

# Laeuft das Spiel gerade? – die 3 Erkennungsmethoden des Monitors
Write-Host "`n--- Laeuft das Spiel gerade? ---"
$byName = (cmd /c "tasklist /FI `"IMAGENAME eq $exe`" /FO CSV /NH") -match [regex]::Escape($exe)
Write-Host "  [Exe-Name, tasklist CSV]: $byName"
$inFolder = (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir\*" } | Measure-Object).Count
Write-Host "  [Prozess im Ordner]:      $inFolder"

# Letzte Spielzeit-Log-Zeilen
Write-Host "`n--- Letzte playtime-log.txt-Zeilen ---"
$log = "$env:APPDATA\lumora\playtime-log.txt"
if (Test-Path $log) { Get-Content $log -Tail 8 } else { Write-Host "  (noch kein Log)" }
