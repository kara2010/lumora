// EnableFill gefunden in Profiles/Global [OSD]. Pruefen: per GetProfileProperty
// lesbar? Per SetProfileProperty+SaveProfile schreibbar+reversibel? WICHTIG:
// SaveProfile('') wuerde die GLOBALE Config-Datei aendern (RTSS-weit, nicht nur
// Lumora) -- das wollen wir NICHT dauerhaft. Nur LESEND testen in diesem Schritt.
'use strict'
const path = require('path')
const koffi = require(path.join(__dirname, '..', 'node_modules', 'koffi'))
const lib = koffi.load('C:/Program Files (x86)/RivaTuner Statistics Server/RTSSHooks64.dll')
const loadProfile = lib.func('void __cdecl LoadProfile(str p)')
const getProp = lib.func('int __cdecl GetProfileProperty(str name, _Out_ void* data, uint32 size)')

loadProfile('')
for (const n of ['EnableFill', 'EnableBgnd', 'EnableStat', 'FillColor', 'EnableFrameColorBar', 'ScaleToFit']) {
  const buf = Buffer.alloc(8)
  const r = getProp(n, buf, 4)
  console.log(r ? `  ${n} = ${buf.readUInt32LE(0)} (0x${buf.readUInt32LE(0).toString(16)})` : `  ${n} -- existiert nicht`)
}
