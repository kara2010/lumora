// Sonde: welche Profil-Properties gelten fuer das AKTUELL LAUFENDE SPIEL (nicht
// das globale '' -Profil)? Ermittelt das aktive Spiel wie main.js (rtssBestApp)
// und laedt dessen Profil per Prozessnamen, bevor GetProfileProperty faellt.
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

const h = open(0x0004, 0, 'RTSSSharedMemoryV2')
if (!h) { console.log('RTSS SharedMemory nicht offen'); process.exit(1) }
const base = map(h, 0x0004, 0, 0, 0)
const hdr = koffi.decode(base, HDR)
let best = null
const n = Math.min(hdr.dwAppArrSize, 4096)
for (let i = 0; i < n; i++) {
  const off = hdr.dwAppArrOffset + i * hdr.dwAppEntrySize
  const e = koffi.decode(base, off, APP)
  if (!e.dwProcessID || !e.dwFrameTime) continue
  const nameBuf = Buffer.from(e.szName)
  const nz = nameBuf.indexOf(0)
  const name = nameBuf.toString('latin1', 0, nz < 0 ? nameBuf.length : nz)
  console.log(`App-Slot ${i}: pid=${e.dwProcessID} name="${name}" time1=${e.dwTime1} frameTime=${e.dwFrameTime}`)
  if (!best || e.dwTime1 > best.time1) best = { name, pid: e.dwProcessID, time1: e.dwTime1 }
}
unmap(base); close(h)

if (!best) { console.log('\nKein aktives Spiel im RTSS-App-Array gefunden.'); process.exit(1) }
console.log(`\n=== Aktives Spiel: "${best.name}" (pid ${best.pid}) ===`)

const lib = koffi.load('C:/Program Files (x86)/RivaTuner Statistics Server/RTSSHooks64.dll')
const loadProfile = lib.func('void __cdecl LoadProfile(str p)')
const getProp = lib.func('int __cdecl GetProfileProperty(str name, _Out_ void* data, uint32 size)')

const props = ['EnableOSD', 'PositionX', 'PositionY', 'ZoomRatio', 'BaseColor', 'BgndColor', 'CoordinateSpace', 'Zoom', 'OSDZoom']

function dump(label, profileArg) {
  loadProfile(profileArg)
  console.log(`\n-- Profil "${label}" (LoadProfile("${profileArg}")) --`)
  for (const name of props) {
    const buf = Buffer.alloc(8)
    const r = getProp(name, buf, 4)
    console.log(r ? `  ${name} = ${buf.readUInt32LE(0)} (0x${buf.readUInt32LE(0).toString(16)})` : `  ${name} -- existiert nicht`)
  }
}
dump('global/leer', '')
dump(best.name, best.name)
