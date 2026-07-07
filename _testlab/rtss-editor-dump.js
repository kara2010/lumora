// Voll-Dump des Overlay-Editor-Slots: Textfelder als Hex+Text, Buffer als
// Objekt-Kette (Signatur + Groesse) – Reverse-Engineering der Layout-Uebergabe.
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

const h = open(0x0004, 0, 'RTSSSharedMemoryV2')
const base = map(h, 0x0004, 0, 0, 0)
const hdr = koffi.decode(base, HDR)
console.log('osdEntrySize=' + hdr.osdES)
for (let i = 0; i < Math.min(hdr.osdN, 8); i++) {
  const so = hdr.osdOff + i * hdr.osdES
  const raw = Buffer.from(koffi.decode(base, so, koffi.array('uint8', hdr.osdES)))
  const cstr = (off, max) => { const e = raw.indexOf(0, off); return raw.toString('latin1', off, e < 0 || e > off + max ? off + max : e) }
  const owner = cstr(256, 40)
  if (!owner) continue
  console.log(`\n=== Slot ${i} owner="${owner}" ===`)
  console.log('szOSD   (text):', JSON.stringify(cstr(0, 250)))
  console.log('szOSDEx (text, 1200):', JSON.stringify(cstr(512, 1200)))
  // szOSDEx2 @ 256+256+4096+262144 = 266752, 32 KB (v2.20+) – NIE zuvor geprueft!
  if (hdr.osdES >= 266752 + 32768) {
    const ex2 = cstr(266752, 8000)
    console.log('szOSDEx2 (text, 8000):', JSON.stringify(ex2))
    if (!ex2) console.log('szOSDEx2 (hex 0..64):', raw.slice(266752, 266752 + 64).toString('hex'))
  }
  // Buffer-Objektkette ab 4608: [sig 4][size 4][payload size-8]...
  let off = 4608
  let n = 0
  console.log('--- Buffer-Objekte ---')
  while (off + 8 <= hdr.osdES && n < 40) {
    const sig = raw.toString('latin1', off, off + 4)
    const size = raw.readUInt32LE(off + 4)
    if (!/^[\x20-\x7e]{4}$/.test(sig) || size < 8 || size > 1 << 20) {
      // keine gueltige Signatur mehr: pruefen ob Rest nur Nullen
      const rest = raw.slice(off, Math.min(off + 4096, hdr.osdES))
      const nz = rest.findIndex(b => b !== 0)
      if (nz < 0) { console.log(`(ab Offset ${off}: alles 0)`); break }
      console.log(`(Offset ${off}: keine Signatur, naechste Nicht-Null bei +${nz}: ${rest.slice(nz, nz + 32).toString('hex')})`)
      break
    }
    console.log(`OBJ${n} @${off}: sig="${sig}" size=${size}`)
    console.log('   payload:', raw.slice(off + 8, off + Math.min(size, 8 + 88)).toString('hex'))
    off += size
    n++
  }
}
unmap(base); close(h)
