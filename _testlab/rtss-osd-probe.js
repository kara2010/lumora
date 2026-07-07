// Schreibt einen Marker-Text mit verschiedenen Positions-Tag-Varianten in unseren
// RTSS-OSD-Slot und schaltet die Anzeige an. Zusammen mit einem Bildschirm-Capture
// laesst sich so die Tag-Semantik OHNE Spiel visuell verifizieren (RTSS rendert
// auch in beschleunigte Fenster wie Lumora/Chromium).
'use strict'
const path = require('path')
const koffi = require(path.join(__dirname, '..', 'node_modules', 'koffi'))
const k32 = koffi.load('kernel32.dll')
const open = k32.func('void* OpenFileMappingA(uint32 a, int i, str n)')
const map = k32.func('void* MapViewOfFile(void* h, uint32 a, uint32 hi, uint32 lo, size_t b)')
const unmap = k32.func('int UnmapViewOfFile(void* p)')
const close = k32.func('int CloseHandle(void* h)')
const HDR = koffi.struct({ sig: 'uint32', ver: 'uint32', appES: 'uint32', appOff: 'uint32', appN: 'uint32', osdES: 'uint32', osdOff: 'uint32', osdN: 'uint32', frame: 'uint32' })

const variant = process.argv[2] || '1'
const texts = {
  // Variante 1: alle Kandidaten als Marker in einem Text
  '1': 'S0-STAPELANFANG\n<P2>M1-P2ANKER\n<P=2400,120>M2-PIXEL2400\n<A=-120>M3-ALIGNBOX<A>\nS1-ENDE',
  // Variante 2: nur Pixel-Positionierung, jede Zeile einzeln gesetzt
  '2': '<P=2400,40>ZEILE-A<P=2400,80>ZEILE-B<P=2400,120>ZEILE-C',
  // Variante 3: schlichter Kontrolltext ohne Tags
  '3': 'LUMORA-PROBE OHNE TAGS',
  // Variante li: Layout-Einbettung mit ABSOLUTEM Pfad (Overlay-Editor-Mechanik)
  'li': 'LUMORA-MARKER-ABS\n<LI=' + (process.env.APPDATA + '\\lumora\\lumora.ovl') + '>',
  // Variante li-rel: mitgeliefertes Layout, RELATIVER Pfad (wie in der DLL gesehen)
  'li-rel': 'LUMORA-MARKER\n<LI=Plugins\\Client\\Overlays\\mini.ovl>',
  // Variante ex2: Layout-Hypertext nach szOSDEx2 (Feld @266752, v2.20+) –
  // exakt die Syntax, die der Overlay-Editor/Afterburner dort schreiben
  // (<P=x,y> Zellen, negativ = von rechts/unten; <E> Extents; <FNT> Font).
  'ex2': '<FNT=Consolas,-16,700,1>'
       + '<C1=74E857><C2=8A8A96><C3=FFE100>'
       + '<P=-16,0><E=-16,-4><C=E60D1016><B=0,0>\b<C>'
       + '<P=-16,0><E=-16,-1><C1>LUMORA EX2<C>'
       + '<P=-16,-1><E=-8,-1><C2>GPU Temp<C>'
       + '<P=-8,-1><E=-8,-1,2><C3>42 °C<C>'
       + '<P=-16,-2><E=-8,-1><C2>CPU Temp<C>'
       + '<P=-8,-2><E=-8,-1,2><C3>77 °C<C>'
       + '<P=-16,-3><E=-8,-1><C2>FPS<C>'
       + '<P=-8,-3><E=-8,-1,2><FR> FPS',
  // Variante cal: Koordinaten-Kalibrierung in szOSDEx2 – Marker mit
  // verschiedenen Semantiken (positive Pixel? negative Zellen? Box vs. Screen)
  'cal': '<FNT=Consolas,-16,700,1>'
       + '<C=FFFF3030>A-ORIGIN<C>'
       + '<P=900,16><C=FF30FF30>B-PIX900<C>'
       + '<P=1600,64><C=FF30FFFF>C-PIX1600<C>'
       + '<P=-30,-2><C=FFFF30FF>D-NEG30<C>',
  // Variante right: Panel OBEN RECHTS via absolute Pixel (Client-Aufloesung
  // 1920 hart kodiert fuer den GG-Test; produktiv aus dwResolutionX @9224)
  'right': (() => {
    const resX = 1920, charW = 8.8, lineH = 20, margin = 8
    const rows = [
      ['LUMORA RECHTS', 'FF74E857'],
      ['GPU  42\xB0C  40W', 'FFFFE100'],
      ['CPU  77\xB0C  51W', 'FFFFE100'],
      ['FPS-Anzeige folgt', 'FF8A8A96'],
    ]
    const w = Math.max(...rows.map(r => r[0].length)) * charW + 12
    const x = Math.round(resX - margin - w)
    let s = '<FNT=Consolas,-16,700,1>'
    rows.forEach(([t, c], i) => { s += `<P=${x},${i * lineH}><C=${c}>${t}<C>` })
    return s
  })(),
  // clear: Slot raeumen
  'clear': '',
}
const text = texts[variant] !== undefined ? texts[variant] : texts['1']

const h = open(0x0006, 0, 'RTSSSharedMemoryV2')
if (!h) { console.log('RTSS SHM nicht offen'); process.exit(1) }
const base = map(h, 0x0006, 0, 0, 0)
const hdr = koffi.decode(base, HDR)
const enc = (off, buf) => koffi.encode(base, off, koffi.array('uint8', buf.length), Array.from(buf))
let mine = -1, free = -1
for (let i = 1; i < Math.min(hdr.osdN, 16); i++) {
  const so = hdr.osdOff + i * hdr.osdES
  const ob = Buffer.from(koffi.decode(base, so + 256, koffi.array('uint8', 16)))
  const z = ob.indexOf(0)
  const owner = ob.toString('latin1', 0, z < 0 ? 16 : z)
  if (owner === 'Lumora') { mine = i; break }
  if (free < 0 && !owner) free = i
}
const slot = mine >= 0 ? mine : free
console.log('Slot:', slot, '(mine=' + mine + ', free=' + free + ')')
const so = hdr.osdOff + slot * hdr.osdES
if (mine < 0) enc(so + 256, Buffer.from('Lumora\0', 'latin1'))
const buf = Buffer.from(text + '\0', 'latin1')
const EX2 = 256 + 256 + 4096 + 262144 // szOSDEx2 @266752 (v2.20+)
// NUR EIN Textfeld befuellen – RTSS rendert szOSD und szOSDEx sonst BEIDE
// (verifiziert: doppelte Marker-Zeile). Gross -> szOSDEx, szOSD leeren.
const useEx2 = variant === 'ex2' || variant === 'cal' || variant === 'right'
if (useEx2 && hdr.osdES >= EX2 + 32768) {
  enc(so + EX2, buf); enc(so + 512, Buffer.from([0])); enc(so, Buffer.from([0]))
} else if (hdr.osdES >= 4608) { enc(so + 512, buf); enc(so, Buffer.from([0])) }
else enc(so, buf.length <= 256 ? buf : Buffer.from(text.slice(0, 255) + '\0', 'latin1'))
const frame = koffi.decode(base, 32, 'uint32')
koffi.encode(base, 32, 'uint32', (frame + 1) >>> 0)
unmap(base); close(h)
// Sichtbarkeit an
const hooks = koffi.load('C:/Program Files (x86)/RivaTuner Statistics Server/RTSSHooks64.dll')
hooks.func('void __cdecl SetFlags(uint32 a, uint32 x)')((~1) >>> 0, 1)
console.log('Geschrieben (Variante ' + variant + '):', JSON.stringify(text))

// WICHTIG: RTSS raeumt Slots verstorbener Prozesse auf. Fuer sichtbare Tests muss
// der Schreiber-Prozess also AM LEBEN bleiben: Slot alle 2 s auffrischen, 10 Min lang.
if (variant !== 'clear') {
  console.log('Halte Slot am Leben (10 Minuten) – Strg+C beendet.')
  let n = 0
  const t = setInterval(() => {
    const h2 = open(0x0006, 0, 'RTSSSharedMemoryV2')
    if (!h2) return
    const b2 = map(h2, 0x0006, 0, 0, 0)
    if (b2) {
      const hd = koffi.decode(b2, HDR)
      const so2 = hd.osdOff + slot * hd.osdES
      const tb = Buffer.from(text + '\0', 'latin1')
      koffi.encode(b2, so2 + 256, koffi.array('uint8', 7), Array.from(Buffer.from('Lumora\0', 'latin1')))
      if (useEx2 && hd.osdES >= EX2 + 32768) {
        koffi.encode(b2, so2 + EX2, koffi.array('uint8', tb.length), Array.from(tb))
        koffi.encode(b2, so2 + 512, koffi.array('uint8', 1), [0])
        koffi.encode(b2, so2, koffi.array('uint8', 1), [0])
      } else if (hd.osdES >= 4608) {
        koffi.encode(b2, so2 + 512, koffi.array('uint8', tb.length), Array.from(tb))
        koffi.encode(b2, so2, koffi.array('uint8', 1), [0])          // szOSD leer (sonst doppelt)
      } else {
        koffi.encode(b2, so2, koffi.array('uint8', Math.min(tb.length, 255)), Array.from(tb.slice(0, 255)))
      }
      const fr = koffi.decode(b2, 32, 'uint32')
      koffi.encode(b2, 32, 'uint32', (fr + 1) >>> 0)
      unmap(b2)
    }
    close(h2)
    if (++n > 300) { clearInterval(t); process.exit(0) }
  }, 2000)
}
