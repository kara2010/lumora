// Headless-Test: getrennte Schreibbereiche (Broker @0, App @24) im selben
// Shared Memory, dann Voll-Struktur lesen. Verifiziert koffi.encode MIT Offset.
const { app } = require('electron')

app.whenReady().then(() => {
  const out = { koffi: false, read: null, error: null }
  try {
    const koffi = require('koffi')
    out.koffi = 'v' + (koffi.version || '?')
    const k32 = koffi.load('kernel32.dll')
    const CreateFileMappingA = k32.func('void* CreateFileMappingA(void* hFile, void* sec, uint32 protect, uint32 maxHi, uint32 maxLo, str name)')
    const MapViewOfFile = k32.func('void* MapViewOfFile(void* h, uint32 access, uint32 offHi, uint32 offLo, size_t bytes)')

    const FULL = koffi.struct('Full', { magic: 'uint32', brokerTick: 'uint32', fps: 'uint32', frametimeX100: 'uint32', apiCode: 'uint32', pid: 'uint32', appTick: 'uint32', wanted: 'uint32' })
    const BROKER = koffi.struct('Broker', { magic: 'uint32', brokerTick: 'uint32', fps: 'uint32', frametimeX100: 'uint32', apiCode: 'uint32', pid: 'uint32' })
    const APP = koffi.struct('App', { appTick: 'uint32', wanted: 'uint32' })

    const h = CreateFileMappingA(koffi.as(-1, 'void*'), null, 0x04, 0, 64, 'Local\\LumoraShmOffTest')
    const base = MapViewOfFile(h, 0xF001F, 0, 0, 0)

    koffi.encode(base, 0, BROKER, { magic: 0x4C4F5344, brokerTick: 111, fps: 144, frametimeX100: 694, apiCode: 8, pid: 42 })
    koffi.encode(base, 24, APP, { appTick: 999, wanted: 1 })   // @24 = nach 6 uint32
    out.read = koffi.decode(base, FULL)
    console.log('SHMOFF_TEST_OK ' + JSON.stringify(out))
  } catch (e) {
    out.error = (e && e.message)
    console.log('SHMOFF_TEST_FAIL ' + JSON.stringify(out))
  }
  app.quit()
})
app.on('window-all-closed', () => app.quit())
