// Minimaler PE32+-Export-Tabellen-Parser: listet alle exportierten Funktionsnamen
// einer DLL/EXE. Reines Metadaten-Auslesen (keine Disassemblierung von Code) -
// zeigt, welche Funktionen RTSSHooks64.dll/RTSS.exe ueberhaupt nach aussen anbieten.
'use strict'
const fs = require('fs')
const file = process.argv[2]
const buf = fs.readFileSync(file)

const e_lfanew = buf.readUInt32LE(0x3c)
if (buf.toString('latin1', e_lfanew, e_lfanew + 4) !== 'PE\0\0') { console.log('Kein gueltiger PE-Header'); process.exit(1) }
const fileHdrOff = e_lfanew + 4
const machine = buf.readUInt16LE(fileHdrOff)
const numSections = buf.readUInt16LE(fileHdrOff + 2)
const sizeOfOptHdr = buf.readUInt16LE(fileHdrOff + 16)
const optHdrOff = fileHdrOff + 20
const magic = buf.readUInt16LE(optHdrOff)
console.log('Machine=0x' + machine.toString(16), 'Magic=0x' + magic.toString(16), '(0x20b=PE32+, 0x10b=PE32)', 'Sections=' + numSections)

const isPE32Plus = magic === 0x20b
const dataDirOff = optHdrOff + (isPE32Plus ? 112 : 96)
const exportRVA = buf.readUInt32LE(dataDirOff)
const exportSize = buf.readUInt32LE(dataDirOff + 4)
if (!exportRVA) { console.log('Keine Export-Tabelle.'); process.exit(0) }

// Sections fuer RVA->FileOffset
const sectHdrOff = optHdrOff + sizeOfOptHdr
const sections = []
for (let i = 0; i < numSections; i++) {
  const off = sectHdrOff + i * 40
  sections.push({
    name: buf.toString('latin1', off, off + 8).replace(/\0+$/, ''),
    vsize: buf.readUInt32LE(off + 8),
    vaddr: buf.readUInt32LE(off + 12),
    rawSize: buf.readUInt32LE(off + 16),
    rawPtr: buf.readUInt32LE(off + 20),
  })
}
function rva2off(rva) {
  for (const s of sections) if (rva >= s.vaddr && rva < s.vaddr + Math.max(s.vsize, s.rawSize)) return rva - s.vaddr + s.rawPtr
  return -1
}
const expOff = rva2off(exportRVA)
if (expOff < 0) { console.log('Export-RVA nicht in Sections gefunden.'); process.exit(1) }

const dllNameRVA = buf.readUInt32LE(expOff + 12)
const dllNameOff = rva2off(dllNameRVA)
const dllName = buf.toString('latin1', dllNameOff, buf.indexOf(0, dllNameOff))
const numNames = buf.readUInt32LE(expOff + 24)
const namesRVA = buf.readUInt32LE(expOff + 32)
const namesOff = rva2off(namesRVA)

console.log(`\nExportierte DLL-Name: ${dllName}  |  Anzahl benannter Exporte: ${numNames}\n`)
const names = []
for (let i = 0; i < numNames; i++) {
  const nameRVA = buf.readUInt32LE(namesOff + i * 4)
  const nameOff = rva2off(nameRVA)
  const name = buf.toString('latin1', nameOff, buf.indexOf(0, nameOff))
  names.push(name)
}
names.sort()
names.forEach((n) => console.log('  ' + n))
