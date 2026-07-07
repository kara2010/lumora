// Beweis-Test: Beginnt Afterburner wieder zu schreiben, sobald OSD_VISIBLE=1?
// Setzt das Flag, beobachtet ABs Slot 3 s lang, stellt den Ausgangszustand wieder her.
'use strict'
const path = require('path')
const koffi = require(path.join(__dirname, '..', 'node_modules', 'koffi'))
const k32 = koffi.load('kernel32.dll')
const open = k32.func('void* OpenFileMappingA(uint32 a, int i, str n)')
const map = k32.func('void* MapViewOfFile(void* h, uint32 a, uint32 hi, uint32 lo, size_t b)')
const unmap = k32.func('int UnmapViewOfFile(void* p)')
const close = k32.func('int CloseHandle(void* h)')
const HDR = koffi.struct({ sig: 'uint32', ver: 'uint32', appES: 'uint32', appOff: 'uint32', appN: 'uint32', osdES: 'uint32', osdOff: 'uint32', osdN: 'uint32', frame: 'uint32' })
function abSlotText() {
  const h = open(0x0004, 0, 'RTSSSharedMemoryV2'); if (!h) return null
  const base = map(h, 0x0004, 0, 0, 0); if (!base) { close(h); return null }
  const hdr = koffi.decode(base, HDR)
  let res = null
  for (let i = 0; i < Math.min(hdr.osdN, 8); i++) {
    const so = hdr.osdOff + i * hdr.osdES
    const ob = Buffer.from(koffi.decode(base, so + 256, koffi.array('uint8', 32)))
    const z = ob.indexOf(0); const owner = ob.toString('latin1', 0, z < 0 ? 32 : z)
    if (/afterburner/i.test(owner)) {
      const tb = Buffer.from(koffi.decode(base, so, koffi.array('uint8', 200)))
      const tz = tb.indexOf(0)
      const eb = hdr.osdES >= 4608 ? Buffer.from(koffi.decode(base, so + 512, koffi.array('uint8', 200))) : Buffer.alloc(0)
      const ez = eb.indexOf(0)
      res = { osd: tb.toString('latin1', 0, tz < 0 ? 200 : tz), ex: ez !== 0 && eb.length ? eb.toString('latin1', 0, ez < 0 ? 200 : ez) : '' }
      break
    }
  }
  unmap(base); close(h)
  return res
}
const hooks = koffi.load('C:/Program Files (x86)/RivaTuner Statistics Server/RTSSHooks64.dll')
const getFlags = hooks.func('uint32 __cdecl GetFlags()')
const setFlags = hooks.func('void __cdecl SetFlags(uint32 dwAND, uint32 dwXOR)')
const before = getFlags()
console.log('Flag vorher:', before & 1, '| AB-Slot:', JSON.stringify(abSlotText()))
setFlags((~1) >>> 0, 1)
console.log('Flag gesetzt ->', getFlags() & 1)
setTimeout(() => {
  console.log('Nach 3 s | AB-Slot:', JSON.stringify(abSlotText()))
  // Ausgangszustand wiederherstellen
  if (!(before & 1)) setFlags((~1) >>> 0, 0)
  console.log('Flag zurueckgestellt ->', getFlags() & 1)
}, 3000)
