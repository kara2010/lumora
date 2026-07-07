// Grober PDF-Text-Extraktor: inflatet alle Flate-Streams und zieht die
// Text-Operatoren (Tj/TJ) heraus. Reicht, um im RTSS-ReadMe die
// Hypertext-Tag-Referenz zu finden.
'use strict'
const fs = require('fs')
const zlib = require('zlib')
const b = fs.readFileSync(process.argv[2])
const out = []
let idx = 0
while (true) {
  const s = b.indexOf('stream', idx)
  if (s < 0) break
  let ds = s + 6
  if (b[ds] === 0x0d) ds++
  if (b[ds] === 0x0a) ds++
  const e = b.indexOf('endstream', ds)
  if (e < 0) break
  idx = e + 9
  let raw
  try { raw = zlib.inflateSync(b.slice(ds, e)) } catch { continue }
  const t = raw.toString('latin1')
  if (!/T[jJ]/.test(t)) continue
  // Textzeilen: (…) Tj  und  [(…)(…)] TJ  – Klammerinhalte einsammeln
  let line = ''
  const re = /\((?:\\.|[^\\()])*\)|TJ|Tj|T\*|Td|TD/g
  let m
  while ((m = re.exec(t))) {
    const tok = m[0]
    if (tok[0] === '(') {
      line += tok.slice(1, -1).replace(/\\([()\\])/g, '$1').replace(/\\(\d{3})/g, (_, o) => String.fromCharCode(parseInt(o, 8)))
    } else if (tok === 'Tj' || tok === 'TJ') {
      // weiter sammeln bis Zeilenumbruch-Operator
    } else { // T*, Td, TD = neue Zeile
      if (line.trim()) out.push(line)
      line = ''
    }
  }
  if (line.trim()) out.push(line)
}
fs.writeFileSync(process.argv[3], out.join('\n'))
console.log('Zeilen:', out.length, '->', process.argv[3])
