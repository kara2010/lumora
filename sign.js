// Custom Windows-Sign-Hook fuer electron-builder — Certum Open Source Code Signing
// in the Cloud (via SimplySign Desktop). electron-builder ruft diese Funktion pro
// zu signierender Datei auf (App-EXE, Installer, Uninstaller, gebuendelte Binaries);
// wir signieren mit dem lokal extrahierten signtool + Certum-Zeitstempel.
//
// VORAUSSETZUNG: SimplySign Desktop muss auf dem Build-PC ANGEMELDET sein — dann
// signiert der Cloud-Schluessel OHNE Nachfrage. Ist es nicht angemeldet, schlaegt
// signtool fehl und der Build bricht mit klarer Meldung ab (kein stilles Skippen).
//
// Der Thumbprint identifiziert GENAU unser Zertifikat im Windows-Speicher. Bei einer
// Zertifikatsverlaengerung (jaehrlich) aendert er sich -> hier aktualisieren
// (neuen per `Get-ChildItem Cert:\CurrentUser\My` ermitteln). Gueltig bis 2027-07-17.
const { execFileSync } = require('child_process')
const path = require('path')
const fs = require('fs')

const SIGNTOOL = path.join(__dirname, '_testlab', 'tools', 'signtool', 'signtool.exe')
const THUMBPRINT = 'EC6B6B6FDEBDB88941519F15E9570994CE3E14E3'
const TIMESTAMP = 'http://time.certum.pl'   // Certum RFC-3161-Zeitstempel (haelt Signatur nach Zert-Ablauf gueltig)

exports.default = async function sign(configuration) {
  const file = configuration && configuration.path
  if (!file) return
  if (!fs.existsSync(SIGNTOOL)) {
    throw new Error('signtool nicht gefunden: ' + SIGNTOOL + ' — _testlab/tools/signtool/ fehlt (via SDK-BuildTools-NuGet holen).')
  }
  console.log('  • Certum-Signatur: ' + path.basename(file))
  // /sha1 <thumbprint>: Zertifikat per Fingerabdruck aus dem Store waehlen (der
  // private Schluessel liegt in der SimplySign-Cloud, angebunden ueber den CSP).
  execFileSync(SIGNTOOL, [
    'sign',
    '/sha1', THUMBPRINT,
    '/fd', 'sha256',
    '/tr', TIMESTAMP,
    '/td', 'sha256',
    file,
  ], { stdio: 'inherit' })
}
