// Laesst den Broker >8s mit Heartbeat laufen und prueft, ob brokerTick ueber die
// 8s-Grenze hinaus weiterlaeuft (Heartbeat kommt an) oder einfriert (Bug).
const { app } = require('electron')
const { spawn } = require('child_process')

app.whenReady().then(() => {
  const koffi = require('koffi')
  const k32 = koffi.load('kernel32.dll')
  const CreateFileMappingA = k32.func('void* CreateFileMappingA(void* f, void* s, uint32 p, uint32 hi, uint32 lo, str n)')
  const MapViewOfFile = k32.func('void* MapViewOfFile(void* h, uint32 a, uint32 hi, uint32 lo, size_t b)')
  const GetTickCount = k32.func('uint32 GetTickCount()')
  const FULL = koffi.struct({ magic: 'uint32', brokerTick: 'uint32', fps: 'uint32', frametimeX100: 'uint32', apiCode: 'uint32', pid: 'uint32', appTick: 'uint32', wanted: 'uint32' })
  const APP = koffi.struct({ appTick: 'uint32', wanted: 'uint32' })

  const h = CreateFileMappingA(koffi.as(-1, 'void*'), null, 0x04, 0, 64, 'Local\\LumoraOSDFps')
  const base = MapViewOfFile(h, 0xF001F, 0, 0, 0)
  koffi.encode(base, 24, APP, { appTick: GetTickCount(), wanted: 1 })
  spawn('schtasks', ['/run', '/tn', 'LumoraOSD-FPS'], { windowsHide: true })

  const samples = []
  let i = 0
  const t = setInterval(() => {
    koffi.encode(base, 24, APP, { appTick: GetTickCount(), wanted: 1 })   // Heartbeat @24
    const s = koffi.decode(base, FULL)
    if (i % 4 === 0) samples.push({ sec: Math.round(i / 4), brokerTick: s.brokerTick, appTickSeen: s.appTick, wantedSeen: s.wanted, fps: s.fps })
    if (++i >= 60) {   // 15 s
      clearInterval(t)
      koffi.encode(base, 24, APP, { appTick: 0, wanted: 0 })
      // Lief brokerTick ueber die 8s-Grenze weiter?
      var ticks = samples.map(x => x.brokerTick)
      var alive = ticks[ticks.length - 1] !== ticks[Math.max(0, ticks.length - 3)]
      console.log('BROKER_LIFE ' + JSON.stringify({ aliveAtEnd: alive, samples: samples }))
      setTimeout(() => app.quit(), 400)
    }
  }, 250)
})
app.on('window-all-closed', () => app.quit())
