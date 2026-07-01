// Headless-Test: laedt nvml.dll via koffi und liest die GPU-Live-Werte aus.
//   ./node_modules/.bin/electron _testlab/nvml-test.js
const { app } = require('electron')

app.whenReady().then(() => {
  const out = { koffi: false, dll: null, init: null, gpu: null, error: null }
  try {
    const koffi = require('koffi')
    out.koffi = 'v' + (koffi.version || '?')

    let lib = null
    for (const p of ['nvml.dll', 'C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll']) {
      try { lib = koffi.load(p); out.dll = p; break } catch (e) {}
    }
    if (!lib) throw new Error('nvml.dll nicht ladbar')

    // Opaque Handle-Typ (nvmlDevice_t = Zeiger auf nvmlDevice_st)
    const nvmlDevice = koffi.pointer('nvmlDevice_t', koffi.opaque())
    koffi.struct('nvmlUtilization_t', { gpu: 'uint32', memory: 'uint32' })
    koffi.struct('nvmlMemory_t', { total: 'uint64', free: 'uint64', used: 'uint64' })

    const nvmlInit = lib.func('int nvmlInit_v2()')
    const nvmlGetHandle = lib.func('int nvmlDeviceGetHandleByIndex_v2(uint32 index, _Out_ nvmlDevice_t* device)')
    const nvmlName = lib.func('int nvmlDeviceGetName(nvmlDevice_t device, _Out_ char* name, uint32 length)')
    const nvmlUtil = lib.func('int nvmlDeviceGetUtilizationRates(nvmlDevice_t device, _Out_ nvmlUtilization_t* u)')
    const nvmlTemp = lib.func('int nvmlDeviceGetTemperature(nvmlDevice_t device, uint32 sensor, _Out_ uint32* t)')
    const nvmlPower = lib.func('int nvmlDeviceGetPowerUsage(nvmlDevice_t device, _Out_ uint32* mw)')
    const nvmlClock = lib.func('int nvmlDeviceGetClockInfo(nvmlDevice_t device, uint32 type, _Out_ uint32* mhz)')
    const nvmlMem = lib.func('int nvmlDeviceGetMemoryInfo(nvmlDevice_t device, _Out_ nvmlMemory_t* m)')

    const ri = nvmlInit()
    out.init = ri
    if (ri !== 0) throw new Error('nvmlInit_v2 rc=' + ri)

    const dev = [null]
    const rh = nvmlGetHandle(0, dev)
    if (rh !== 0) throw new Error('GetHandleByIndex rc=' + rh)
    const device = dev[0]

    const nameBuf = Buffer.alloc(96)
    nvmlName(device, nameBuf, 96)
    const util = {}, mem = {}
    const temp = [0], pw = [0], clk = [0]
    nvmlUtil(device, util)
    nvmlTemp(device, 0, temp)       // 0 = NVML_TEMPERATURE_GPU
    nvmlPower(device, pw)
    nvmlClock(device, 0, clk)       // 0 = NVML_CLOCK_GRAPHICS
    nvmlMem(device, mem)

    out.gpu = {
      name: nameBuf.toString('utf8').split('\0')[0],
      load: util.gpu,
      temp: temp[0],
      power: +(pw[0] / 1000).toFixed(1),
      clock: clk[0],
      vramUsedMB: Math.round(Number(mem.used) / 1048576),
      vramTotalMB: Math.round(Number(mem.total) / 1048576),
    }
    console.log('NVML_TEST_OK ' + JSON.stringify(out))
  } catch (e) {
    out.error = (e && e.message) + '\n' + (e && e.stack)
    console.log('NVML_TEST_FAIL ' + JSON.stringify(out))
  }
  app.quit()
})

app.on('window-all-closed', () => app.quit())
