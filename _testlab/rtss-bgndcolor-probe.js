// dwOSDBgndColor: laut RTSSSharedMemory.h PRO APP-ENTRY (nicht pro OSD-Slot!) im
// Shared Memory, offset 600 innerhalb RTSS_SHARED_MEMORY_APP_ENTRY (berechnet aus
// dem offiziellen Struct-Layout: dwProcessID+szName[260]+dwFlags+dwTime0/1+dwFrames+
// dwFrameTime+dwStat*(7x4)+dwOSDX/Y/Pixel/Color/Frame(5x4)+dwScreenCaptureFlags+
// szScreenCapturePath[260] = 600). Das erklaert, warum der Balken BEIDE Bloecke
// (AB+Lumora) umspannt: EIN Wert pro Spiel/Swapchain, nicht pro Text-Anbieter.
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
const BGND_OFF = 600

const h = open(0x0006, 0, 'RTSSSharedMemoryV2')   // READ|WRITE
if (!h) { console.log('RTSS SHM nicht offen'); process.exit(1) }
const base = map(h, 0x0006, 0, 0, 0)
const hdr = koffi.decode(base, HDR)
console.log(`dwAppEntrySize=${hdr.dwAppEntrySize} (muss > ${BGND_OFF + 4} sein fuer dwOSDBgndColor)`)

let best = null, bestOff = 0
const n = Math.min(hdr.dwAppArrSize, 4096)
for (let i = 0; i < n; i++) {
  const off = hdr.dwAppArrOffset + i * hdr.dwAppEntrySize
  const e = koffi.decode(base, off, APP)
  if (!e.dwProcessID || !e.dwFrameTime) continue
  const nameBuf = Buffer.from(e.szName)
  const nz = nameBuf.indexOf(0)
  const name = nameBuf.toString('latin1', 0, nz < 0 ? nameBuf.length : nz)
  if (!best || e.dwTime1 > best.time1) { best = { name, off, time1: e.dwTime1 }; bestOff = off }
}
if (!best) { console.log('Kein aktives Spiel gefunden.'); unmap(base); close(h); process.exit(1) }
console.log(`Aktives Spiel: "${best.name}" @App-Offset ${best.off}`)

const colorOff = best.off + BGND_OFF
const val = koffi.decode(base, colorOff, 'uint32')
console.log(`dwOSDBgndColor = 0x${val.toString(16).padStart(8, '0')}  (${val === 0 ? 'transparent/aus' : 'GESETZT – vermutlich unser Balken!'})`)

// Nachbar-DWORDs mitloggen (Kontext, falls mein Offset leicht daneben liegt)
console.log('\nKontext (Offset : Wert) rund um 600:')
for (let o = 588; o <= 616; o += 4) {
  const v = koffi.decode(base, best.off + o, 'uint32')
  console.log(`  +${o}: 0x${v.toString(16).padStart(8, '0')}${o === BGND_OFF ? '  <-- dwOSDBgndColor (Annahme)' : ''}`)
}
unmap(base); close(h)
