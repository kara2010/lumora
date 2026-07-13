// EINMALIGE KRÜCKE für den Sprung 2.2.12/2.2.13 (kaputter Datei-Updater) -> 2.2.14 (Fix).
// ------------------------------------------------------------------------------------
// Hintergrund: Bestandsclients tragen das ALTE fuSafeTarget OHNE '@' in der Whitelist.
// Ihre fuDownload-Vorabschleife wirft, sobald das Manifest EINEN @koromix/koffi-Pfad
// enthält -> Update hängt für ALLE. Dieses Skript erzeugt aus dem vollen 2.2.14-Manifest
// ein reduziertes Rescue-Manifest, das NUR app.asar listet (die einzige geänderte Datei
// UND Träger des Fixes). Damit:
//   - kein @-Pfad  -> der alte, kaputte fuSafeTarget wirft nicht,
//   - kein bin-/koffi-Eintrag -> kein Hash-Drift/404-Risiko (Audit-Finding 15),
//   - koffi/bin bleiben eingefroren -> 2.2.14 MUSS ein reiner JS-Fix ohne neue
//     Native-Fläche sein (Audit-Bedingung 4).
// minElectron wird HART auf 42 (die eingefrorene Fleet-Basis) fixiert, damit das
// fuCheck-Gate keinen Electron-42-Client wegfiltert (Audit-Bedingung 3).
// AB 2.2.15: wieder volle Manifeste zulässig (die 2.2.14-Clients können dann '@').
const fs = require('fs')
const MF = 'C:/Users/kara/Documents/HDR-Launcher/website/updates/app/manifest.json'
const EXPECT_VERSION = '2.2.14'
const OLD_RE = /^[A-Za-z0-9._/-]+$/   // exakt die ALTE Client-Whitelist (ohne @ + ~)

const full = JSON.parse(fs.readFileSync(MF, 'utf8'))
const asar = full.files.find((f) => f.path === 'app.asar')
if (!asar) { console.error('FEHLER: app.asar fehlt im vollen Manifest'); process.exit(1) }

const rescue = {
  version: full.version,
  minElectron: 42,
  notes: full.notes || '',
  notesEn: full.notesEn || '',
  files: [{ path: asar.path, size: asar.size, sha512: asar.sha512 }],
}

// --- Validierung: lieber abbrechen als eine tote Rescue ausliefern ---
const errs = []
if (rescue.version !== EXPECT_VERSION) errs.push('Version != ' + EXPECT_VERSION + ': ' + rescue.version)
if (rescue.minElectron !== 42) errs.push('minElectron != 42')
if (!rescue.files.length) errs.push('keine Dateien im Rescue-Manifest')
for (const f of rescue.files) {
  if (typeof f.path !== 'string' || !OLD_RE.test(f.path) || f.path.includes('..')) errs.push('Pfad verletzt ALTE Client-Whitelist: ' + f.path)
  if (f.path.includes('@')) errs.push('@-Pfad im Rescue-Manifest: ' + f.path)
  if (!f.sha512 || !f.size) errs.push('Eintrag unvollständig: ' + f.path)
}
let out
try { out = JSON.stringify(rescue, null, 2); JSON.parse(out) } catch (e) { errs.push('JSON ungültig: ' + e.message) }
if (out && /^﻿/.test(out)) errs.push('BOM im Output')

if (errs.length) {
  console.error('RESCUE-VALIDIERUNG FEHLGESCHLAGEN – nichts geschrieben:')
  errs.forEach((e) => console.error('  ✗ ' + e))
  process.exit(1)
}

fs.writeFileSync(MF, out, 'utf8')  // utf8 (Node) = KEIN BOM
console.log('OK: Rescue-Manifest geschrieben.')
console.log('  version    :', rescue.version)
console.log('  minElectron:', rescue.minElectron)
console.log('  files      :', rescue.files.map((f) => f.path).join(', '))
console.log('  app.asar   :', (asar.size / 1048576).toFixed(2), 'MB, sha512', asar.sha512.slice(0, 24) + '…')
console.log('  notes[0]   :', (rescue.notes || '').split('\n')[0])
console.log('  notesEn[0] :', (rescue.notesEn || '').split('\n')[0])
