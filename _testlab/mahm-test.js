// Liest MSI Afterburners Hardware-Monitoring-Shared-Memory ("MAHMSharedMemory").
// Beweist, ob wir daraus CPU-Temp/Takt/Power (und mehr) treiberfrei bekommen –
// analog zum RTSS-FPS-Leser. MSI Afterburner muss laufen.
//   ./node_modules/.bin/electron _testlab/mahm-test.js
const { app } = require('electron')

app.whenReady().then(() => {
  const out = { koffi: false, opened: false, signature: null, version: null, numEntries: 0, entrySize: 0, aggregates: [], error: null }
  try {
    const koffi = require('koffi')
    out.koffi = 'v' + (koffi.version || '?')
    const k32 = koffi.load('kernel32.dll')
    const OpenFileMappingA = k32.func('void* OpenFileMappingA(uint32 access, int inherit, str name)')
    const MapViewOfFile = k32.func('void* MapViewOfFile(void* h, uint32 access, uint32 offHi, uint32 offLo, size_t bytes)')
    const UnmapViewOfFile = k32.func('int UnmapViewOfFile(void* p)')
    const CloseHandle = k32.func('int CloseHandle(void* h)')
    // Kopf: dwSignature, dwVersion, dwHeaderSize, dwNumEntries, dwEntrySize (dann time/gpu – egal)
    const HDR = koffi.struct({ sig: 'uint32', ver: 'uint32', headerSize: 'uint32', numEntries: 'uint32', entrySize: 'uint32' })

    const h = OpenFileMappingA(0x0004 /*FILE_MAP_READ*/, 0, 'MAHMSharedMemory')
    if (!h) { out.error = 'OpenFileMapping=0 (laeuft MSI Afterburner?)'; throw new Error(out.error) }
    out.opened = true
    const base = MapViewOfFile(h, 0x0004, 0, 0, 0)
    if (!base) { out.error = 'MapViewOfFile=0'; throw new Error(out.error) }
    try {
      const hdr = koffi.decode(base, HDR)
      out.signature = '0x' + hdr.sig.toString(16)   // 'MAHM' = 0x4d41484d
      out.version = '0x' + hdr.ver.toString(16)
      out.numEntries = hdr.numEntries
      out.entrySize = hdr.entrySize
      // Jeder Eintrag: 5x char[260] (Name, Einheit, lokalisiert x2, Format), dann float data
      const readStr = (off) => {
        const bytes = koffi.decode(base, off, koffi.array('uint8', 96))
        const b = Buffer.from(bytes); const z = b.indexOf(0)
        return b.toString('latin1', 0, z < 0 ? b.length : z)
      }
      for (let i = 0; i < hdr.numEntries; i++) {
        const eOff = hdr.headerSize + i * hdr.entrySize
        const name = readStr(eOff)
        const units = readStr(eOff + 260)
        const data = koffi.decode(base, eOff + 5 * 260, 'float')
        // Nur Aggregat-Sensoren (keine Kern-Ziffer wie "CPU1 ...") -> GPU + CPU-Summen
        if (!/\d/.test(name)) out.aggregates.push({ name, data: Math.round(data * 100) / 100, units })
      }
    } finally { UnmapViewOfFile(base); CloseHandle(h) }
    console.log('MAHM_TEST ' + JSON.stringify(out))
    app.quit()
  } catch (e) {
    if (!out.error) out.error = (e && e.message)
    console.log('MAHM_TEST ' + JSON.stringify(out))
    app.quit()
  }
})
app.on('window-all-closed', () => app.quit())
