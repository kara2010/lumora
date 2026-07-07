// Diagnose: RTSS-OSD-Slots dumpen – wer schreibt, was ist eingefroren?
// Zwei Samples im Abstand von 2,5 s zeigen, welche Slots live aktualisiert werden.
'use strict'
const path = require('path')
const koffi = require(path.join(__dirname, '..', 'node_modules', 'koffi'))
const k32 = koffi.load('kernel32.dll')
const open = k32.func('void* OpenFileMappingA(uint32 a, int i, str n)')
const map = k32.func('void* MapViewOfFile(void* h, uint32 a, uint32 hi, uint32 lo, size_t b)')
const unmap = k32.func('int UnmapViewOfFile(void* p)')
const close = k32.func('int CloseHandle(void* h)')
const HDR = koffi.struct({ sig: 'uint32', ver: 'uint32', appES: 'uint32', appOff: 'uint32', appN: 'uint32', osdES: 'uint32', osdOff: 'uint32', osdN: 'uint32', frame: 'uint32' })

function cstr(base, off, len) {
  const arr = Buffer.from(koffi.decode(base, off, koffi.array('uint8', len)))
  const z = arr.indexOf(0)
  return arr.toString('latin1', 0, z < 0 ? len : z)
}
function dumpSlots() {
  const h = open(0x0004, 0, 'RTSSSharedMemoryV2'); if (!h) return null
  const base = map(h, 0x0004, 0, 0, 0); if (!base) { close(h); return null }
  const hdr = koffi.decode(base, HDR)
  const out = { frame: hdr.frame, slots: [] }
  for (let i = 0; i < Math.min(hdr.osdN, 8); i++) {
    const so = hdr.osdOff + i * hdr.osdES
    const owner = cstr(base, so + 256, 32)
    const osd = cstr(base, so, 256)                                        // szOSD (klassisch)
    const osdEx = hdr.osdES >= 4608 ? cstr(base, so + 512, 200) : ''       // szOSDEx (gross)
    out.slots.push({ i, owner, osdLen: osd.length, osdHead: osd.slice(0, 70).replace(/[\r\n]/g, '|'), exLen: osdEx.length, exHead: osdEx.slice(0, 70).replace(/[\r\n]/g, '|') })
  }
  unmap(base); close(h)
  return out
}
const hooks = koffi.load('C:/Program Files (x86)/RivaTuner Statistics Server/RTSSHooks64.dll')
const getFlags = hooks.func('uint32 __cdecl GetFlags()')
console.log('OSD_VISIBLE:', getFlags() & 1)
const show = (s) => console.log(`  Slot ${s.i} [${s.owner}] szOSD(len=${s.osdLen}): ${s.osdHead}  ||  szOSDEx(len=${s.exLen}): ${s.exHead}`)
const a = dumpSlots()
console.log('Sample 1: frame=' + a.frame)
for (const s of a.slots) if (s.owner) show(s)
setTimeout(() => {
  const b = dumpSlots()
  console.log('Sample 2 (+2,5s): frame=' + b.frame)
  for (const s of b.slots) if (s.owner) show(s)
  for (let i = 0; i < a.slots.length; i++) {
    if (!a.slots[i].owner) continue
    const changed = a.slots[i].osdHead !== b.slots[i].osdHead || a.slots[i].exHead !== b.slots[i].exHead
    console.log(`  -> Slot ${i} (${a.slots[i].owner}): ` + (changed ? 'AENDERT sich (live)' : 'UNVERAENDERT (eingefroren/leer)'))
  }
}, 2500)
