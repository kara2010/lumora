// Extrahiert UI-Strings aus der deutschen Lokalisierungs-DLL des Overlay-Editors,
// um die exakte Beschriftung der "Layout ans OSD anhaengen"-Option zu finden.
'use strict'
const fs = require('fs')
const file = process.argv[2] || 'C:/Program Files (x86)/RivaTuner Statistics Server/Localization/GER/Translation/Plugins/Client/OverlayEditor.dll'
const b = fs.readFileSync(file)
const found = new Set()
// UTF-16LE-Strings
let cur = ''
for (let i = 0; i + 1 < b.length; i += 2) {
  const c = b.readUInt16LE(i)
  const ch = String.fromCharCode(c)
  if ((c >= 32 && c < 127) || 'äöüÄÖÜß§°'.includes(ch)) cur += ch
  else { if (cur.length >= 4) found.add('U16|' + cur); cur = '' }
}
if (cur.length >= 4) found.add('U16|' + cur)
// ASCII-Strings
cur = ''
for (let i = 0; i < b.length; i++) {
  const c = b[i]
  if (c >= 32 && c < 127) cur += String.fromCharCode(c)
  else { if (cur.length >= 5) found.add('A|' + cur); cur = '' }
}
const pat = process.argv[3] ? new RegExp(process.argv[3], 'i') : /osd|anzeig|anhäng|einblend|attach|display|übernehm|aktivier|overlay|layout/i
const hits = [...found].filter(s => pat.test(s.slice(s.indexOf('|') + 1)) && s.length < 120)
for (const s of hits.sort()) console.log(s)
console.log('--- gesamt:', found.size, 'Strings, Treffer:', hits.length)
