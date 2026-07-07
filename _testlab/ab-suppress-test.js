// Experiment: Laesst sich das NATIVE Afterburner-OSD flackerfrei unterdruecken,
// indem wir seinen RTSS-Slot-Text fluechtig leeren? (Keine AB-Einstellung wird
// beruehrt; nach dem Loslassen schreibt AB von selbst wieder -> voll autark.)
// Phase 1: 6 s beobachten (Schreibperiode von AB messen)
// Phase 2: 20 s unterdruecken (Poll 25 ms, nullen + dwOSDFrame++)
// Phase 3: loslassen, 5 s warten -> kommt ABs Text von selbst wieder?
'use strict'
const path = require('path')
const koffi = require(path.join(__dirname, '..', 'node_modules', 'koffi'))
const k32 = koffi.load('kernel32.dll')
const open = k32.func('void* OpenFileMappingA(uint32 a, int i, str n)')
const map = k32.func('void* MapViewOfFile(void* h, uint32 a, uint32 hi, uint32 lo, size_t b)')
const unmap = k32.func('int UnmapViewOfFile(void* p)')
const close = k32.func('int CloseHandle(void* h)')
const HDR = koffi.struct({ sig: 'uint32', ver: 'uint32', appES: 'uint32', appOff: 'uint32', appN: 'uint32', osdES: 'uint32', osdOff: 'uint32', osdN: 'uint32', frame: 'uint32' })
const EX2 = 256 + 256 + 4096 + 262144

function cstr(base, off, len) {
  const arr = Buffer.from(koffi.decode(base, off, koffi.array('uint8', len)))
  const z = arr.indexOf(0)
  return arr.toString('latin1', 0, z < 0 ? len : z)
}
function enc(base, off, buf) { koffi.encode(base, off, koffi.array('uint8', buf.length), buf) }

const h = open(0x0006, 0, 'RTSSSharedMemoryV2')   // READ|WRITE
if (!h) { console.log('RTSS SharedMemory nicht offen (RTSS laeuft nicht?)'); process.exit(1) }
const base = map(h, 0x0006, 0, 0, 0)
if (!base) { console.log('Map fehlgeschlagen'); process.exit(1) }
const hdr = koffi.decode(base, HDR)
console.log(`RTSS v0x${hdr.ver.toString(16)}  Slots=${hdr.osdN}  EntrySize=${hdr.osdES}`)

// Afterburner-Slot suchen
let ab = -1
for (let i = 0; i < Math.min(hdr.osdN, 16); i++) {
  const owner = cstr(base, hdr.osdOff + i * hdr.osdES + 256, 32)
  if (/afterburner/i.test(owner)) { ab = i; console.log(`AB-Slot: ${i} (owner='${owner}')`); break }
  if (owner) console.log(`  Slot ${i}: owner='${owner}'`)
}
if (ab < 0) { console.log('KEIN Afterburner-Slot gefunden – laeuft ABs OSD gerade?'); process.exit(1) }
const so = hdr.osdOff + ab * hdr.osdES
const fields = [ { name: 'szOSD', off: so, len: 256 }, { name: 'szOSDEx', off: so + 512, len: 4096 } ]
if (hdr.osdES >= EX2 + 32768) fields.push({ name: 'szOSDEx2', off: so + EX2, len: 32768 })

const t0 = Date.now()
const stamp = () => ((Date.now() - t0) / 1000).toFixed(2) + 's'

// Phase 1: beobachten
console.log('\n--- Phase 1: 6 s beobachten ---')
let last = fields.map(f => cstr(base, f.off, Math.min(f.len, 512)))
let writes = []
const p1End = Date.now() + 6000
while (Date.now() < p1End) {
  for (let fi = 0; fi < fields.length; fi++) {
    const cur = cstr(base, fields[fi].off, Math.min(fields[fi].len, 512))
    if (cur !== last[fi]) { writes.push(Date.now()); last[fi] = cur; console.log(`${stamp()}  AB schrieb ${fields[fi].name} (len=${cur.length})`) }
  }
  const until = Date.now() + 25
  while (Date.now() < until) {}   // busy-wait 25ms (praeziser als setTimeout im Skript)
}
const periods = writes.slice(1).map((t, i) => t - writes[i])
console.log(`AB-Schreibvorgaenge in 6 s: ${writes.length}` + (periods.length ? `, Periode ~${Math.round(periods.reduce((a, b) => a + b, 0) / periods.length)} ms` : ''))
const nonEmpty = fields.map((f, i) => ({ f, hasText: !!cstr(base, f.off, 64) })).filter(x => x.hasText)
console.log('Befuellte Felder: ' + (nonEmpty.map(x => x.f.name).join(', ') || 'KEINE (OSD evtl. gerade leer?)'))

// Phase 2: unterdruecken
const P2_MS = parseInt(process.argv[2], 10) || 20000
console.log(`\n--- Phase 2: ${Math.round(P2_MS / 1000)} s unterdruecken (Poll 25 ms) – JETZT aufs Spiel schauen! ---`)
const zero = Buffer.from([0])
let suppressed = 0, visibleMsMax = 0
let lastClear = 0
const p2End = Date.now() + P2_MS
// initial leeren
for (const f of fields) enc(base, f.off, zero)
koffi.encode(base, 32, 'uint32', (koffi.decode(base, 32, 'uint32') + 1) >>> 0)
while (Date.now() < p2End) {
  let dirty = false
  for (const f of fields) { if (koffi.decode(base, f.off, 'uint8') !== 0) { dirty = true; break } }
  if (dirty) {
    const now = Date.now()
    for (const f of fields) enc(base, f.off, zero)
    koffi.encode(base, 32, 'uint32', (koffi.decode(base, 32, 'uint32') + 1) >>> 0)
    suppressed++
    if (lastClear) visibleMsMax = Math.max(visibleMsMax, now - lastClear > 25 ? 25 : now - lastClear)
    lastClear = now
  }
  const until = Date.now() + 25
  while (Date.now() < until) {}
}
console.log(`Unterdrueckt: ${suppressed} AB-Schreibvorgaenge in 20 s (Sichtbarkeitsfenster je <=25 ms + 1 Renderzyklus)`)

// Phase 3: loslassen
console.log('\n--- Phase 3: losgelassen, 5 s warten – kommt AB von selbst wieder? ---')
const p3End = Date.now() + 5000
let back = false
while (Date.now() < p3End) {
  for (const f of fields) { if (cstr(base, f.off, 64)) { back = true; break } }
  if (back) break
  const until = Date.now() + 50
  while (Date.now() < until) {}
}
console.log(back ? `AB schreibt wieder von selbst (${stamp()}) -> voll autark, nichts kaputt` : 'AB hat in 5 s NICHT neu geschrieben (Monitoring-Periode pruefen)')
unmap(base); close(h)
