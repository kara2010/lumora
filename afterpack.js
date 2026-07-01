// Wird nach dem Packen (vor dem NSIS-Installer) ausgeführt:
// entfernt nicht benötigte Chromium-Sprachdateien und die Medien-DLL.
const fs = require('fs')
const path = require('path')

exports.default = async function (context) {
  const dir = context.appOutDir

  // Nur Deutsch + Englisch behalten
  const keep = new Set(['en-US.pak', 'de.pak'])
  const localesDir = path.join(dir, 'locales')
  let removedLocales = 0
  try {
    for (const f of fs.readdirSync(localesDir)) {
      if (f.endsWith('.pak') && !keep.has(f)) {
        try { fs.unlinkSync(path.join(localesDir, f)); removedLocales++ } catch {}
      }
    }
  } catch {}

  // Nicht benötigte DLLs entfernen:
  // - ffmpeg.dll: Audio-/Video-Wiedergabe (nutzen wir nicht)
  // WICHTIG: dxcompiler.dll/dxil.dll NICHT entfernen – ohne sie rendert das
  // Fenster grau/leer (von ANGLE in Electron 42 benötigt, getestet).
  const dropDlls = ['ffmpeg.dll']
  const removedDlls = []
  for (const f of dropDlls) {
    try { fs.unlinkSync(path.join(dir, f)); removedDlls.push(f) } catch {}
  }

  console.log(`[afterpack] ${removedLocales} Sprachdateien entfernt, DLLs entfernt: ${removedDlls.join(', ')}`)
}
