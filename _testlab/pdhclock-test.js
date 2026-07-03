// Verifiziert den treiberfreien Live-CPU-Takt via PDH-Performance-Counter:
// "% Processor Performance" (kann >100 = Boost) x Basistakt = aktueller Takt.
// PdhAddEnglishCounter -> sprachunabhaengig (auch auf deutschem Windows).
//   ./node_modules/.bin/electron _testlab/pdhclock-test.js
const { app } = require('electron')
const os = require('os')

app.whenReady().then(() => {
  const out = { koffi: false, baseSpeed: (os.cpus()[0] || {}).speed, open: null, add: null, idlePct: null, loadPct: null, idleMhz: null, loadMhz: null, error: null }
  try {
    const koffi = require('koffi')
    out.koffi = 'v' + (koffi.version || '?')
    const pdh = koffi.load('pdh.dll')
    const PdhOpenQueryA = pdh.func('long PdhOpenQueryA(str src, uintptr user, _Out_ void** q)')
    const PdhAddEnglishCounterA = pdh.func('long PdhAddEnglishCounterA(void* q, str path, uintptr user, _Out_ void** c)')
    const PdhCollectQueryData = pdh.func('long PdhCollectQueryData(void* q)')
    koffi.struct('PDH_FMT_COUNTERVALUE', { CStatus: 'uint32', _pad: 'uint32', doubleValue: 'double' })
    const PdhGetFormattedCounterValue = pdh.func('long PdhGetFormattedCounterValue(void* c, uint32 fmt, void* type, _Out_ PDH_FMT_COUNTERVALUE* v)')

    const q = [null]
    out.open = PdhOpenQueryA(null, 0, q)
    const c = [null]
    out.add = PdhAddEnglishCounterA(q[0], '\\Processor Information(_Total)\\% Processor Performance', 0, c)
    const PDH_FMT_DOUBLE = 0x00000200
    const readPct = () => { PdhCollectQueryData(q[0]); const v = {}; PdhGetFormattedCounterValue(c[0], PDH_FMT_DOUBLE, null, v); return v.doubleValue }

    PdhCollectQueryData(q[0])   // erstes Sample (Ratenzaehler braucht 2)
    const burn = () => { let x = 0; for (let i = 0; i < 8e7; i++) x += Math.sqrt(i); return x }

    setTimeout(() => {
      out.idlePct = Math.round(readPct() * 10) / 10
      out.idleMhz = Math.round(out.baseSpeed * out.idlePct / 100)
      const t0 = Date.now(); while (Date.now() - t0 < 900) burn()   // Last erzeugen
      out.loadPct = Math.round(readPct() * 10) / 10
      out.loadMhz = Math.round(out.baseSpeed * out.loadPct / 100)
      console.log('PDHCLOCK_TEST ' + JSON.stringify(out))
      app.quit()
    }, 1000)
  } catch (e) {
    out.error = (e && e.message) + '\n' + (e && e.stack)
    console.log('PDHCLOCK_TEST ' + JSON.stringify(out))
    app.quit()
  }
})
app.on('window-all-closed', () => app.quit())
