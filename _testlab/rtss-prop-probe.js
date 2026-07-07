// Probe: Welche Profil-Properties kennt RTSS? Kandidaten via GetProfileProperty
// testen (ret=1 => existiert) und aktuelle Werte anzeigen. Rein lesend.
'use strict'
const path = require('path')
const koffi = require(path.join(__dirname, '..', 'node_modules', 'koffi'))
const lib = koffi.load('C:/Program Files (x86)/RivaTuner Statistics Server/RTSSHooks64.dll')
const load = lib.func('void __cdecl LoadProfile(str p)')
const get = lib.func('int __cdecl GetProfileProperty(str name, _Out_ void* data, uint32 size)')

const candidates = [
  'EnableOSD', 'PositionX', 'PositionY', 'Position', 'OSDPositionX', 'OSDPositionY',
  'ZoomRatio', 'Zoom', 'OSDZoom', 'BaseColor', 'BgndColor', 'FillColor', 'Coordinates',
  'CoordinateSpace', 'LayerMode', 'RenderMode', 'FrameColor', 'CaptureFlags',
]
load('')
for (const name of candidates) {
  const buf = Buffer.alloc(8)
  const r = get(name, buf, 4)
  if (r) console.log(`  ${name} = ${buf.readUInt32LE(0)} (int32: ${buf.readInt32LE(0)})`)
  else console.log(`  ${name} -- existiert nicht`)
}
