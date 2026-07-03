// Headless-Test: liest den RTSS-Shared-Memory und zeigt PRO App genau die Werte,
// die mein Filter benutzt: dwTime1, GetTickCount(now), delta=(now-dwTime1),
// dwFrameTime, dwFlags. So sehe ich, ob mein Stale-Filter (>1500ms) das Spiel
// faelschlich rauswirft.  ->  ./node_modules/.bin/electron _testlab/rtss-test.js
const { app } = require('electron')

app.whenReady().then(() => {
  const out = { mapping: null, now: null, apps: [], error: null }
  try {
    const koffi = require('koffi')
    const k32 = koffi.load('kernel32.dll')
    const OpenFileMappingA = k32.func('void* OpenFileMappingA(uint32 a, int i, str n)')
    const MapViewOfFile = k32.func('void* MapViewOfFile(void* h, uint32 a, uint32 hi, uint32 lo, size_t b)')
    const UnmapViewOfFile = k32.func('int UnmapViewOfFile(void* p)')
    const CloseHandle = k32.func('int CloseHandle(void* h)')
    const GetTickCount = k32.func('uint32 GetTickCount()')

    const h = OpenFileMappingA(0x0004, 0, 'RTSSSharedMemoryV2')
    out.mapping = h ? 'RTSSSharedMemoryV2' : 'NICHT gefunden'
    if (!h) throw new Error('OpenFileMapping fehlgeschlagen')
    const base = MapViewOfFile(h, 0x0004, 0, 0, 0)

    const HDR = koffi.struct({ dwSignature: 'uint32', dwVersion: 'uint32', dwAppEntrySize: 'uint32', dwAppArrOffset: 'uint32', dwAppArrSize: 'uint32', dwOSDEntrySize: 'uint32', dwOSDArrOffset: 'uint32', dwOSDArrSize: 'uint32', dwOSDFrame: 'uint32' })
    const APP = koffi.struct({ dwProcessID: 'uint32', szName: koffi.array('uint8', 260), dwFlags: 'uint32', dwTime0: 'uint32', dwTime1: 'uint32', dwFrames: 'uint32', dwFrameTime: 'uint32' })
    const hdr = koffi.decode(base, HDR)
    const now = GetTickCount()
    out.now = now
    for (let i = 0; i < hdr.dwAppArrSize; i++) {
      const e = koffi.decode(base, hdr.dwAppArrOffset + i * hdr.dwAppEntrySize, APP)
      if (!e.dwProcessID) continue
      let nm = ''; for (const b of e.szName) { if (!b) break; nm += String.fromCharCode(b) }
      out.apps.push({
        name: nm.split('\\').pop(), pid: e.dwProcessID,
        dwTime1: e.dwTime1, delta: ((now - e.dwTime1) >>> 0), dwFrameTime: e.dwFrameTime,
        flags: '0x' + (e.dwFlags >>> 0).toString(16),
        wouldPass: (e.dwFrameTime > 0 && (((now - e.dwTime1) >>> 0) <= 1500)),
      })
    }
    UnmapViewOfFile(base); CloseHandle(h)
    console.log('RTSS_TEST_OK ' + JSON.stringify(out))
  } catch (e) {
    out.error = (e && e.message)
    console.log('RTSS_TEST_FAIL ' + JSON.stringify(out))
  }
  app.quit()
})
app.on('window-all-closed', () => app.quit())
