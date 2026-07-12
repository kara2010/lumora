# Crash-Diagnose fuer den harten PC-Absturz beim Streaming (AMD-System):
# NACH dem naechsten Absturz + Neustart auf dem betroffenen Rechner ausfuehren.
# Liest die relevanten Windows-Ereignisse seit 24h und ordnet sie ein - damit
# unterscheiden wir FAKTENBASIERT: GPU-Treiber vs. Netzwerktreiber vs. Hardware/
# Strom (Mini-PC-Netzteil!) vs. echter Bluescreen mit Schuldigem.
# HINWEIS: Diese Datei bewusst ASCII-only halten (PowerShell 5.1 liest .ps1 als ANSI).
$ErrorActionPreference = 'SilentlyContinue'
$since = (Get-Date).AddHours(-24)
Write-Host "== Lumora Crash-Check (Ereignisse seit $since) =="

# 1) Kernel-Power 41: Rechner ging aus, OHNE sauber herunterzufahren.
#    BugcheckCode 0 = KEIN Bluescreen -> deutet auf Strom/Hardware/Hard-Freeze.
#    BugcheckCode != 0 = es GAB einen Bluescheck -> Code sagt, welcher Treiber.
Write-Host "`n--- Kernel-Power 41 (harter Neustart) ---"
Get-WinEvent -FilterHashtable @{LogName='System'; Id=41; StartTime=$since} | ForEach-Object {
  $x = [xml]$_.ToXml()
  $bc = ($x.Event.EventData.Data | Where-Object { $_.Name -eq 'BugcheckCode' }).'#text'
  Write-Host ("{0}  BugcheckCode={1} {2}" -f $_.TimeCreated, $bc, $(if ($bc -eq '0') { '(kein Bluescreen -> Strom/Hardware/Freeze)' } else { '(Bluescreen! Code unten pruefen)' }))
}

# 2) BugCheck 1001: der Bluescreen selbst (falls es einen gab) inkl. Parameter/Dump-Pfad.
Write-Host "`n--- BugCheck 1001 (Bluescreen-Details) ---"
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-WER-SystemErrorReporting'; StartTime=$since} | ForEach-Object {
  Write-Host ("{0}  {1}" -f $_.TimeCreated, $_.Message)
}

# 3) WHEA 17/18/19: Hardware-Fehler (CPU/Cache/Bus) - klarer Hardware-Befund.
Write-Host "`n--- WHEA (Hardware-Fehler) ---"
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-WHEA-Logger'; StartTime=$since} | Select-Object -First 10 | ForEach-Object {
  Write-Host ("{0}  Id={1}  {2}" -f $_.TimeCreated, $_.Id, ($_.Message -split "`n")[0])
}

# 4) Display-Treiber-Resets (Event 4101 = "Treiber reagiert nicht mehr" / amdkmdag):
#    Haeufigkeit VOR dem Absturz zeigt, ob der AMD-Grafiktreiber unter Druck stand.
Write-Host "`n--- Display-/AMD-Treiber (4101, amdkmdag, amdwddmg) ---"
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=$since} | Where-Object {
  $_.Id -eq 4101 -or $_.ProviderName -match 'amdkmdag|amdwddmg|Display'
} | Select-Object -First 15 | ForEach-Object {
  Write-Host ("{0}  [{1}] Id={2}  {3}" -f $_.TimeCreated, $_.ProviderName, $_.Id, ($_.Message -split "`n")[0])
}

# 5) Netzwerktreiber-Resets: mehr Zuschauer = mehr UDP-Sendelast. Beruechtigte
#    Kandidaten (v. a. Realtek RTL8125 in Mini-PCs) resetten/haengen unter Last.
Write-Host "`n--- Netzwerk-Adapter/NDIS-Ereignisse ---"
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=$since} | Where-Object {
  $_.ProviderName -match 'rt640|rtcx|Realtek|e1d|NDIS|Netwtw|mlx|tcpip' -and $_.LevelDisplayName -match 'Fehler|Warnung|Error|Warning'
} | Select-Object -First 15 | ForEach-Object {
  Write-Host ("{0}  [{1}] Id={2}  {3}" -f $_.TimeCreated, $_.ProviderName, $_.Id, ($_.Message -split "`n")[0])
}

# 6) Verbauter Netzwerkadapter + Treiberversion (fuer die Einordnung von Punkt 5).
Write-Host "`n--- Netzwerkadapter im System ---"
Get-NetAdapter | Where-Object Status -eq 'Up' | ForEach-Object {
  Write-Host ("{0}  |  {1}  |  Treiber {2} ({3})" -f $_.Name, $_.InterfaceDescription, $_.DriverVersion, $_.DriverDate)
}

Write-Host "`nFertig. Bitte die KOMPLETTE Ausgabe weitergeben."
Write-Host "Zusaetzlich hilfreich: \\synology\Fileshare\lumora-stream.log (letzte Zeilen VOR dem Absturz - dort steht jetzt auch jeder Zuschauer-Wechsel)."
