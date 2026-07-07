// Sonde: ist die "Bildschirmfuellung (Fill)"-Option ueber GetProfileProperty
// erreichbar, oder das volle Flags-Bitmuster (nicht nur Bit0 OSD_VISIBLE)?
'use strict'
const path = require('path')
const koffi = require(path.join(__dirname, '..', 'node_modules', 'koffi'))
const lib = koffi.load('C:/Program Files (x86)/RivaTuner Statistics Server/RTSSHooks64.dll')
const loadProfile = lib.func('void __cdecl LoadProfile(str p)')
const getProp = lib.func('int __cdecl GetProfileProperty(str name, _Out_ void* data, uint32 size)')
const getFlags = lib.func('uint32 __cdecl GetFlags()')
loadProfile('')
const fl = getFlags()
console.log('Flags (voll):', '0x' + fl.toString(16).padStart(8, '0'), '=', fl.toString(2).padStart(32, '0'))
const names = ['Fill', 'FillBackground', 'BackgroundFill', 'ScreenFill', 'OSDFill', 'UseFill', 'ShowFill',
  'Frame', 'FrameFill', 'Background', 'BgFill', 'Solid', 'SolidBackground', 'ClassicOSD', 'LegacyOSD',
  'FillEnable', 'FillMode', 'FillStyle', 'FillArea', 'FillOpacity', 'BgndOpacity', 'BgndAlpha', 'Alpha']
for (const n of names) {
  const buf = Buffer.alloc(8)
  const r = getProp(n, buf, 4)
  console.log(r ? `  ${n} = ${buf.readUInt32LE(0)} (0x${buf.readUInt32LE(0).toString(16)})` : `  ${n} -- existiert nicht`)
}
