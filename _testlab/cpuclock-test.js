// Prueft, ob sich der AKTUELLE CPU-Takt (Boost) treiberfrei lesen laesst:
// CallNtPowerInformation(ProcessorInformation) fuellt pro logischem Prozessor eine
// PROCESSOR_POWER_INFORMATION mit CurrentMhz/MaxMhz. Kein Admin noetig.
//   ./node_modules/.bin/electron _testlab/cpuclock-test.js
const { app } = require('electron')
const os = require('os')

app.whenReady().then(() => {
  const out = { koffi: false, status: null, nproc: 0, maxMhz: 0, samples: [], error: null }
  try {
    const koffi = require('koffi')
    out.koffi = 'v' + (koffi.version || '?')
    const powrprof = koffi.load('powrprof.dll')
    // NTSTATUS CallNtPowerInformation(LEVEL, PVOID in, ULONG inLen, PVOID out, ULONG outLen)
    const CallNtPowerInformation = powrprof.func('long __stdcall CallNtPowerInformation(int level, void* inBuf, uint32 inLen, void* outBuf, uint32 outLen)')
    // struct PROCESSOR_POWER_INFORMATION { ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState }
    const PPI = koffi.struct('PROCESSOR_POWER_INFORMATION', {
      Number: 'uint32', MaxMhz: 'uint32', CurrentMhz: 'uint32', MhzLimit: 'uint32', MaxIdleState: 'uint32', CurrentIdleState: 'uint32'
    })
    const nproc = os.cpus().length
    out.nproc = nproc
    const arr = koffi.array(PPI, nproc)
    const ProcessorInformation = 11

    const read = () => {
      const buf = Buffer.alloc(koffi.sizeof(PPI) * nproc)
      const st = CallNtPowerInformation(ProcessorInformation, null, 0, buf, buf.length)
      const rows = koffi.decode(buf, arr)
      let maxCur = 0
      for (const r of rows) { if (r.CurrentMhz > maxCur) maxCur = r.CurrentMhz; if (r.MaxMhz > out.maxMhz) out.maxMhz = r.MaxMhz }
      return { st, maxCur }
    }

    // Phase 1: OHNE Last (idle). Phase 2: MIT Last. Bewegt sich CurrentMhz?
    const burn = () => { let x = 0; for (let i = 0; i < 8e7; i++) x += Math.sqrt(i); return x }
    out.idle = []; out.load = []
    let n = 0
    const t = setInterval(() => {
      const phaseLoad = n >= 4
      if (phaseLoad) burn()
      const { st, maxCur } = read()
      out.status = st
      ;(phaseLoad ? out.load : out.idle).push(maxCur)
      if (++n >= 8) {
        clearInterval(t)
        delete out.samples
        console.log('CPUCLOCK_TEST ' + JSON.stringify(out))
        app.quit()
      }
    }, 500)
  } catch (e) {
    out.error = (e && e.message) + '\n' + (e && e.stack)
    console.log('CPUCLOCK_TEST ' + JSON.stringify(out))
    app.quit()
  }
})
app.on('window-all-closed', () => app.quit())
