// Test: EnableFill via SetProfileProperty setzen (auch wenn GetProfileProperty es
// nicht kennt - Set koennte trotzdem funktionieren) auf das SPIELSPEZIFISCHE Profil
// (nicht global!), dann UpdateProfiles() aufrufen - das ist vermutlich der Live-
// Anwenden-Mechanismus, den RTSS Setup's "Uebernehmen"-Knopf intern nutzt.
// SICHERHEIT: Profiles/Global wurde vorher als echte Datei-Kopie gesichert.
// Scoped auf das aktive Spiel (nicht LoadProfile('')) - betrifft nur DIESES Spiel.
'use strict'
const path = require('path')
const koffi = require(path.join(__dirname, '..', 'node_modules', 'koffi'))
const k32 = koffi.load('kernel32.dll')
const open = k32.func('void* OpenFileMappingA(uint32 a, int i, str n)')
const map = k32.func('void* MapViewOfFile(void* h, uint32 a, uint32 hi, uint32 lo, size_t b)')
const unmap = k32.func('int UnmapViewOfFile(void* p)')
const close = k32.func('int CloseHandle(void* h)')
const HDR = koffi.struct({ dwSignature: 'uint32', dwVersion: 'uint32', dwAppEntrySize: 'uint32', dwAppArrOffset: 'uint32', dwAppArrSize: 'uint32', dwOSDEntrySize: 'uint32', dwOSDArrOffset: 'uint32', dwOSDArrSize: 'uint32', dwOSDFrame: 'uint32' })
const APP = koffi.struct({ dwProcessID: 'uint32', szName: koffi.array('uint8', 260), dwFlags: 'uint32', dwTime0: 'uint32', dwTime1: 'uint32', dwFrames: 'uint32', dwFrameTime: 'uint32' })

// Aktives Spiel ermitteln (wie main.js rtssBestApp)
const h0 = open(0x0004, 0, 'RTSSSharedMemoryV2')
const base0 = map(h0, 0x0004, 0, 0, 0)
const hdr0 = koffi.decode(base0, HDR)
let gameName = null
for (let i = 0; i < Math.min(hdr0.dwAppArrSize, 4096); i++) {
  const off = hdr0.dwAppArrOffset + i * hdr0.dwAppEntrySize
  const e = koffi.decode(base0, off, APP)
  if (!e.dwProcessID || !e.dwFrameTime) continue
  const nameBuf = Buffer.from(e.szName)
  const nz = nameBuf.indexOf(0)
  gameName = nameBuf.toString('latin1', 0, nz < 0 ? nameBuf.length : nz)
}
unmap(base0); close(h0)
if (!gameName) { console.log('Kein aktives Spiel gefunden.'); process.exit(1) }
console.log('Ziel-Profil (spielspezifisch, NICHT global):', gameName)

const lib = koffi.load('C:/Program Files (x86)/RivaTuner Statistics Server/RTSSHooks64.dll')
const loadProfile = lib.func('void __cdecl LoadProfile(str p)')
const getProp = lib.func('int __cdecl GetProfileProperty(str name, _Out_ void* data, uint32 size)')
const setProp = lib.func('int __cdecl SetProfileProperty(str name, void* data, uint32 size)')
const saveProfile = lib.func('void __cdecl SaveProfile(str p)')
let updateProfiles = null
try { updateProfiles = lib.func('void __cdecl UpdateProfiles()') } catch (e) { console.log('UpdateProfiles() Signatur nicht ladbar:', e.message) }

loadProfile(gameName)
const rd = () => { const b = Buffer.alloc(8); const r = getProp('EnableFill', b, 4); return r ? b.readUInt32LE(0) : null }
console.log('EnableFill vor Set (GetProfileProperty):', rd())

const zero = Buffer.alloc(4)
const setOk = setProp('EnableFill', zero, 4)
console.log('SetProfileProperty("EnableFill", 0) ->', setOk ? 'TRUE (akzeptiert)' : 'FALSE (abgelehnt)')

if (setOk) {
  saveProfile(gameName)
  console.log('SaveProfile("' + gameName + '") aufgerufen.')
  if (updateProfiles) { updateProfiles(); console.log('UpdateProfiles() aufgerufen.') }
  console.log('EnableFill nach Set (GetProfileProperty):', rd())
  console.log('\n>>> JETZT 15 SEKUNDEN AUF DEN BILDSCHIRM SCHAUEN <<<\n')
} else {
  console.log('Set wurde abgelehnt - kein Schreibversuch, nichts zu beobachten/zuruecksetzen.')
}

setTimeout(() => {
  if (setOk) {
    const one = Buffer.from([1, 0, 0, 0])
    const restoreOk = setProp('EnableFill', one, 4)
    saveProfile(gameName)
    if (updateProfiles) updateProfiles()
    console.log('Restore SetProfileProperty("EnableFill", 1) ->', restoreOk)
    console.log('EnableFill nach Restore:', rd())
  }
  console.log('\nFertig.')
}, setOk ? 15000 : 0)
