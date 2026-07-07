// Live-Monitor: protokolliert ALLE Aenderungen an den RTSS-OSD-Slots in eine
// Log-Datei – inkl. Entry-Groesse und Buffer-Kopf (Embedded Objects). Damit
// sehen wir EXAKT, was der Overlay-Editor schreibt, wenn ein Layout aktiv ist.
'use strict'
const fs = require('fs')
const path = require('path')
const koffi = require(path.join(__dirname, '..', 'node_modules', 'koffi'))
const k32 = koffi.load('kernel32.dll')
const open = k32.func('void* OpenFileMappingA(uint32 a, int i, str n)')
const map = k32.func('void* MapViewOfFile(void* h, uint32 a, uint32 hi, uint32 lo, size_t b)')
const unmap = k32.func('int UnmapViewOfFile(void* p)')
const close = k32.func('int CloseHandle(void* h)')
const HDR = koffi.struct({ sig: 'uint32', ver: 'uint32', appES: 'uint32', appOff: 'uint32', appN: 'uint32', osdES: 'uint32', osdOff: 'uint32', osdN: 'uint32', frame: 'uint32' })
const LOG = path.join(__dirname, 'rtss-monitor.log')
fs.writeFileSync(LOG, '=== RTSS-Slot-Monitor gestartet ' + new Date().toISOString() + ' ===\n')
const log = (s) => { fs.appendFileSync(LOG, s + '\n'); console.log(s) }

// Globales Laufzeit-Flag (Bit0 = OSD_VISIBLE, schaltet der AB-Hotkey)
let getFlags = null
try {
  const hooks = koffi.load('C:/Program Files (x86)/RivaTuner Statistics Server/RTSSHooks64.dll')
  getFlags = hooks.func('uint32 __cdecl GetFlags()')
} catch {}

const EX2 = 256 + 256 + 4096 + 262144   // szOSDEx2 @266752 (v2.20+)
const lastState = new Map()
let headerShown = false
let lastFlags = -1
function tick() {
  const h = open(0x0004, 0, 'RTSSSharedMemoryV2'); if (!h) return
  const base = map(h, 0x0004, 0, 0, 0); if (!base) { close(h); return }
  try {
    const hdr = koffi.decode(base, HDR)
    if (!headerShown) {
      headerShown = true
      log(`Header: ver=0x${hdr.ver.toString(16)} osdEntrySize=${hdr.osdES} osdArrSize=${hdr.osdN} osdOff=${hdr.osdOff}`)
    }
    if (getFlags) {
      const fl = getFlags() >>> 0
      if (fl !== lastFlags) {
        log(`[${new Date().toISOString().slice(11, 19)}] GLOBAL Flags=0x${fl.toString(16)} (OSD_VISIBLE=${fl & 1})`)
        lastFlags = fl
      }
    }
    for (let i = 0; i < Math.min(hdr.osdN, 8); i++) {
      const so = hdr.osdOff + i * hdr.osdES
      const raw = Buffer.from(koffi.decode(base, so, koffi.array('uint8', hdr.osdES)))
      const z = (b, off, max) => { const e = b.indexOf(0, off); return b.toString('latin1', off, e < 0 || e > off + max ? off + max : e) }
      const owner = z(raw, 256, 40)
      const osd = z(raw, 0, 250)
      const ex = hdr.osdES >= 4608 ? z(raw, 512, 300) : ''
      const ex2 = hdr.osdES >= EX2 + 32768 ? z(raw, EX2, 300) : ''
      const stateKey = owner + '|' + osd + '|' + ex + '|' + ex2.length + '|' + ex2.slice(0, 60)
      if (lastState.get(i) !== stateKey) {
        lastState.set(i, stateKey)
        log(`[${new Date().toISOString().slice(11, 19)}] Slot ${i} owner="${owner}"`)
        if (osd) log(`    szOSD:    ${JSON.stringify(osd)}`)
        if (ex) log(`    szOSDEx:  ${JSON.stringify(ex)}`)
        if (ex2) log(`    szOSDEx2: ${JSON.stringify(ex2.slice(0, 200))}${ex2.length > 200 ? '…(' + ex2.length + ')' : ''}`)
        if (!osd && !ex && !ex2 && !owner) log('    (leer)')
        if (owner && !osd && !ex && !ex2) log('    (Text leer)')
      }
    }
  } catch (e) { log('ERR ' + (e && e.message)) }
  finally { try { unmap(base) } catch {}; try { close(h) } catch {} }
}
setInterval(tick, 500)
tick()
console.log('Monitor laeuft (Log: ' + LOG + ') – Strg+C beendet.')
setTimeout(() => process.exit(0), 15 * 60 * 1000)
