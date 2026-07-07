// Diagnose der RTSS-Profil-API (RTSSHooks64.dll):
//   1) Welche Export-Namen existieren (plain C vs. C++-gemangelt)?
//   2) GetProfileProperty("EnableOSD") lesen
//   3) EnableOSD setzen + zurücklesen (Beweis, dass der Mechanismus wirkt)
// Laeuft OHNE Spiel und ohne Adminrechte.
'use strict'
const path = require('path')
const fs = require('fs')
const koffi = require(path.join(__dirname, '..', 'node_modules', 'koffi'))

const DLL = 'C:/Program Files (x86)/RivaTuner Statistics Server/RTSSHooks64.dll'

// --- 1) Export-Tabelle direkt aus dem PE-Header lesen (rein lesend) -----------
function listExports(file) {
  const b = fs.readFileSync(file)
  const peOff = b.readUInt32LE(0x3c)
  const numSections = b.readUInt16LE(peOff + 6)
  const optSize = b.readUInt16LE(peOff + 20)
  const optOff = peOff + 24
  const exportRva = b.readUInt32LE(optOff + 112)   // DataDirectory[0] (PE32+)
  const secOff = optOff + optSize
  const rva2off = (rva) => {
    for (let i = 0; i < numSections; i++) {
      const o = secOff + i * 40
      const va = b.readUInt32LE(o + 12), sz = b.readUInt32LE(o + 8), raw = b.readUInt32LE(o + 20)
      if (rva >= va && rva < va + sz) return raw + (rva - va)
    }
    return -1
  }
  const e = rva2off(exportRva)
  if (e < 0) return []
  const numNames = b.readUInt32LE(e + 24)
  const namesRva = b.readUInt32LE(e + 32)
  const namesOff = rva2off(namesRva)
  const out = []
  for (let i = 0; i < numNames; i++) {
    const nameOff = rva2off(b.readUInt32LE(namesOff + i * 4))
    let end = nameOff; while (b[end] !== 0) end++
    out.push(b.toString('latin1', nameOff, end))
  }
  return out
}

const exports_ = listExports(DLL)
console.log('Exporte gesamt:', exports_.length)
const interesting = exports_.filter(n => /profile|osd/i.test(n))
console.log('Profil-/OSD-relevante Exporte:')
for (const n of interesting) console.log('  ', n)

// --- 2+3) Binden und EnableOSD lesen/setzen -----------------------------------
const lib = koffi.load(DLL)
function bind(sig) { try { return lib.func(sig) } catch (e) { return null } }
// klassische Bindung fuer evtl. gemangelte Namen:
function bindRaw(name, ret, args) { try { return lib.func(name, ret, args) } catch (e) { return null } }

const cand = {
  load: bind('void __cdecl LoadProfile(str profile)') || bindRaw(exports_.find(n => /LoadProfile/.test(n)) || 'LoadProfile', 'void', ['str']),
  save: bind('void __cdecl SaveProfile(str profile)') || bindRaw(exports_.find(n => /SaveProfile/.test(n)) || 'SaveProfile', 'void', ['str']),
  get: bind('int __cdecl GetProfileProperty(str name, _Out_ void* data, uint32 size)') || bindRaw(exports_.find(n => /GetProfileProperty/.test(n)) || 'GetProfileProperty', 'int', ['str', 'void*', 'uint32']),
  set: bind('int __cdecl SetProfileProperty(str name, void* data, uint32 size)') || bindRaw(exports_.find(n => /SetProfileProperty/.test(n)) || 'SetProfileProperty', 'int', ['str', 'void*', 'uint32']),
  update: bind('void __cdecl UpdateProfiles()') || bindRaw(exports_.find(n => /UpdateProfiles/.test(n)) || 'UpdateProfiles', 'void', []),
}
console.log('\nBindung: load=%s save=%s get=%s set=%s update=%s',
  !!cand.load, !!cand.save, !!cand.get, !!cand.set, !!cand.update)

if (cand.load && cand.get && cand.set && cand.save && cand.update) {
  const buf = Buffer.alloc(4)
  cand.load('')                                  // globales Profil
  let r = cand.get('EnableOSD', buf, 4)
  console.log('EnableOSD aktuell: ret=%d wert=%d', r, buf.readUInt32LE(0))
  buf.writeUInt32LE(1, 0)
  const rs = cand.set('EnableOSD', buf, 4)
  cand.save('')
  cand.update()
  const buf2 = Buffer.alloc(4)
  cand.load('')
  r = cand.get('EnableOSD', buf2, 4)
  console.log('Nach Set(1)+Save+Update: set-ret=%d  read-ret=%d wert=%d', rs, r, buf2.readUInt32LE(0))
} else {
  console.log('Nicht alle Funktionen bindbar – Exportliste oben pruefen.')
}
