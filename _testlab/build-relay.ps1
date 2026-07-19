# Lumora Media-Relay bauen: mediamtx (bluenviron, MIT) selbst kompilieren + branden
# (Lumora-Logo, VersionInfo "Lumora Media-Relay") + mit Certum signieren. Reproduzierbar.
# VORAUSSETZUNG: Go installiert (C:\Program Files\Go) + SimplySign Desktop ANGEMELDET.
# Bei mediamtx-Update: $ver hochsetzen, Skript erneut laufen lassen.
param([string]$ver = 'v1.19.2')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent                 # HDR-Launcher
$src  = Join-Path $PSScriptRoot 'mediamtx-src'
$env:PATH = $env:PATH + ';C:\Program Files\Go\bin;' + $env:USERPROFILE + '\go\bin'

# 1. Quellcode auf die gepinnte Version holen
if (-not (Test-Path $src)) {
  git clone --depth 1 --branch $ver https://github.com/bluenviron/mediamtx.git $src
}
Set-Location $src

# 2. goversioninfo (erzeugt die .syso-Ressource fuer Icon + VersionInfo)
go install github.com/josephspurrier/goversioninfo/cmd/goversioninfo@latest

# 3. Branding-Ressource: Lumora-Logo + Metadaten
Copy-Item (Join-Path $root 'icon.ico') 'app.ico' -Force
@'
{
  "FixedFileInfo": { "FileVersion": {"Major":2,"Minor":2,"Patch":21,"Build":0}, "ProductVersion": {"Major":2,"Minor":2,"Patch":21,"Build":0}, "FileFlagsMask":"3f","FileFlags":"00","FileOS":"040004","FileType":"01","FileSubType":"00" },
  "StringFileInfo": { "CompanyName":"Karsten Radermacher", "FileDescription":"Lumora Media-Relay", "FileVersion":"2.2.21.0", "InternalName":"lumora-media-relay", "LegalCopyright":"(c) Karsten Radermacher; basiert auf mediamtx (bluenviron, MIT)", "OriginalFilename":"lumora-media-relay.exe", "ProductName":"Lumora Media-Relay", "ProductVersion":"2.2.21.0" },
  "VarFileInfo": { "Translation": {"LangID":"0409","CharsetID":"04B0"} },
  "IconPath": "app.ico"
}
'@ | Out-File -Encoding ascii 'versioninfo.json'
goversioninfo -o resource.syso versioninfo.json

# 4. go:embed-Dateien erzeugen (im flachen Klon nicht enthalten; rpicamera ist Linux-only)
$ver | Out-File -NoNewline -Encoding ascii 'internal\core\VERSION'
Push-Location 'internal\servers\hls'; go run ./hlsjsdownloader; Pop-Location

# 5. Build (statisch, ohne Debug-Info)
$env:CGO_ENABLED = '0'
go build -ldflags '-s -w' -o 'lumora-media-relay.exe' .

# 6. Signieren (Certum via SimplySign)
$signtool = Join-Path $PSScriptRoot 'tools\signtool\signtool.exe'
& $signtool sign /sha1 EC6B6B6FDEBDB88941519F15E9570994CE3E14E3 /fd sha256 /tr http://time.certum.pl /td sha256 'lumora-media-relay.exe'
$sig = Get-AuthenticodeSignature 'lumora-media-relay.exe'
if ($sig.Status -ne 'Valid') { throw "Signatur ungueltig: $($sig.Status)" }

# 7. nach bin/
Copy-Item 'lumora-media-relay.exe' (Join-Path $root 'bin\lumora-media-relay.exe') -Force
Write-Host "Lumora Media-Relay ($ver) gebaut, signiert ($($sig.Status)) und in bin/ abgelegt."
