// Testet, ob der Broker-Write @0 die App-Felder @24 ueberschreibt (Padding-Bug).
const { app } = require('electron')
app.whenReady().then(() => {
  const koffi = require('koffi')
  const k32 = koffi.load('kernel32.dll')
  const CreateFileMappingA = k32.func('void* CreateFileMappingA(void* f, void* s, uint32 p, uint32 hi, uint32 lo, str n)')
  const MapViewOfFile = k32.func('void* MapViewOfFile(void* h, uint32 a, uint32 hi, uint32 lo, size_t b)')
  const FULL = koffi.struct({ magic: 'uint32', brokerTick: 'uint32', fps: 'uint32', frametimeX100: 'uint32', apiCode: 'uint32', pid: 'uint32', appTick: 'uint32', wanted: 'uint32' })
  const BROKER = koffi.struct({ magic: 'uint32', brokerTick: 'uint32', fps: 'uint32', frametimeX100: 'uint32', apiCode: 'uint32', pid: 'uint32' })
  const APP = koffi.struct({ appTick: 'uint32', wanted: 'uint32' })
  const out = { sizeofFULL: koffi.sizeof(FULL), sizeofBROKER: koffi.sizeof(BROKER), sizeofAPP: koffi.sizeof(APP) }

  const h = CreateFileMappingA(koffi.as(-1, 'void*'), null, 0x04, 0, 64, 'Local\\LumoraClobberTest')
  const base = MapViewOfFile(h, 0xF001F, 0, 0, 0)

  koffi.encode(base, 24, APP, { appTick: 999999, wanted: 1 })
  out.afterAppWrite = koffi.decode(base, FULL)
  koffi.encode(base, 0, BROKER, { magic: 0x4C4F5344, brokerTick: 111, fps: 60, frametimeX100: 1666, apiCode: 8, pid: 42 })
  out.afterBrokerWrite = koffi.decode(base, FULL)
  out.CLOBBERED = (out.afterBrokerWrite.appTick !== 999999)
  console.log('CLOBBER_TEST ' + JSON.stringify(out))
  app.quit()
})
app.on('window-all-closed', () => app.quit())
