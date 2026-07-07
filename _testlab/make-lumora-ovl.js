// Generiert %APPDATA%\lumora\lumora.ovl – ein RTSS-Overlay-Editor-Layout:
// kompaktes Lumora-Panel OBEN RECHTS (Zellen-Anker, negative X = von rechts),
// GPU-/CPU-Zeilen (HAL-Quellen), grosse FPS (<FR>) und Frametime-Graph.
// Eingebettet wird es ueber das <LI=...>-Tag im OSD-Slot.
'use strict'
const fs = require('fs')
const path = require('path')

const W = 34               // Panelbreite in Zellen
const X = -(W + 1)         // linke Kante: W+1 Zellen von rechts
const sources = [
  ['GPU usage', 'GPU1 usage', '%'],
  ['GPU temperature', 'GPU1 temperature', '\xB0C'],
  ['GPU power', 'GPU1 power', 'W'],
  ['GPU clock', 'GPU1 clock', 'MHz'],
  ['CPU usage', 'CPU usage', '%'],
  ['CPU temperature', 'CPU temperature', '\xB0C'],
  ['CPU power', 'CPU power', 'W'],
  ['CPU clock', 'CPU clock', 'MHz'],
]
const L = []
L.push('[Master]')
L.push('Implementation=2')
L.push('FontFace=Consolas')
L.push('FontHeight=-16')
L.push('FontWeight=700')
L.push('ZoomRatio=1')
L.push('[Settings]')
L.push('Name=Lumora')
L.push('RefreshPeriod=500')
L.push('LockUserSettings=1')
L.push('EmbeddedImage=')
L.push('PingAddr=')
L.push('[General]')
L.push('Sources=' + sources.length)
L.push('Tables=0')
L.push('Layers=7')
sources.forEach(([name, id, units], i) => {
  L.push(`[Source${i}]`)
  L.push('Name=' + name)
  L.push('Units=' + units)
  L.push('Format=')
  L.push('Formula=')
  L.push('Provider=HAL')
  L.push('ID=' + id)
})
const layer = (i, name, text, x, y, ex, ey, opts = {}) => {
  L.push(`[Layer${i}]`)
  L.push('Name=' + name)
  L.push('Text=' + text)
  L.push('PositionX=' + x)
  L.push('PositionY=' + y)
  L.push('ExtentX=' + ex)
  L.push('ExtentY=' + ey)
  L.push('ExtentOrigin=' + (opts.origin != null ? opts.origin : 0))
  L.push('FixedAlignment=1')
  if (opts.size) L.push('Size=' + opts.size)
  if (opts.color) L.push('TextColor=' + opts.color)
  if (opts.bg) L.push('BgndColor=' + opts.bg)
}
// Hintergrund-Panel (8 Zeilen hoch)
layer(0, 'Background', '', X, 0, -W, -8, { bg: 'D80D1016' })
// Kennzeile
layer(1, 'Brand', 'LUMORA', X, 0, -W, -1, { size: 75, color: '74E857' })
// GPU-Zeile (gruen)
layer(2, 'GPU', 'GPU %GPU usage%%  %GPU temperature%\xB0  %GPU power%W  %GPU clock%MHz', X, -1, -W, -1, { color: '74E857' })
// CPU-Zeile (orange)
layer(3, 'CPU', 'CPU %CPU usage%%  %CPU temperature%\xB0  %CPU power%W  %CPU clock%MHz', X, -2, -W, -1, { color: 'FF8A1E' })
// FPS gross (weiss) + Einheit
layer(4, 'Framerate', '<FR>', X, -3, -8, -2, { size: 200, color: 'FFFFFF' })
layer(5, 'FPS units', ' FPS', X + 8, -4, -6, -1, { color: 'E0E0E0' })
// Frametime-Graph (unten, 3 Zeilen)
layer(6, 'Frametime graph', '<G=Frametime,0,0,1,0,100,0>', X, -5, -W, -3, { color: '6FE86F' })

const out = path.join(process.env.APPDATA, 'lumora', 'lumora.ovl')
fs.mkdirSync(path.dirname(out), { recursive: true })
fs.writeFileSync(out, Buffer.from(L.join('\r\n') + '\r\n', 'latin1'))
console.log('Geschrieben:', out, '(' + L.length + ' Zeilen)')
