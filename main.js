const { app, BrowserWindow, ipcMain, dialog, shell, screen, Tray, Menu, nativeImage, globalShortcut } = require('electron')
const path = require('path')
const fs = require('fs')
const os = require('os')
const https = require('https')
const { exec, spawn, execSync } = require('child_process')
const crypto = require('crypto')
// Ausfallsicher: fehlt das Modul (z.B. bei einem schnellen Dev-Deploy ohne
// node_modules), bleibt autoUpdater null und die Update-Funktion ist einfach aus,
// statt die ganze App am Start abstuerzen zu lassen.
let autoUpdater = null
try { autoUpdater = require('electron-updater').autoUpdater } catch (e) { console.warn('electron-updater nicht verfügbar:', e.message) }

function httpsGet(url, extraHeaders) {
  return new Promise((resolve, reject) => {
    const headers = Object.assign({ 'User-Agent': 'Lumora/1.0' }, extraHeaders || {})
    https.get(url, { headers }, (res) => {
      if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
        return httpsGet(res.headers.location, extraHeaders).then(resolve).catch(reject)
      }
      let data = ''
      res.on('data', chunk => data += chunk)
      res.on('end', () => resolve({ status: res.statusCode, body: data }))
    }).on('error', reject)
  })
}

function httpsGetBinary(url) {
  return new Promise((resolve, reject) => {
    https.get(url, { headers: { 'User-Agent': 'Lumora/1.0' } }, (res) => {
      if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
        return httpsGetBinary(res.headers.location).then(resolve).catch(reject)
      }
      const chunks = []
      res.on('data', chunk => chunks.push(chunk))
      res.on('end', () => resolve({ status: res.statusCode, buffer: Buffer.concat(chunks) }))
    }).on('error', reject)
  })
}

let mainWindow
let lastHdrState = null
let hdrPollInterval = null
let hdrEnabledByLauncher = false
let mediaDir = null
let windowStateFile = null
let tray = null
let appSettingsFile = null
let gamesFile = null
let prefsFile = null
const appSettings = { autostart: false, startMinimized: false, minimizeToTray: false, steamGridDbKey: '', toggleHotkey: 'Alt+L', gamepadHotkey: [],
  // OSD-Overlay: sichtbar?, Skalierung (Zoomfaktor), Ecke (tl/tr/bl/br), Panel-Deckkraft, FPS-Messung
  osdEnabled: false, osdScale: 1, osdCorner: 'tl', osdOpacity: 0.55, osdFps: false, osdFpsSource: 'auto', osdHotkey: 'Alt+O',
  // OSD-Aussehen: Theme + welche Werte je Gruppe angezeigt werden + Akzentfarbe
  osdTheme: 'compact', osdAccent: '#74e857',
  osdFields: { gpu: ['load','temp','power','clock','vram'], cpu: ['load','temp','clock','power','ram'], fps: ['fps','frametime','graph'] } }
app.isQuitting = false

// Entfernt ein evtl. vorangestelltes UTF-8 BOM – sonst schlaegt JSON.parse fehl
// (z.B. wenn die Datei mal von einem anderen Werkzeug geschrieben wurde).
function stripBom(s) { return (s && s.charCodeAt(0) === 0xFEFF) ? s.slice(1) : s }

function loadAppSettings() {
  try {
    const parsed = JSON.parse(stripBom(fs.readFileSync(appSettingsFile, 'utf8')))
    if (parsed && typeof parsed === 'object') Object.assign(appSettings, parsed)
  } catch {}
}

function saveAppSettings() {
  try { fs.writeFileSync(appSettingsFile, JSON.stringify(appSettings)) } catch {}
}

function applyAutostart() {
  app.setLoginItemSettings({
    openAtLogin: !!appSettings.autostart,
    args: appSettings.startMinimized ? ['--minimized'] : [],
  })
}

function showMainWindow() {
  // Fenster weg/zerstoert (z.B. wurde geschlossen, App lief noch)? Neu aufbauen,
  // statt auf einem toten Objekt zu operieren ("Object has been destroyed").
  if (!mainWindow || mainWindow.isDestroyed()) { createWindow(); return }
  if (mainWindow.isMinimized()) mainWindow.restore()
  // Windows verweigert SetForegroundWindow, wenn der Aufruf aus dem Hintergrund
  // kommt (Hotkey/Tray) – das Fenster blinkt dann nur in der Taskleiste, statt
  // wirklich nach vorne zu kommen. Ein kurzzeitiges alwaysOnTop erzwingt das
  // echte Anheben; danach wieder zuruecknehmen, damit es kein Dauer-Overlay wird.
  mainWindow.setAlwaysOnTop(true)
  mainWindow.show()
  mainWindow.moveTop()
  mainWindow.focus()
  app.focus({ steal: true })
  mainWindow.flashFrame(false)
  setTimeout(() => {
    if (mainWindow && !mainWindow.isDestroyed()) mainWindow.setAlwaysOnTop(false)
  }, 150)
}

// Globaler Hotkey: holt Lumora nach vorne bzw. versteckt es wieder (Toggle).
function toggleMainWindow() {
  if (!mainWindow) return
  if (mainWindow.isVisible() && !mainWindow.isMinimized() && mainWindow.isFocused()) {
    if (appSettings.minimizeToTray) mainWindow.hide()
    else mainWindow.minimize()
  } else {
    showMainWindow()
  }
}

// --- Tastatur-Hotkeys --------------------------------------------------------
// globalShortcut wird von Vollbild-Spielen/erhoehten Rechten (UIPI) abgefangen.
// Deshalb pollen wir die Hotkeys fokusunabhaengig mit GetAsyncKeyState (siehe
// pollNativeHotkeys) – das greift auch im Spiel. khHotkeys ist die aktive Liste.
let khHotkeys = []   // [{ vk, alt, ctrl, shift, action, _down }]

function accelKeyToVk(k) {
  k = String(k || '').toLowerCase()
  if (/^[a-z]$/.test(k)) return k.toUpperCase().charCodeAt(0)
  if (/^[0-9]$/.test(k)) return k.charCodeAt(0)
  const fm = k.match(/^f(\d{1,2})$/); if (fm && +fm[1] >= 1 && +fm[1] <= 24) return 0x6F + +fm[1]
  return ({ space: 0x20, tab: 0x09, enter: 0x0d, return: 0x0d, esc: 0x1b, escape: 0x1b,
    up: 0x26, down: 0x28, left: 0x25, right: 0x27, home: 0x24, end: 0x23,
    insert: 0x2d, delete: 0x2e, pageup: 0x21, pagedown: 0x22, plus: 0xbb, '+': 0xbb })[k] || 0
}
function parseAccel(a) {
  if (!a) return null
  const parts = String(a).split('+').map(s => s.trim().toLowerCase()).filter(Boolean)
  const vk = accelKeyToVk(parts[parts.length - 1]); if (!vk) return null
  const mods = parts.slice(0, -1)
  return { vk, alt: mods.includes('alt'), ctrl: mods.some(m => /^(ctrl|control|commandorcontrol|cmdorctrl)$/.test(m)), shift: mods.includes('shift') }
}
function rebuildHotkeys() {
  khHotkeys = []
  const add = (accel, action) => { const p = parseAccel(accel); if (p) khHotkeys.push(Object.assign(p, { action })) }
  add(appSettings.toggleHotkey, toggleMainWindow)
  add(appSettings.osdHotkey || 'Alt+O', toggleOverlay)
  add('Alt+Shift+O', toggleOsdEdit)
}
// (Neu-)Registriert die Hotkeys. Ist der native Poll (GetAsyncKeyState) aktiv,
// uebernimmt der die Tastatur-Hotkeys – auch im Spiel. Nur wenn der nicht
// verfuegbar ist, faellt es auf globalShortcut zurueck (greift NICHT im Vollbild).
function registerToggleHotkey() {
  globalShortcut.unregisterAll()
  rebuildHotkeys()
  if (getAsyncKey) return true   // Poll erledigt die Tastatur-Hotkeys
  try { globalShortcut.register(appSettings.osdHotkey || 'Alt+O', toggleOverlay) } catch {}
  try { globalShortcut.register('Alt+Shift+O', toggleOsdEdit) } catch {}
  const acc = appSettings.toggleHotkey
  if (!acc) return true
  try { return globalShortcut.register(acc, toggleMainWindow) }
  catch { return false }
}

// --- OSD-Overlay (transparentes, klick-durchlaessiges Fenster ueber dem Spiel) ---
// Meilenstein 1: nur die Anzeige (Platzhalter-Werte in osd.html). Live-Daten
// folgen ab Meilenstein 2 ueber das 'osd-data'-Event an overlayWindow.
let overlayWindow = null

// --- NVIDIA-GPU-Sensoren via NVML (koffi) — treiberfrei, aus dem Grafiktreiber ---
let nvml = null
function setupNvml() {
  let koffi
  try { koffi = require('koffi') } catch { return }
  try {
    let lib = null
    for (const p of ['nvml.dll', 'C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll']) {
      try { lib = koffi.load(p); break } catch {}
    }
    if (!lib) throw new Error('nvml.dll nicht ladbar')
    koffi.pointer('nvmlDevice_t', koffi.opaque())
    koffi.struct('nvmlUtilization_t', { gpu: 'uint32', memory: 'uint32' })
    koffi.struct('nvmlMemory_t', { total: 'uint64', free: 'uint64', used: 'uint64' })
    const f = {
      init:   lib.func('int nvmlInit_v2()'),
      handle: lib.func('int nvmlDeviceGetHandleByIndex_v2(uint32 index, _Out_ nvmlDevice_t* device)'),
      name:   lib.func('int nvmlDeviceGetName(nvmlDevice_t device, _Out_ char* name, uint32 length)'),
      util:   lib.func('int nvmlDeviceGetUtilizationRates(nvmlDevice_t device, _Out_ nvmlUtilization_t* u)'),
      temp:   lib.func('int nvmlDeviceGetTemperature(nvmlDevice_t device, uint32 sensor, _Out_ uint32* t)'),
      power:  lib.func('int nvmlDeviceGetPowerUsage(nvmlDevice_t device, _Out_ uint32* mw)'),
      clock:  lib.func('int nvmlDeviceGetClockInfo(nvmlDevice_t device, uint32 type, _Out_ uint32* mhz)'),
      mem:    lib.func('int nvmlDeviceGetMemoryInfo(nvmlDevice_t device, _Out_ nvmlMemory_t* m)'),
    }
    if (f.init() !== 0) throw new Error('nvmlInit_v2 fehlgeschlagen')
    const dev = [null]
    if (f.handle(0, dev) !== 0) throw new Error('GetHandleByIndex fehlgeschlagen')
    const nameBuf = Buffer.alloc(96)
    f.name(dev[0], nameBuf, 96)
    const model = nameBuf.toString('utf8').split('\0')[0].replace(/^NVIDIA\s+/i, '').replace(/GeForce\s+/i, '').trim()
    nvml = { f, device: dev[0], model }
    console.log('[osd] NVML aktiv:', model)
  } catch (e) {
    console.log('[osd] NVML nicht verfügbar:', e && e.message)
    nvml = null
  }
}

// --- AMD-GPU-Sensoren via ADL — treiberfrei, aus dem AMD-Treiber (atiadlxx.dll) -
// Pendant zu NVML: laeuft nur, wenn ein AMD-Treiber installiert ist (sonst inert –
// atiadlxx.dll fehlt). Wir nehmen den modernen Weg ADL2_New_QueryPMLogData_Get,
// der alle Sensoren in ein Array {supported,value}[256] fuellt (Buffer geparst,
// robuster als koffi-Structs). Sensor-IDs laut ADL SDK.
let adl = null
// Einmal-Diagnose in eine EIGENE Datei (kein Laufzeit-Spam, unabhaengig von
// OSD_DEBUG): zeigt Adapter + welche Sensoren die AMD-GPU liefert. -> justieren.
function gpuDiag(msg) { try { fs.appendFileSync(path.join(app.getPath('temp'), 'lumora-gpu.log'), Date.now() + ' ' + msg + '\n') } catch {} }
const ADL_PM = { gfxclk: 1, tempEdge: 8, gfxActivity: 19, asicPower: 23, tempHotspot: 27, tempGfx: 28, gfxPower: 30 }
function setupAdl() {
  let koffi
  try { koffi = require('koffi') } catch { return }
  let lib
  try { lib = koffi.load('atiadlxx.dll') } catch { return }   // kein AMD-Treiber -> inert
  try {
    const malloc = koffi.load('msvcrt.dll').func('void* malloc(size_t size)')
    const MALLOC = koffi.proto('void* ADLMalloc(int size)')
    const mallocCb = koffi.register((s) => malloc(s), koffi.pointer(MALLOC))
    const create = lib.func('int ADL2_Main_Control_Create(void* cb, int adapters, _Out_ void** ctx)')
    const numGet = lib.func('int ADL2_Adapter_NumberOfAdapters_Get(void* ctx, _Out_ int* num)')
    const infoGet = lib.func('int ADL2_Adapter_AdapterInfo_Get(void* ctx, void* info, int size)')
    const queryGet = lib.func('int ADL2_New_QueryPMLogData_Get(void* ctx, int adapter, void* out)')
    const ctx = [null]
    if (create(mallocCb, 1, ctx) !== 0 || !ctx[0]) throw new Error('Main_Control_Create')
    const num = [0]
    if (numGet(ctx[0], num) !== 0 || !num[0]) throw new Error('NumberOfAdapters')
    const INFO = 1572
    const buf = Buffer.alloc(num[0] * INFO)
    if (infoGet(ctx[0], buf, buf.length) !== 0) throw new Error('AdapterInfo_Get')
    // AMD-VendorID: ADL liefert 1002 DEZIMAL (nicht den PCI-Hexwert 0x1002) – beide
    // zulassen, damit es ueber Treiber-/GPU-Varianten hinweg greift.
    const isAmd = (v) => v === 1002 || v === 0x1002
    let found = null; const seen = []; const amdIdxs = []
    for (let i = 0; i < num[0]; i++) {
      const o = i * INFO
      const vendor = buf.readInt32LE(o + 276), present = buf.readInt32LE(o + 792), idx = buf.readInt32LE(o + 4)
      const nz = buf.indexOf(0, o + 280); const name = buf.toString('latin1', o + 280, nz < 0 ? o + 280 : nz)
      seen.push('#' + idx + ' vendor=0x' + (vendor >>> 0).toString(16) + ' present=' + present + ' "' + name + '"')
      if (isAmd(vendor) && present) { if (!found) found = { idx, name }; if (!amdIdxs.includes(idx)) amdIdxs.push(idx) }
    }
    gpuDiag('[ADL] ' + num[0] + ' Adapter: ' + seen.join(' | '))
    adl = found ? { queryGet, ctx: ctx[0], idx: found.idx, name: found.name, _cb: mallocCb } : null
    if (adl) {
      // Diagnose + Auswahl: Sensoren jedes AMD-Adapter-Index dumpen und den ERSTEN
      // nehmen, der wirklich Werte liefert (bei iGPUs ist "present" nicht zwingend
      // der Adapter mit PMLog-Daten).
      let bestIdx = -1
      for (const ai of amdIdxs) {
        const dump = Buffer.alloc(2052)
        if (queryGet(ctx[0], ai, dump) === 0) {
          const parts = []
          for (let id = 0; id < 256; id++) { const o = 4 + id * 8; if (dump.readInt32LE(o)) parts.push(id + '=' + dump.readInt32LE(o + 4)) }
          gpuDiag('[ADL] idx=' + ai + ' Sensoren [id=value]: ' + (parts.join(' ') || '(keine supported)'))
          if (bestIdx < 0 && parts.length) bestIdx = ai
        } else gpuDiag('[ADL] idx=' + ai + ' QueryPMLogData fehlgeschlagen')
      }
      if (bestIdx >= 0) adl.idx = bestIdx
      gpuDiag('[ADL] aktiver Index fuer Auslesen: ' + adl.idx)
      console.log('[osd] ADL aktiv:', found.name)
    } else gpuDiag('[ADL] kein aktiver AMD-Adapter (VendorID 1002/0x1002) gefunden')
  } catch (e) { gpuDiag('[ADL] Setup-Fehler: ' + (e && e.message)); adl = null }
}
function readAdlGpu() {
  if (!adl) return null
  try {
    const buf = Buffer.alloc(2052)
    if (adl.queryGet(adl.ctx, adl.idx, buf) !== 0) return null
    const val = (id) => { const o = 4 + id * 8; return buf.readInt32LE(o) ? buf.readInt32LE(o + 4) : null }
    // Temp: Edge/Hotspot (dedizierte Radeon) – fehlen bei iGPUs, dort GFX-Temp (28).
    const temp = val(ADL_PM.tempEdge) ?? val(ADL_PM.tempHotspot) ?? val(ADL_PM.tempGfx)
    const clock = val(ADL_PM.gfxclk)
    const power = val(ADL_PM.gfxPower) ?? val(ADL_PM.asicPower)
    const load = val(ADL_PM.gfxActivity)
    if (temp == null && clock == null && power == null && load == null) return null
    const u = (x) => x == null ? undefined : x
    const nm = (adl.name || gpuName || 'GPU').replace(/\((R|TM)\)/gi, '').replace(/^AMD\s+/i, '').replace(/\s+Graphics$/i, '').trim()
    return { brand: 'AMD', name: nm, load: u(load), temp: u(temp), power: u(power), clock: u(clock), vram: undefined }
  } catch { return null }
}

function readGpu() {
  if (nvml) {
    try {
      const { f, device } = nvml
      const u = {}, m = {}, t = [0], p = [0], c = [0]
      f.util(device, u); f.temp(device, 0, t); f.power(device, p); f.clock(device, 0, c); f.mem(device, m)
      return { brand: 'NVIDIA', name: nvml.model, load: u.gpu, temp: t[0], power: +(p[0] / 1000).toFixed(1), clock: c[0], vram: Math.round(Number(m.used) / 1048576) }
    } catch { return null }
  }
  // Keine NVIDIA: erst ADL (treiberfrei, immer bei AMD), sonst Afterburner (MAHM).
  const a = readAdlGpu()
  if (a) return a
  const m = readMahm()
  if (m && m.gpuTemp != null) {
    return { brand: 'GPU', name: gpuName, load: m.gpuLoad, temp: m.gpuTemp, power: m.gpuPower, clock: m.gpuClock, vram: m.gpuVram }
  }
  return null
}
// GPU-Name treiberfrei einmalig via WMI (fuer Nicht-NVIDIA; NVIDIA nutzt NVML).
let gpuName = null
function setupGpuName() {
  try {
    require('child_process').exec(
      'powershell -NoProfile -Command "(Get-CimInstance Win32_VideoController | Where-Object { $_.Name -notmatch \'Microsoft|Basic|Remote\' } | Select-Object -First 1 -ExpandProperty Name)"',
      { windowsHide: true, timeout: 5000 },
      (err, stdout) => {
        if (!err && stdout && stdout.trim()) {
          gpuName = stdout.trim().replace(/\((R|TM|C)\)/gi, '').replace(/^AMD\s+Radeon\s*/i, 'RADEON ').replace(/^AMD\s+/i, '').replace(/\s+Graphics$/i, '').trim() || null
        }
      })
  } catch {}
}

// --- CPU-Auslastung + RAM — treiberfrei ueber Node/Windows-Bordmittel ---------
// Last/RAM immer treiberfrei. Temp/Takt/Power: Takt geht treiberfrei (PDH), Temp
// und Power brauchen ein Sensor-Tool -> wir lesen sie aus Afterburner (MAHM),
// falls aktiv; sonst bleiben sie null und werden im OSD ausgeblendet.
let prevCpu = null
function cpuLoadPercent() {
  const cpus = os.cpus()
  let idle = 0, total = 0
  for (const c of cpus) { for (const k in c.times) total += c.times[k]; idle += c.times.idle }
  if (!prevCpu) { prevCpu = { idle, total }; return 0 }
  const di = idle - prevCpu.idle, dt = total - prevCpu.total
  prevCpu = { idle, total }
  return dt > 0 ? Math.max(0, Math.min(100, Math.round((1 - di / dt) * 100))) : 0
}
function readCpu() {
  const raw = (os.cpus()[0] && os.cpus()[0].model) || 'CPU'
  const brand = /intel/i.test(raw) ? 'INTEL' : /amd|ryzen/i.test(raw) ? 'AMD' : 'CPU'
  const model = raw.replace(/\((R|TM)\)/gi, '').replace(/^AMD\s+/i, '').replace(/^Intel\s+/i, '')
    .replace(/\s+w\/.*$/i, '').replace(/\s+with\s+.*$/i, '')   // "w/ Radeon 780M Graphics" weg (GPU hat eigenen Block)
    .replace(/\s+\d+-Core Processor.*$/i, '').replace(/\s+CPU.*$/i, '').trim()
  // Temp/Takt/Power aus Afterburner (falls aktiv). Ohne Afterburner nur der Takt
  // treiberfrei (PDH); Temp/Power bleiben null -> im OSD ausgeblendet.
  const m = readMahm()
  let temp = null, clock = null, power = null
  if (m && m.cpuTemp != null) { temp = m.cpuTemp; clock = m.cpuClock; power = m.cpuPower }
  else { clock = readCpuClockMhz() }
  return {
    brand,
    name: model,
    load: cpuLoadPercent(),
    ram: Math.round((os.totalmem() - os.freemem()) / 1048576),
    temp, clock, power,
  }
}

// --- FPS/Frametime via PresentMon (ETW, keine Injection) ----------------------
// PresentMon schreibt CSV in eine Temp-Datei; wir "tailen" sie und rechnen daraus
// FPS/Frametime des gerade aktivsten (praesentierenden) Spiels. PresentMon braucht
// Adminrechte -> wird elevated gestartet (ein UAC-Dialog). Faellt weg/scheitert,
// bleiben FPS einfach auf "—".
// --- FPS-Quelle 1 (bevorzugt): RivaTuner Statistics Server (RTSS) -------------
// Laeuft RTSS/Afterburner ohnehin, lesen wir FPS/Frametime direkt aus dessen
// Shared Memory – kein Admin, kein UAC, kein ETW-Konflikt.
let rtss = null
function setupRtss() {
  let koffi
  try { koffi = require('koffi') } catch { return }
  try {
    const k32 = koffi.load('kernel32.dll')
    rtss = {
      koffi,
      open: k32.func('void* OpenFileMappingA(uint32 access, int inherit, str name)'),
      map: k32.func('void* MapViewOfFile(void* h, uint32 access, uint32 offHi, uint32 offLo, size_t bytes)'),
      unmap: k32.func('int UnmapViewOfFile(void* p)'),
      close: k32.func('int CloseHandle(void* h)'),
      ticks: k32.func('uint32 GetTickCount()'),
      HDR: koffi.struct({ dwSignature: 'uint32', dwVersion: 'uint32', dwAppEntrySize: 'uint32', dwAppArrOffset: 'uint32', dwAppArrSize: 'uint32', dwOSDEntrySize: 'uint32', dwOSDArrOffset: 'uint32', dwOSDArrSize: 'uint32', dwOSDFrame: 'uint32' }),
      APP: koffi.struct({ dwProcessID: 'uint32', szName: koffi.array('uint8', 260), dwFlags: 'uint32', dwTime0: 'uint32', dwTime1: 'uint32', dwFrames: 'uint32', dwFrameTime: 'uint32' }),
      base: 0, handle: 0,   // dauerhaft offene Zuordnung fuer schnelle Reads
    }
    console.log('[fps] RTSS-Leser bereit')
  } catch (e) { console.log('[fps] RTSS-Setup fehlgeschlagen:', e && e.message); rtss = null }
}
function rtssApi(flags) {
  return ({ 1: 'OpenGL', 2: 'DirectDraw', 3: 'D3D8', 4: 'D3D9', 5: 'D3D9', 6: 'D3D10', 7: 'D3D11', 8: 'D3D12', 9: 'D3D12', 10: 'Vulkan' })[flags & 0xffff] || null
}
function rtssAvailable() {
  if (!rtss) return false
  const h = rtss.open(0x0004, 0, 'RTSSSharedMemoryV2')
  if (h) { try { rtss.close(h) } catch {} return true }
  return false
}
// Liest den zuletzt aktualisierten, aktiv praesentierenden App-Eintrag.
// Pro Aufruf oeffnen/schliessen – bewaehrt und robust (koffi mag wiederverwendete
// Map-Zeiger nicht). Bei 30 Hz ist der Overhead vernachlaessigbar.
function readRtssFps() {
  if (!rtss) return null
  const h = rtss.open(0x0004, 0, 'RTSSSharedMemoryV2')
  if (!h) return null
  let base = 0
  try {
    base = rtss.map(h, 0x0004, 0, 0, 0)
    if (!base) return null
    const hdr = rtss.koffi.decode(base, rtss.HDR)
    const now = rtss.ticks()
    let best = null
    const n = Math.min(hdr.dwAppArrSize, 4096)
    for (let i = 0; i < n; i++) {
      const e = rtss.koffi.decode(base, hdr.dwAppArrOffset + i * hdr.dwAppEntrySize, rtss.APP)
      if (!e.dwProcessID || !e.dwFrameTime) continue
      if (((now - e.dwTime1) >>> 0) > 1500) continue   // veralteter Eintrag (Spiel praesentiert nicht mehr)
      if (!best || e.dwTime1 > best.dwTime1) best = e
    }
    if (!best) return null
    const dt = best.dwTime1 - best.dwTime0
    return {
      fps: dt > 0 ? Math.round(best.dwFrames * 1000 / dt) : Math.round(1000000 / best.dwFrameTime),
      frametime: +(best.dwFrameTime / 1000).toFixed(1),   // momentane Frametime -> lebendiger Graph
      api: rtssApi(best.dwFlags),
    }
  } catch { return null }
  finally {
    if (base) { try { rtss.unmap(base) } catch {} }
    try { rtss.close(h) } catch {}
  }
}

// --- CPU-Temp/Takt/Power aus MSI Afterburner ("MAHMSharedMemory") --------------
// Treiberfrei bei uns: laeuft Afterburner, schreibt es alle Sensoren in ein Shared
// Memory. Wir lesen die Aggregat-Eintraege "CPU temperature/clock/power" – exakt
// wie der RTSS-FPS-Leser (pro Aufruf open/map/decode/unmap; koffi mag keine
// dauerhaft wiederverwendeten Map-Zeiger). Laeuft Afterburner nicht -> null.
let mahm = null
let mahmOffsets = null         // { numEntries, <key>: dataOffset } – einmal gesucht
let mahmVal, mahmValAt = 0     // Ergebnis-Cache: ein echter Read pro OSD-Tick (readCpu+readGpu teilen ihn)
const MAHM_SIG = 0x4D41484D    // 'MAHM'
// Gesuchte Aggregat-Sensoren (Afterburner-Namen) -> interner Schluessel
const MAHM_SENSORS = {
  cpuTemp: 'CPU temperature', cpuClock: 'CPU clock', cpuPower: 'CPU power',
  gpuTemp: 'GPU temperature', gpuClock: 'Core clock', gpuPower: 'Power',
  gpuLoad: 'GPU usage', gpuVram: 'Memory usage',
}
function setupMahm() {
  let koffi
  try { koffi = require('koffi') } catch { return }
  try {
    const k32 = koffi.load('kernel32.dll')
    mahm = {
      koffi,
      open: k32.func('void* OpenFileMappingA(uint32 access, int inherit, str name)'),
      map: k32.func('void* MapViewOfFile(void* h, uint32 access, uint32 offHi, uint32 offLo, size_t bytes)'),
      unmap: k32.func('int UnmapViewOfFile(void* p)'),
      close: k32.func('int CloseHandle(void* h)'),
      HDR: koffi.struct({ sig: 'uint32', ver: 'uint32', headerSize: 'uint32', numEntries: 'uint32', entrySize: 'uint32' }),
      NAME: koffi.array('uint8', 32),
    }
    console.log('[osd] MAHM-Leser bereit (CPU-Werte aus Afterburner, falls aktiv)')
  } catch (e) { console.log('[osd] MAHM-Setup fehlgeschlagen:', e && e.message); mahm = null }
}
function readMahmRaw() {
  if (!mahm) return null
  const h = mahm.open(0x0004 /*FILE_MAP_READ*/, 0, 'MAHMSharedMemory')
  if (!h) { mahmOffsets = null; return null }   // Afterburner laeuft nicht
  let base = 0
  try {
    base = mahm.map(h, 0x0004, 0, 0, 0)
    if (!base) return null
    const hdr = mahm.koffi.decode(base, mahm.HDR)
    if (hdr.sig !== MAHM_SIG || !hdr.numEntries) return null
    // Die gesuchten Aggregat-Sensoren einmal suchen und ihre data-Offsets cachen
    // (aendern sich waehrend einer Afterburner-Sitzung nicht; neu bei geaenderter
    // Eintragszahl). Eintrag: 5x char[260], dann float data (Offset 5*260).
    if (!mahmOffsets || mahmOffsets.numEntries !== hdr.numEntries) {
      const off = { numEntries: hdr.numEntries }
      const wanted = new Map(Object.entries(MAHM_SENSORS).map(([k, n]) => [n, k]))
      for (let i = 0; i < hdr.numEntries; i++) {
        const eOff = hdr.headerSize + i * hdr.entrySize
        const nb = Buffer.from(mahm.koffi.decode(base, eOff, mahm.NAME))
        const z = nb.indexOf(0); const name = nb.toString('latin1', 0, z < 0 ? nb.length : z)
        const key = wanted.get(name)
        if (key) off[key] = eOff + 5 * 260
      }
      mahmOffsets = off
    }
    const f = (k) => mahmOffsets[k] ? Math.round(mahm.koffi.decode(base, mahmOffsets[k], 'float')) : null
    return {
      cpuTemp: f('cpuTemp'), cpuClock: f('cpuClock'), cpuPower: f('cpuPower'),
      gpuTemp: f('gpuTemp'), gpuClock: f('gpuClock'), gpuPower: f('gpuPower'),
      gpuLoad: f('gpuLoad'), gpuVram: f('gpuVram'),
    }
  } catch { return null }
  finally {
    if (base) { try { mahm.unmap(base) } catch {} }
    try { mahm.close(h) } catch {}
  }
}
// Ein echter Shared-Memory-Read pro OSD-Tick; readCpu und readGpu teilen sich das
// Ergebnis (sie werden im selben tick() direkt nacheinander aufgerufen).
function readMahm() {
  const now = Date.now()
  if (mahmVal !== undefined && (now - mahmValAt) < 150) return mahmVal
  mahmVal = readMahmRaw(); mahmValAt = now
  return mahmVal
}

// --- CPU-Takt treiberfrei via PDH-Performance-Counter -------------------------
// Fallback ohne Afterburner: "% Processor Performance" (kann >100 = Boost) x
// Basistakt = aktueller Takt – genau wie der Windows-Task-Manager. PdhAddEnglish-
// Counter -> sprachunabhaengig. Temp/Power gibt es so NICHT (nur mit Sensor-Tool).
let cpuClock = null
function setupCpuClock() {
  let koffi
  try { koffi = require('koffi') } catch { return }
  try {
    const pdh = koffi.load('pdh.dll')
    const openQ = pdh.func('long PdhOpenQueryA(str src, uintptr user, _Out_ void** q)')
    const addC = pdh.func('long PdhAddEnglishCounterA(void* q, str path, uintptr user, _Out_ void** c)')
    const collect = pdh.func('long PdhCollectQueryData(void* q)')
    koffi.struct('PDH_FMT_COUNTERVALUE', { CStatus: 'uint32', _pad: 'uint32', doubleValue: 'double' })
    const getVal = pdh.func('long PdhGetFormattedCounterValue(void* c, uint32 fmt, void* type, _Out_ PDH_FMT_COUNTERVALUE* v)')
    const q = [null]
    if (openQ(null, 0, q) !== 0) throw new Error('PdhOpenQuery')
    const c = [null]
    if (addC(q[0], '\\Processor Information(_Total)\\% Processor Performance', 0, c) !== 0) throw new Error('PdhAddEnglishCounter')
    collect(q[0])   // erstes Sample (Ratenzaehler braucht zwei)
    cpuClock = { collect, getVal, q: q[0], c: c[0] }
    console.log('[osd] PDH-Takt-Leser bereit (treiberfreier Boost-Takt)')
  } catch (e) { console.log('[osd] PDH-Takt nicht verfuegbar:', e && e.message); cpuClock = null }
}
function readCpuClockMhz() {
  if (!cpuClock) return null
  try {
    cpuClock.collect(cpuClock.q)
    const v = {}
    if (cpuClock.getVal(cpuClock.c, 0x00000200 /*PDH_FMT_DOUBLE*/, null, v) !== 0) return null
    const base = (os.cpus()[0] || {}).speed || 0
    const mhz = Math.round(base * v.doubleValue / 100)
    return mhz > 0 ? mhz : null
  } catch { return null }
}

function getPresentMonPath() {
  return app.isPackaged ? path.join(process.resourcesPath, 'PresentMon.exe') : path.join(__dirname, 'PresentMon.exe')
}

// --- Shared Memory zwischen elevated Broker und normaler App ------------------
// Der Broker (Lumora.exe --fps-broker, via geplanter Aufgabe mit hoechsten
// Rechten) misst FPS und schreibt sie hier hinein; die App liest sie – ganz
// ohne UAC/Abmelden im Betrieb. Heartbeat (appTick/wanted) beendet den Broker
// sauber, sobald die App ihn nicht mehr braucht.
const FPS_TASK = 'LumoraOSD-FPS'
const SHM_NAME = 'Local\\LumoraOSDFps'
const SHM_MAGIC = 0x4C4F5344   // 'LOSD'
// Diagnose-Log fuer den OSD-/Broker-/FPS-Pfad. Standardmaessig AUS (kein Spam im
// Normalbetrieb). Zum Einschalten die Flag-Datei anlegen und Lumora neu starten:
//   type nul > "%TEMP%\lumora-osd-debug.on"
// Beim Start EINMAL geprueft; gilt fuer App UND (separaten) Broker-Prozess, da
// beide dasselbe %TEMP% nutzen. Danach schreiben alle osdDbg()-Aufrufe nach
// %TEMP%\lumora-osd.log; ist die Flag-Datei aus, kostet osdDbg praktisch nichts.
const OSD_DEBUG = (() => { try { return fs.existsSync(path.join(app.getPath('temp'), 'lumora-osd-debug.on')) } catch { return false } })()
function osdDbg(msg) { if (!OSD_DEBUG) return; try { fs.appendFileSync(path.join(app.getPath('temp'), 'lumora-osd.log'), Date.now() + ' ' + msg + '\n') } catch {} }
let shm = null
let shmStructs = null   // koffi-Structs nur EINMAL definieren (sonst wirft koffi bei Wiederholung)
function shmemInit() {
  if (shm) return true
  let koffi
  try { koffi = require('koffi') } catch (e) { osdDbg('shmemInit: koffi fehlt ' + (e && e.message)); return false }
  try {
    const k32 = koffi.load('kernel32.dll')
    const CreateFileMappingA = k32.func('void* CreateFileMappingA(void* hFile, void* sec, uint32 protect, uint32 maxHi, uint32 maxLo, str name)')
    if (!shmStructs) {
      // Getrennte Schreibbereiche: Broker @0 (magic..pid), App @24 (appTick,wanted).
      shmStructs = {
        FULL: koffi.struct('LumoraFpsFull', { magic: 'uint32', brokerTick: 'uint32', fps: 'uint32', frametimeX100: 'uint32', apiCode: 'uint32', pid: 'uint32', appTick: 'uint32', wanted: 'uint32' }),
        BROKER: koffi.struct('LumoraFpsBroker', { magic: 'uint32', brokerTick: 'uint32', fps: 'uint32', frametimeX100: 'uint32', apiCode: 'uint32', pid: 'uint32' }),
        APP: koffi.struct('LumoraFpsApp', { appTick: 'uint32', wanted: 'uint32' }),
      }
    }
    // Handle bleibt dauerhaft offen (haelt die Section am Leben). ABER: gemappt wird
    // PRO Zugriff frisch (map/decode|encode/unmap) – exakt wie beim RTSS-Leser. Ein
    // dauerhaft wiederverwendeter Map-Zeiger liefert mit koffi PROZESSUEBERGREIFEND
    // inkohaerente Snapshots (magic=0 trotz fps>0) – genau der Fehler, den wir beim
    // RTSS-Pfad schon hatten. Frisches Mapping je Aufruf behebt das.
    const h = CreateFileMappingA(koffi.as(-1, 'void*'), null, 0x04 /*PAGE_READWRITE*/, 0, 64, SHM_NAME)
    if (!h) { osdDbg('shmemInit: CreateFileMapping=0'); return false }
    shm = {
      koffi, handle: h, FULL: shmStructs.FULL, BROKER: shmStructs.BROKER, APP: shmStructs.APP,
      map: k32.func('void* MapViewOfFile(void* h, uint32 access, uint32 offHi, uint32 offLo, size_t bytes)'),
      unmap: k32.func('int UnmapViewOfFile(void* p)'),
      ticks: k32.func('uint32 GetTickCount()'),
    }
    osdDbg('shmemInit OK (pid=' + process.pid + ')')
    return true
  } catch (e) { osdDbg('shmemInit throw: ' + (e && e.message)); return false }
}
function shmReadFull() {
  if (!shm) return null
  const base = shm.map(shm.handle, 0xF001F /*FILE_MAP_ALL_ACCESS*/, 0, 0, 0)
  if (!base) return null
  try { return shm.koffi.decode(base, shm.FULL) }
  catch { return null }
  finally { try { shm.unmap(base) } catch {} }
}
function shmWriteBroker(o) {
  if (!shm) return
  const base = shm.map(shm.handle, 0xF001F, 0, 0, 0)
  if (!base) return
  try { shm.koffi.encode(base, 0, shm.BROKER, o) } catch {}
  finally { try { shm.unmap(base) } catch {} }
}
function shmWriteApp(appTick, wanted) {
  if (!shm) return
  const base = shm.map(shm.handle, 0xF001F, 0, 0, 0)
  if (!base) return
  try { shm.koffi.encode(base, 24, shm.APP, { appTick, wanted }) } catch {}
  finally { try { shm.unmap(base) } catch {} }
}

// Broker-Prozess: laeuft elevated, misst FPS via PresentMon und fuellt den
// Shared Memory. Beendet sich (und PresentMon) sauber, wenn die App weg ist.
function runFpsBroker() {
  if (!shmemInit()) { app.quit(); return }
  // Laeuft bereits ein anderer Broker (frischer brokerTick)? Dann SOFORT raus –
  // niemals PresentMon starten, sonst wuergt --stop_existing_session die laufende
  // ETW-Session ab (= genau die ~2s-Aussetzer). Es darf nur EIN Broker leben.
  const s0 = shmReadFull()
  if (s0 && s0.magic === SHM_MAGIC && s0.brokerTick && ((shm.ticks() - s0.brokerTick) >>> 0) < 2500) {
    osdDbg('[broker] anderer Broker bereits aktiv – beende mich sofort'); app.quit(); return
  }
  const frames = new Map()
  let partial = '', cols = null
  let rawLines = 0
  const seenApps = new Set()   // ALLE App-Namen, die PresentMon meldet (auch ignorierte)
  const exe = getPresentMonPath()
  const args = ['--output_stdout', '--session_name', 'LumoraOSD', '--stop_existing_session', '--no_console_stats', '--v1_metrics']
  let child = null
  try { child = spawn(exe, args, { windowsHide: true }) } catch { app.quit(); return }
  osdDbg('[broker] gestartet, PresentMon pid=' + child.pid)
  child.stderr.on('data', (d) => osdDbg('[broker] PM-stderr: ' + d.toString().trim()))

  child.stdout.on('data', (d) => {
    const lines = (partial + d.toString('utf8')).split(/\r?\n/)
    partial = lines.pop()
    for (const line of lines) {
      if (!line) continue
      const p = line.split(',')
      if (!cols) {
        const idx = (n) => p.findIndex(h => h.trim().toLowerCase() === n)
        cols = { app: idx('application'), pid: idx('processid'), t: idx('timeinseconds'), ft: idx('msbetweenpresents') }
        continue
      }
      const pid = p[cols.pid], appn = (p[cols.app] || '').trim()
      const t = parseFloat(p[cols.t]), ft = parseFloat(p[cols.ft])
      rawLines++
      if (appn && seenApps.size < 40) seenApps.add(appn.toLowerCase())
      if (!pid || isNaN(t) || isNaN(ft) || PM_IGNORE.has(appn.toLowerCase())) continue
      let e = frames.get(pid); if (!e) { e = { pid: Number(pid), times: [], ft: [] }; frames.set(pid, e) }
      e.times.push(t); e.ft.push(ft); if (e.times.length > 400) { e.times.shift(); e.ft.shift() }
    }
  })

  const startTick = shm.ticks()
  let lastFreshApp = shm.ticks()   // letzter Zeitpunkt mit frischem App-Heartbeat
  let brokerDbg = 0, brokerWriteDbg = 0
  const wtimer = setInterval(() => {
    const now = shm.ticks()
    const s = shmReadFull()
    if (Date.now() - brokerDbg > 2000) {   // Diagnose: liefert PresentMon ueberhaupt Daten?
      brokerDbg = Date.now()
      osdDbg('[broker] PM rawLines=' + rawLines + ' nichtIgnorierteProzesse=' + frames.size + ' apps=[' + [...seenApps].join(',') + ']')
    }
    // App lebt, solange appTick frisch ist. wanted wird bewusst IGNORIERT, weil es
    // beim Umschalten kurz auf 0 zucken kann – nur dauerhaftes Ausbleiben zaehlt.
    if (s && s.appTick && ((now - s.appTick) >>> 0) < 3000) lastFreshApp = now
    if (((now - startTick) >>> 0) > 8000 && ((now - lastFreshApp) >>> 0) > 5000) {
      osdDbg('[broker] Exit: kein frischer App-Heartbeat seit 5s (App zu / FPS aus)')
      cleanup(); return
    }
    // Aktivsten Praesentierer der letzten 0,5 s bestimmen (reaktiver als 1 s).
    const WIN = 0.5
    let best = null, bestC = 0, tmax = 0
    for (const e of frames.values()) if (e.times.length) tmax = Math.max(tmax, e.times[e.times.length - 1])
    for (const e of frames.values()) {
      let c = 0
      for (let i = e.times.length - 1; i >= 0 && e.times[i] >= tmax - WIN; i--) c++
      // fps = Frames im Fenster / Fensterlaenge (stabile Zahl), Frametime = letzte (momentan)
      if (c > bestC) { bestC = c; best = { pid: e.pid, fps: Math.round(c / WIN), ft: e.ft[e.ft.length - 1] } }
    }
    const outFps = best && bestC >= 2 ? best.fps : 0
    shmWriteBroker({
      magic: SHM_MAGIC, brokerTick: now,
      fps: outFps, frametimeX100: best && bestC >= 2 ? Math.round(best.ft * 100) : 0,
      apiCode: 0, pid: best ? best.pid : 0,
    })
    // Diagnose: Was RECHNET/SCHREIBT der Broker – und liest er den App-Heartbeat?
    if (Date.now() - brokerWriteDbg > 1000) {
      brokerWriteDbg = Date.now()
      osdDbg('[broker] schreibe fps=' + outFps + ' bestC=' + bestC + ' procs=' + frames.size + ' appTickGesehen=' + ((s && s.appTick) >>> 0) + ' brokerNow=' + now)
    }
  }, 16)   // ~60 Hz

  let cleaned = false
  function cleanup() { if (cleaned) return; cleaned = true; clearInterval(wtimer); try { child.kill() } catch {} app.quit() }
  child.on('exit', (code, sig) => { osdDbg('[broker] PresentMon EXIT code=' + code + ' sig=' + sig); cleanup() })
  app.on('window-all-closed', (e) => { e.preventDefault() })   // Broker hat kein Fenster
}
const PM_IGNORE = new Set(['dwm.exe', 'explorer.exe', 'lumora.exe', 'presentmon.exe', 'searchhost.exe',
  'textinputhost.exe', 'shellexperiencehost.exe', 'startmenuexperiencehost.exe', 'applicationframehost.exe'])
// --- App-Seite des Brokers ----------------------------------------------------
// Startet den elevated Broker per geplanter Aufgabe (kein UAC) und liest dessen
// FPS aus dem Shared Memory. Existiert die Aufgabe nicht -> Einrichtungs-Hinweis.
let brokerSpawnAt = 0
// Lebt gerade ein Broker? (Frischer brokerTick im Shared Memory.)
function brokerAlive() {
  if (!shm) return false
  const s = shmReadFull()
  return !!(s && s.magic === SHM_MAGIC && s.brokerTick && ((shm.ticks() - s.brokerTick) >>> 0) < 2500)
}
// Ist die geplante FPS-Aufgabe bereits registriert?
function fpsTaskPresent() {
  try { return require('child_process').spawnSync('schtasks', ['/query', '/tn', FPS_TASK], { windowsHide: true }).status === 0 }
  catch { return false }
}
// Legt die Aufgabe EINMAL elevated an (ein UAC-Dialog). Danach startet der Broker
// per schtasks /run ganz ohne weiteres UAC. Register-ScheduledTask laeuft in der
// interaktiven Nutzer-Session mit hoechsten Rechten -> selber "Local\"-Namespace
// fuers Shared Memory UND Adminrechte fuer PresentMon/ETW.
function createFpsTaskElevated() {
  const exe = process.execPath.replace(/'/g, "''")
  const ps = [
    `$a=New-ScheduledTaskAction -Execute '${exe}' -Argument '--fps-broker'`,
    `$p=New-ScheduledTaskPrincipal -UserId $env:USERNAME -RunLevel Highest -LogonType Interactive`,
    `$s=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew`,
    `Register-ScheduledTask -TaskName '${FPS_TASK}' -Action $a -Principal $p -Settings $s -Force`,
  ].join('; ')
  const b64 = Buffer.from(ps, 'utf16le').toString('base64')
  const outer = `Start-Process powershell -Verb RunAs -WindowStyle Hidden -ArgumentList '-NoProfile -EncodedCommand ${b64}'`
  try { spawn('powershell', ['-NoProfile', '-Command', outer], { windowsHide: true }) } catch (e) { osdDbg('createFpsTask: ' + (e && e.message)) }
}
function runFpsTask() {
  osdDbg('startBroker: schtasks /run ' + FPS_TASK)
  const p = spawn('schtasks', ['/run', '/tn', FPS_TASK], { windowsHide: true })
  let bad = false, errOut = ''
  p.stderr.on('data', (d) => { bad = true; errOut += d.toString() })
  p.on('error', (e) => { bad = true; errOut += (e && e.message) })
  p.on('exit', (code) => osdDbg('startBroker: schtasks exit code=' + code + ' bad=' + bad + ' ' + errOut.trim()))
}
let fpsTaskCreating = false
function startBroker() {
  if (!shmemInit()) { osdDbg('startBroker: shmemInit FAIL'); return }
  shmWriteApp(shm.ticks(), 1)                 // will FPS -> Broker soll laufen
  // Idempotent: nur starten, wenn KEIN Broker lebt UND wir nicht gerade eben
  // schon einen angestossen haben (er braucht ~1-2s bis zum ersten brokerTick).
  // Ohne das entsteht ein Spawn-Sturm -> mehrere PresentMon-Instanzen reissen
  // sich gegenseitig die ETW-Session weg -> ~2s-Aussetzer bei jedem Neustart.
  const now = Date.now()
  if (brokerAlive() || (now - brokerSpawnAt) < 4000) { osdDbg('startBroker: Broker laeuft/startet bereits – kein Neustart'); return }
  brokerSpawnAt = now
  // Aufgabe noch nicht da (frisches System)? Erst dem Nutzer ERKLAEREN, was gleich
  // passiert (sonst ist der Windows-UAC-Dialog voellig kontextlos), dann EINMAL
  // elevated anlegen (UAC), auf die Erstellung warten, starten. Ohne die Aufgabe
  // gaebe es nie FPS.
  if (!fpsTaskPresent()) {
    if (fpsTaskCreating) return
    fpsTaskCreating = true
    const parent = (mainWindow && !mainWindow.isDestroyed()) ? mainWindow : null
    dialog.showMessageBox(parent, {
      type: 'info', noLink: true,
      title: 'FPS-Anzeige einrichten',
      message: 'FPS-Anzeige einrichten',
      detail: 'Damit Lumora die Bildrate (FPS) im Spiel messen kann, richtet es einmalig einen kleinen Hintergrunddienst ein.\n\nGleich fragt Windows nach deiner Bestaetigung mit Administratorrechten (ein Dialog von "Windows PowerShell"). Das ist nur fuer diese einmalige Einrichtung noetig – danach nie wieder.',
      buttons: ['Einrichten', 'Abbrechen'], defaultId: 0, cancelId: 1,
    }).then((r) => {
      if (r.response !== 0) {                         // abgelehnt -> FPS aus, nicht staendig neu fragen
        fpsTaskCreating = false
        appSettings.osdFps = false; saveAppSettings(); stopFps()
        if (parent) parent.webContents.send('osd-fps-off')
        osdDbg('startBroker: Einrichtung abgelehnt -> FPS aus')
        return
      }
      osdDbg('startBroker: Aufgabe fehlt -> lege sie elevated an (UAC)')
      createFpsTaskElevated()
      let tries = 0
      const wait = setInterval(() => {
        if (fpsTaskPresent()) { clearInterval(wait); fpsTaskCreating = false; osdDbg('startBroker: Aufgabe angelegt'); runFpsTask() }
        else if (++tries >= 30) { clearInterval(wait); fpsTaskCreating = false; osdDbg('startBroker: Aufgabe nicht angelegt (UAC abgelehnt?)') }
      }, 1000)
    }).catch(() => { fpsTaskCreating = false })
    return
  }
  runFpsTask()
}
function stopBroker() {
  shmWriteApp(0, 0)                           // Broker beendet sich (+ PresentMon)
}
let lastGoodFps = null, lastGoodAt = 0, readDbgAt = 0
function readBrokerFps() {
  if (!shm) return null
  const now = shm.ticks()
  shmWriteApp(now, 1)                          // Heartbeat
  const s = shmReadFull()
  // Diagnose: liest die App ihren EIGENEN appTick@24 zurueck (Mapping ok?) und was
  // steht in magic@0/fps@8 (Broker-Bereich)? appEcho ~ now => App liest ihre eigene
  // Section korrekt; ist dann magic=0, schreiben App+Broker in GETRENNTE Sections.
  if (Date.now() - readDbgAt > 1000) {
    readDbgAt = Date.now()
    osdDbg('[app] read: magic=0x' + ((s && s.magic) >>> 0).toString(16) + ' brokerTick=' + ((s && s.brokerTick) >>> 0) + ' fps=' + (s && s.fps) + ' appEcho=' + ((s && s.appTick) >>> 0) + ' now=' + now + ' d=' + (s ? ((now - s.appTick) >>> 0) : '-'))
  }
  const brokerAge = s && s.brokerTick ? ((now - s.brokerTick) >>> 0) : 999999
  const fresh = s && s.magic === SHM_MAGIC && brokerAge <= 1500 && s.fps
  if (fresh) {
    lastGoodFps = { fps: s.fps, frametime: s.frametimeX100 / 100, api: null }
    lastGoodAt = Date.now()
    return lastGoodFps
  }
  // Aussetzer ueberbruecken: letzten gueltigen Wert bis 1,5 s halten – egal ob
  // brokerTick kurz "stale" oder fps momentan 0.
  if (lastGoodFps && Date.now() - lastGoodAt < 1500) return lastGoodFps
  return null
}

// FPS-Sende-Schleife: bevorzugt RTSS (kein Admin), sonst PresentMon-Broker.
let fpsTimer = null, fpsUseRtss = false
function startFps() {
  if (fpsTimer) return
  const src = appSettings.osdFpsSource || 'auto'
  fpsUseRtss = src === 'rtss' ? true : src === 'presentmon' ? false : rtssAvailable()
  if (!fpsUseRtss) startBroker()
  console.log('[fps] Quelle:', fpsUseRtss ? 'RTSS' : 'PresentMon-Broker')
  fpsTimer = setInterval(() => {
    if (!overlayWindow || overlayWindow.isDestroyed() || !overlayWindow.isVisible()) return
    const f = fpsUseRtss ? readRtssFps() : readBrokerFps()
    if (f) overlayWindow.webContents.send('osd-data', { fps: f.fps, frametime: f.frametime, api: f.api || undefined })
    else overlayWindow.webContents.send('osd-data', { fps: '…' })
  }, 33)   // ~30 Hz fuer den Frametime-Graphen; die Zahlen drosselt das OSD auf lesbare ~4 Hz
}
function stopFps() {
  if (fpsTimer) { clearInterval(fpsTimer); fpsTimer = null }
  if (!fpsUseRtss) stopBroker()
}

// Startet/stoppt die FPS-Messung idempotent, passend zum gewuenschten Zustand
// (Overlay sichtbar UND FPS aktiviert). Verhindert Spawn-/Kill-Stuerme, wenn
// set-app-settings bei jeder Slider-Bewegung erneut aufgerufen wird.
function syncFps() {
  const vis = overlayWindow && !overlayWindow.isDestroyed() && overlayWindow.isVisible()
  const want = !!(appSettings.osdEnabled && appSettings.osdFps && vis)
  if (want && !fpsTimer) startFps()
  else if (!want && fpsTimer) stopFps()
}

// Sensor-Pump: solange das Overlay sichtbar ist, 1x/s Werte ans Overlay senden.
let osdDataInterval = null
function startOsdData() {
  if (osdDataInterval) return
  prevCpu = null
  const tick = () => {
    if (!overlayWindow || overlayWindow.isDestroyed() || !overlayWindow.isVisible()) return
    const payload = { gpu: readGpu(), cpu: readCpu() }
    if (!appSettings.osdFps) payload.fps = '—'   // FPS-Messung aus; sonst sendet die FPS-Schleife
    overlayWindow.webContents.send('osd-data', payload)
  }
  tick()
  osdDataInterval = setInterval(tick, 200)   // 5 Hz – Temps/Power/Takt fluessig
}
function stopOsdData() {
  if (osdDataInterval) { clearInterval(osdDataInterval); osdDataInterval = null }
}

// Das Overlay-Fenster deckt die GANZE Bildschirmflaeche ab (transparent +
// klick-durchlaessig). Dadurch ist die Panel-Position nur noch CSS (Ecke) und die
// Groesse ein einziger Zoomfaktor – kein Fenster-Verschieben/-Skalieren noetig.
// Ermittelt Kante + Dicke der Windows-Taskleiste (auch der auto-ausgeblendeten)
// via SHAppBarMessage. So kann das Overlay diesen Streifen freilassen, damit die
// Taskleiste sich einblenden UND anklicken laesst (sonst deckt das oberste
// Overlay sie ab). Faellt es aus, wird nichts freigelassen (edge=null).
function getTaskbarStrip() {
  let koffi
  try { koffi = require('koffi') } catch { return null }
  try {
    if (!getTaskbarStrip._fn) {
      const shell = koffi.load('shell32.dll')
      koffi.struct('LM_RECT', { left: 'int32', top: 'int32', right: 'int32', bottom: 'int32' })
      koffi.struct('LM_APPBARDATA', { cbSize: 'uint32', hWnd: 'uintptr', uCallbackMessage: 'uint32', uEdge: 'uint32', rc: 'LM_RECT', lParam: 'int64' })
      getTaskbarStrip._fn = shell.func('uintptr __stdcall SHAppBarMessage(uint32 dwMessage, _Inout_ LM_APPBARDATA* pData)')
    }
    const data = { cbSize: koffi.sizeof('LM_APPBARDATA'), hWnd: 0, uCallbackMessage: 0, uEdge: 0, rc: { left: 0, top: 0, right: 0, bottom: 0 }, lParam: 0 }
    if (!getTaskbarStrip._fn(0x5 /*ABM_GETTASKBARPOS*/, data)) return null
    const r = data.rc
    const EDGE = { 0: 'left', 1: 'top', 2: 'right', 3: 'bottom' }
    const state = getTaskbarStrip._fn(0x4 /*ABM_GETSTATE*/, data)   // ABS_AUTOHIDE = 0x1
    return {
      edge: EDGE[data.uEdge] || null,
      thickness: (data.uEdge === 1 || data.uEdge === 3) ? (r.bottom - r.top) : (r.right - r.left),
      autohide: (Number(state) & 1) === 1,
    }
  } catch { return null }
}

function createOverlayWindow() {
  if (overlayWindow && !overlayWindow.isDestroyed()) return overlayWindow
  const b = screen.getPrimaryDisplay().bounds
  // Overlay auf den Bildschirm legen, aber den Taskleisten-Streifen freilassen –
  // sonst deckt das oberste, klick-durchlaessige Overlay die (auto-ausgeblendete)
  // Taskleiste ab und sie laesst sich weder einblenden noch anklicken.
  let x = b.x, y = b.y, width = b.width, height = b.height
  const tb = getTaskbarStrip()
  if (tb) {
    // Auto-Hide: nur einen schmalen Reveal-Streifen freilassen -> OSD kommt fast
    // bis an den Rand UND die Leiste laesst sich noch einblenden. Dauerhaft
    // sichtbare Leiste: vollen Streifen freilassen, damit das OSD davor sitzt.
    const gap = tb.autohide ? 3 : tb.thickness
    if (tb.edge === 'bottom') height -= gap
    else if (tb.edge === 'top') { y += gap; height -= gap }
    else if (tb.edge === 'left') { x += gap; width -= gap }
    else if (tb.edge === 'right') width -= gap
  } else {
    height -= 1   // Fallback: 1px unten frei (bricht Vollbild-Erkennung)
  }
  overlayWindow = new BrowserWindow({
    x, y, width, height,
    frame: false,
    transparent: true,
    resizable: false,
    movable: false,
    minimizable: false,
    maximizable: false,
    skipTaskbar: true,
    focusable: false,          // stiehlt dem Spiel niemals den Fokus
    show: false,
    hasShadow: false,
    webPreferences: { nodeIntegration: true, contextIsolation: false, backgroundThrottling: false },
  })
  overlayWindow.setIgnoreMouseEvents(true, { forward: true })   // Klicks gehen ans Spiel
  overlayWindow.setAlwaysOnTop(true, 'screen-saver')           // hoechste Ebene – liegt auch ueber einem bereits im Vordergrund laufenden Vollbild-Spiel
  overlayWindow.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true })
  overlayWindow.loadFile('osd.html')
  overlayWindow.webContents.once('did-finish-load', () => applyOsdConfig())
  overlayWindow.on('closed', () => { overlayWindow = null })
  return overlayWindow
}

// Uebertraegt die aktuelle OSD-Konfiguration ins Overlay: Zoom = Groesse,
// Ecke + Deckkraft ans Renderer-Panel.
function applyOsdConfig() {
  if (!overlayWindow || overlayWindow.isDestroyed()) return
  const z = Math.max(0.4, Math.min(3, Number(appSettings.osdScale) || 1))
  try { overlayWindow.webContents.setZoomFactor(z) } catch {}
  overlayWindow.webContents.send('osd-config', {
    corner: appSettings.osdCorner || 'tl',
    opacity: appSettings.osdOpacity != null ? appSettings.osdOpacity : 0.55,
    theme: appSettings.osdTheme || 'compact',
    fields: appSettings.osdFields,
    accent: appSettings.osdAccent || '#74e857',
  })
}

function showOverlay() {
  const w = createOverlayWindow()
  // Beim (Wieder-)Einblenden ALLE Overlay-Eigenschaften neu setzen: ein zuvor
  // verstecktes Fenster verliert ueber einem Vollbild-Spiel sonst Klick-
  // Durchlaessigkeit / oberste Lage und erscheint gar nicht bzw. hinter dem Spiel.
  w.setIgnoreMouseEvents(true, { forward: true })
  w.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true })
  w.showInactive()                                 // anzeigen, ohne zu fokussieren
  w.setAlwaysOnTop(true, 'screen-saver')           // hoechste Ebene – auch ueber laufendem Vollbild-Spiel
  w.moveTop()                                      // erzwingt oberste Lage ueber dem Spiel
  applyOsdConfig()
  startOsdData()
  syncFps()
  try { osdDbg('showOverlay: isVisible=' + w.isVisible() + ' onTop=' + w.isAlwaysOnTop() + ' bounds=' + JSON.stringify(w.getBounds())) } catch {}
}

function hideOverlay() {
  if (overlayWindow && !overlayWindow.isDestroyed()) overlayWindow.hide()
  stopOsdData()
  syncFps()
}

function toggleOverlay() {
  const visible = overlayWindow && !overlayWindow.isDestroyed() && overlayWindow.isVisible()
  appSettings.osdEnabled = !visible
  osdDbg('toggleOverlay: war sichtbar=' + visible + ' -> osdEnabled=' + appSettings.osdEnabled)
  saveAppSettings()
  if (appSettings.osdEnabled) showOverlay(); else hideOverlay()
}

// --- Live-Edit-Modus: OSD direkt ueber dem Spiel anfassbar machen -------------
// Der Hotkey macht das Overlay kurz interaktiv (faengt Maus): ziehen = Position
// (rastet in die naechste Ecke), Mausrad = Groesse. "Fertig" schaltet zurueck.
let osdEditActive = false
function setOsdEditMode(on) {
  const w = createOverlayWindow()
  if (on) {
    if (!appSettings.osdEnabled) { appSettings.osdEnabled = true; saveAppSettings() }
    showOverlay()
    // Fokussierbar machen + fokussieren: ein focusable:false-Fenster wird von
    // Windows nicht aktiviert und faengt die Maus nur, wenn es ohnehin oberstes
    // Fenster ist (z.B. ueber einem Spiel). Ist Lumora selbst im Vordergrund,
    // braucht es echten Fokus, damit Ziehen/Mausrad/Buttons reagieren.
    w.setFocusable(true)
    w.setIgnoreMouseEvents(false)             // Maus wird jetzt gefangen
    w.focus()
  } else {
    w.setIgnoreMouseEvents(true, { forward: true })   // wieder klick-durchlaessig
    w.setFocusable(false)
  }
  osdEditActive = on
  w.webContents.send('osd-edit', on)
}
function toggleOsdEdit() { setOsdEditMode(!osdEditActive) }

// --- Nativer Gamepad-Hotkey (funktioniert auch minimiert / während Spielen) ---
// Der Renderer-Gamepad-Code friert ein, sobald das Fenster den Fokus verliert.
// Darum lesen wir die Controller hier im Hauptprozess direkt über die Windows-
// XInput-DLL (via koffi). Fällt koffi/DLL aus, bleibt alles beim Alten – dann
// funktioniert der Gamepad-Hotkey eben nur im Vordergrund (Renderer-Fallback).
let xinputPoll = null
let xinputGetState = null
let gpHotkeyDown = false
let getAsyncKey = null   // GetAsyncKeyState – fuer Tastatur-Hotkeys via Polling (greift auch im Spiel)

// Standard-Gamepad-API-Index -> Prüf-Funktion auf dem XINPUT_GAMEPAD-Struct.
const XI_MASK = {
  0: g => g.wButtons & 0x1000, 1: g => g.wButtons & 0x2000,
  2: g => g.wButtons & 0x4000, 3: g => g.wButtons & 0x8000,
  4: g => g.wButtons & 0x0100, 5: g => g.wButtons & 0x0200,
  6: g => g.bLeftTrigger > 40,  7: g => g.bRightTrigger > 40,
  8: g => g.wButtons & 0x0020, 9: g => g.wButtons & 0x0010,
  10: g => g.wButtons & 0x0040, 11: g => g.wButtons & 0x0080,
  12: g => g.wButtons & 0x0001, 13: g => g.wButtons & 0x0002,
  14: g => g.wButtons & 0x0004, 15: g => g.wButtons & 0x0008,
}

function setupGamepadHotkey() {
  let koffi
  try { koffi = require('koffi') } catch (e) {
    console.log('[gamepad] koffi nicht verfügbar – nativer Hotkey deaktiviert:', e && e.message)
    return
  }
  try {
    let lib = null
    for (const name of ['xinput1_4.dll', 'xinput1_3.dll', 'xinput9_1_0.dll']) {
      try { lib = koffi.load(name); break } catch {}
    }
    if (!lib) throw new Error('keine XInput-DLL ladbar')

    koffi.struct('XINPUT_GAMEPAD', {
      wButtons: 'uint16', bLeftTrigger: 'uint8', bRightTrigger: 'uint8',
      sThumbLX: 'int16', sThumbLY: 'int16', sThumbRX: 'int16', sThumbRY: 'int16'
    })
    koffi.struct('XINPUT_STATE', { dwPacketNumber: 'uint32', Gamepad: 'XINPUT_GAMEPAD' })
    xinputGetState = lib.func('uint32 __stdcall XInputGetState(uint32 dwUserIndex, _Out_ XINPUT_STATE *pState)')
    console.log('[gamepad] nativer XInput-Hotkey aktiv')
  } catch (e) {
    console.log('[gamepad] XInput-Init fehlgeschlagen:', e && e.message)
    xinputGetState = null
  }
  // Tastatur-Hotkeys: GetAsyncKeyState pollen (fokusunabhaengig -> greift auch im
  // Spiel, im Gegensatz zu globalShortcut/LL-Hook, die vom Spiel abgefangen werden).
  try { getAsyncKey = koffi.load('user32.dll').func('int16 GetAsyncKeyState(int vKey)') }
  catch (e) { getAsyncKey = null; console.log('[hotkey] GetAsyncKeyState nicht ladbar:', e && e.message) }
  // Poll starten, sobald wenigstens EINE native Eingabe verfuegbar ist.
  if ((xinputGetState || getAsyncKey) && !xinputPoll) xinputPoll = setInterval(pollNativeHotkeys, 40)
}

function pollNativeHotkeys() {
  // 1) Gamepad-Kombi (XInput)
  const combo = appSettings.gamepadHotkey
  if (xinputGetState && combo && combo.length && !combo.some(i => !XI_MASK[i])) {
    let anyDown = false
    for (let slot = 0; slot < 4 && !anyDown; slot++) {
      const st = {}; let ret
      try { ret = xinputGetState(slot, st) } catch { continue }
      if (ret !== 0) continue
      if (combo.every(i => XI_MASK[i](st.Gamepad))) anyDown = true
    }
    if (anyDown && !gpHotkeyDown) toggleMainWindow()   // steigende Flanke
    gpHotkeyDown = anyDown
  } else gpHotkeyDown = false

  // 2) Tastatur-Hotkeys (GetAsyncKeyState, steigende Flanke je Hotkey)
  if (getAsyncKey && khHotkeys.length) {
    const dn = (vk) => (getAsyncKey(vk) & 0x8000) !== 0
    const alt = dn(0x12), ctrl = dn(0x11), shift = dn(0x10)
    for (const h of khHotkeys) {
      const on = dn(h.vk) && h.alt === alt && h.ctrl === ctrl && h.shift === shift
      if (on && !h._down) h.action()
      h._down = on
    }
  }
}

function createTray() {
  if (tray) return
  const icon = nativeImage.createFromPath(path.join(__dirname, 'icon.ico'))
  tray = new Tray(icon)
  tray.setToolTip('Lumora')
  tray.setContextMenu(Menu.buildFromTemplate([
    { label: 'Öffnen', click: showMainWindow },
    { type: 'separator' },
    { label: 'Beenden', click: () => { app.isQuitting = true; app.quit() } },
  ]))
  tray.on('click', () => {
    if (mainWindow && mainWindow.isVisible() && !mainWindow.isMinimized()) mainWindow.hide()
    else showMainWindow()
  })
}

function destroyTray() {
  if (tray) { tray.destroy(); tray = null }
}

function loadWindowState() {
  try { return JSON.parse(fs.readFileSync(windowStateFile, 'utf8')) } catch { return null }
}

function saveWindowState() {
  if (!mainWindow) return
  try {
    const b = mainWindow.getNormalBounds()
    fs.writeFileSync(windowStateFile, JSON.stringify({ ...b, maximized: mainWindow.isMaximized() }))
  } catch {}
}

function isVisibleOnSomeDisplay(b) {
  return screen.getAllDisplays().some(d => {
    const a = d.workArea
    return b.x < a.x + a.width && b.x + b.width > a.x && b.y < a.y + a.height && b.y + b.height > a.y
  })
}

// Einmalige Übernahme der Bibliothek/Einstellungen aus dem alten Profilordner
// "hdr-launcher" (vor dem Rebrand zu "Lumora"). app.getName() = "lumora" → userData
// liegt jetzt in %APPDATA%\lumora; ohne Migration erschiene die Bibliothek leer.
function migrateOldUserData() {
  try {
    const cur = app.getPath('userData')
    fs.mkdirSync(cur, { recursive: true })
    if (fs.existsSync(path.join(cur, 'games.json'))) return // schon vorhanden/migriert
    const old = path.join(app.getPath('appData'), 'hdr-launcher')
    if (old === cur || !fs.existsSync(old)) return
    for (const f of ['games.json', 'prefs.json', 'app-settings.json', 'window-state.json']) {
      const s = path.join(old, f)
      if (fs.existsSync(s)) { try { fs.copyFileSync(s, path.join(cur, f)) } catch {} }
    }
    const oldMedia = path.join(old, 'media'), newMedia = path.join(cur, 'media')
    if (fs.existsSync(oldMedia)) {
      try {
        fs.mkdirSync(newMedia, { recursive: true })
        for (const m of fs.readdirSync(oldMedia)) {
          try { fs.copyFileSync(path.join(oldMedia, m), path.join(newMedia, m)) } catch {}
        }
      } catch {}
    }
  } catch {}
}

function createWindow() {
  migrateOldUserData()
  mediaDir = path.join(app.getPath('userData'), 'media')
  windowStateFile = path.join(app.getPath('userData'), 'window-state.json')
  appSettingsFile = path.join(app.getPath('userData'), 'app-settings.json')
  gamesFile = path.join(app.getPath('userData'), 'games.json')
  prefsFile = path.join(app.getPath('userData'), 'prefs.json')
  try { fs.mkdirSync(mediaDir, { recursive: true }) } catch {}

  loadAppSettings()
  applyAutostart()
  if (appSettings.minimizeToTray) createTray()

  const state = loadWindowState() || {}
  const hasValidPos = Number.isInteger(state.x) && Number.isInteger(state.y) &&
    isVisibleOnSomeDisplay({ x: state.x, y: state.y, width: state.width || 900, height: state.height || 600 })
  // Minimiert starten NUR beim Autostart: der Autostart-Eintrag uebergibt
  // '--minimized' (siehe applyAutostart). Beim MANUELLEN Start fehlt das Argument
  // -> die App oeffnet normal, auch wenn "Minimiert starten" aktiviert ist.
  const wantMin = process.argv.includes('--minimized')

  mainWindow = new BrowserWindow({
    width: state.width || 900,
    height: state.height || 600,
    x: hasValidPos ? state.x : undefined,
    y: hasValidPos ? state.y : undefined,
    minWidth: 700,
    minHeight: 500,
    frame: false,
    title: 'Lumora',
    show: !wantMin,         // normal sofort sichtbar (dunkler bg + Boot-Screen); nur minimiert verborgen
    icon: path.join(__dirname, 'icon.ico'),
    webPreferences: {
      nodeIntegration: true,
      contextIsolation: false,
      // Timer/Gamepad-Polling laufen auch weiter, wenn das Fenster minimiert/
      // verdeckt ist – nötig, damit ein Gamepad-Hotkey Lumora zurückholen kann.
      backgroundThrottling: false
    },
    backgroundColor: '#0f0f0f'
  })

  if (state.maximized) mainWindow.maximize()

  // Nur beim minimierten Start (Autostart): nach dem ersten Rendern minimieren bzw. im Tray lassen.
  mainWindow.once('ready-to-show', () => {
    if (!wantMin) return
    if (appSettings.minimizeToTray) return
    mainWindow.minimize()
  })

  mainWindow.on('maximize',   () => mainWindow.webContents.send('window-maximized'))
  mainWindow.on('unmaximize', () => mainWindow.webContents.send('window-unmaximized'))
  mainWindow.on('close', (e) => {
    saveWindowState()
    if (appSettings.minimizeToTray && !app.isQuitting) {
      e.preventDefault()
      mainWindow.hide()
      return
    }
    // Echtes Schliessen -> App ganz beenden. Sonst haelt ein offenes OSD-Overlay-
    // Fenster die App im Hintergrund am Leben (window-all-closed feuert nie).
    app.isQuitting = true
    if (overlayWindow && !overlayWindow.isDestroyed()) overlayWindow.destroy()
  })

  mainWindow.loadFile('index.html')

  mainWindow.webContents.once('did-finish-load', () => {
    queryHDRStatus((enabled) => {
      lastHdrState = enabled
      mainWindow.webContents.send('hdr-status', enabled)
    })
    startHDRPolling()
    // Kurz nach dem Start still nach Updates schauen (UI ist dann bereit).
    setTimeout(() => checkForUpdates(false), 4000)
  })
}

function getHdrCmdPath() {
  return app.isPackaged
    ? path.join(process.resourcesPath, 'HDRCmd.exe')
    : path.join(__dirname, 'HDRCmd.exe')
}

function setHDR(enabled) {
  const arg = enabled ? 'on' : 'off'
  exec(`"${getHdrCmdPath()}" ${arg}`, (err) => {
    if (err) console.error('HDR Fehler:', err)
  })
}

function queryHDRStatus(callback) {
  exec(`"${getHdrCmdPath()}" status`, (err, stdout) => {
    const enabled = stdout && stdout.toLowerCase().includes('hdr is on')
    callback(enabled)
  })
}

function startHDRPolling() {
  hdrPollInterval = setInterval(() => {
    queryHDRStatus((enabled) => {
      if (enabled !== lastHdrState) {
        lastHdrState = enabled
        mainWindow.webContents.send('hdr-status', enabled)
      }
    })
  }, 3000)
}

// ── Echte Steam-Icons aus dem lokalen librarycache ───────────
async function getSteamLibraries() {
  const steamPath = await regQuery('HKCU\\Software\\Valve\\Steam', 'SteamPath')
  if (!steamPath) return null
  const main = steamPath.replace(/\//g, '\\')
  const libs = [path.join(main, 'steamapps')]
  try {
    const content = fs.readFileSync(path.join(main, 'steamapps', 'libraryfolders.vdf'), 'utf8')
    for (const p of parseVdfLibraryPaths(content)) libs.push(path.join(p, 'steamapps'))
  } catch {}
  return { main, libs }
}

async function steamAppIdForExe(exePath) {
  const info = await getSteamLibraries()
  if (!info) return null
  const m = exePath.toLowerCase().match(/steamapps[\\/]+common[\\/]+([^\\/]+)[\\/]/)
  if (!m) return null
  const installdir = m[1]
  for (const lib of info.libs) {
    try {
      for (const f of fs.readdirSync(lib).filter(f => /^appmanifest_\d+\.acf$/i.test(f))) {
        const content = fs.readFileSync(path.join(lib, f), 'utf8')
        const dir = parseVdfValue(content, 'installdir')
        if (dir && dir.toLowerCase() === installdir) {
          return { appId: parseVdfValue(content, 'appid') || f.match(/\d+/)[0], main: info.main }
        }
      }
    } catch {}
  }
  return null
}

function readSteamIcon(mainSteam, appId) {
  try {
    const dir = path.join(mainSteam, 'appcache', 'librarycache', String(appId))
    if (!fs.existsSync(dir)) return null
    const icons = fs.readdirSync(dir).filter(f => /^[0-9a-f]{40}\.jpg$/i.test(f))
    if (!icons.length) return null
    icons.sort((a, b) => fs.statSync(path.join(dir, a)).size - fs.statSync(path.join(dir, b)).size)
    return 'data:image/jpeg;base64,' + fs.readFileSync(path.join(dir, icons[0])).toString('base64')
  } catch { return null }
}

// Signatur des generischen Windows-Exe-Icons ermitteln (0-Byte-Probe-Datei)
let genericIconHash = null
async function getGenericIconHash() {
  if (genericIconHash !== null) return genericIconHash
  genericIconHash = ''
  try {
    const probe = path.join(app.getPath('temp'), '__hdrl_icon_probe__.exe')
    fs.writeFileSync(probe, Buffer.alloc(0))
    const icon = await app.getFileIcon(probe, { size: 'large' })
    genericIconHash = crypto.createHash('md5').update(icon.toPNG()).digest('hex')
    try { fs.unlinkSync(probe) } catch {}
  } catch {}
  return genericIconHash
}

ipcMain.handle('get-file-icon', async (event, filePath) => {
  try {
    // 1) Echtes Steam-Original-Icon (für Steam-Spiele ohne brauchbares Exe-Icon)
    if (/steamapps/i.test(filePath)) {
      const r = await steamAppIdForExe(filePath)
      if (r) { const si = readSteamIcon(r.main, r.appId); if (si) return si }
    }
    // 2) Exe-/Datei-Icon — generischen Windows-Platzhalter verwerfen
    const icon = await app.getFileIcon(filePath, { size: 'large' })
    const generic = await getGenericIconHash()
    if (generic && crypto.createHash('md5').update(icon.toPNG()).digest('hex') === generic) return null
    return icon.toDataURL()
  } catch {
    return null
  }
})

ipcMain.handle('browse-icon', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    title: 'Icon auswählen (.exe oder .ico)',
    filters: [
      { name: 'Icon-Dateien', extensions: ['ico', 'exe'] },
      { name: 'Alle Dateien', extensions: ['*'] }
    ],
    properties: ['openFile']
  })
  if (result.canceled) return null
  const filePath = result.filePaths[0]
  try {
    if (filePath.toLowerCase().endsWith('.ico')) {
      const data = fs.readFileSync(filePath)
      return 'data:image/x-icon;base64,' + data.toString('base64')
    }
    const icon = await app.getFileIcon(filePath, { size: 'large' })
    return icon.toDataURL()
  } catch {
    return null
  }
})

// Bereinigt Spielnamen (oft Ordnernamen mit ™/®/Edition-Zusätzen) für die Store-Suche.
function cleanGameName(name) {
  return (name || '')
    .replace(/[™®©]/g, ' ')
    .replace(/\s+(deluxe|ultimate|gold|goty|definitive|complete|standard|premium|legendary|anniversary)\s+edition\s*$/i, '')
    .replace(/\s+edition\s*$/i, '')
    .replace(/\s+/g, ' ')
    .trim()
}

async function resolveSteamAppId(gameName) {
  const cleaned = cleanGameName(gameName)
  const query = encodeURIComponent(cleaned)
  const res = await httpsGet(`https://store.steampowered.com/api/storesearch/?term=${query}&l=english&cc=US`)
  if (res.status !== 200) return null
  const data = JSON.parse(res.body)
  const items = data?.items
  if (!items || items.length === 0) return null
  const norm = s => (s || '').toLowerCase().replace(/[^a-z0-9]/g, '')
  const target = norm(cleaned)
  // Exakte Namensübereinstimmung bevorzugen, sonst erstes Ergebnis
  const exact = items.find(i => norm(i.name) === target)
  return (exact || items[0]).id
}

// Versucht mehrere CDN-Pfade/Hosts und liefert das erste echte Bild als dataURL.
async function fetchFirstImage(urls) {
  for (const url of urls) {
    try {
      const bin = await httpsGetBinary(url)
      const b = bin.buffer
      // Steam liefert für fehlende Assets einen winzigen Platzhalter (~1,5 KB) mit Status 200.
      // Daher echtes JPEG verlangen: Magic-Bytes (FF D8) UND ausreichend groß.
      if (bin.status === 200 && b && b.length > 5000 && b[0] === 0xFF && b[1] === 0xD8) {
        return 'data:image/jpeg;base64,' + b.toString('base64')
      }
    } catch {}
  }
  return null
}

const STEAM_ASSET_BASE = 'https://shared.akamai.steamstatic.com/store_item_assets/'

// Alte, un-gehashte Pfade als Fallback – NUR Hochformat (library_600x900),
// KEIN header.jpg, denn das ist Querformat und sähe als Cover falsch aus.
function steamCoverUrls(appId) {
  return [
    `https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/${appId}/library_600x900.jpg`,
    `https://cdn.akamai.steamstatic.com/steam/apps/${appId}/library_600x900.jpg`,
  ]
}

// Liefert die ECHTEN Asset-URLs (inkl. Content-Hash) über die offizielle Store-API.
// Wichtig für neuere Spiele (z.B. Battlefield 6): deren Cover/Hero liegen NUR unter
// gehashtem Pfad – der schlichte library_600x900.jpg-Pfad gibt nur einen Platzhalter.
// Akzeptiert mehrere AppIDs in einem Aufruf → map appId -> {name, cover, hero, header}.
async function steamGetItems(appIds) {
  const map = {}
  try {
    if (!appIds || !appIds.length) return map
    const input = JSON.stringify({
      ids: appIds.map(a => ({ appid: Number(a) })),
      context: { language: 'english', country_code: 'US' },
      data_request: { include_assets: true, include_basic_info: true },
    })
    const res = await httpsGet(`https://api.steampowered.com/IStoreBrowseService/GetItems/v1/?input_json=${encodeURIComponent(input)}`)
    if (res.status !== 200) return map
    const items = JSON.parse(res.body)?.response?.store_items || []
    for (const it of items) {
      const a = it.assets || {}
      const mk = f => (a.asset_url_format && f) ? STEAM_ASSET_BASE + a.asset_url_format.replace('${FILENAME}', f) : null
      // type 0 = Spiel, alles andere (4 = DLC, …) bzw. ein parent_appid = Zusatzinhalt
      const isDlc = it.type !== 0 || !!(it.related_items && it.related_items.parent_appid)
      map[String(it.id)] = { name: it.name, cover: mk(a.library_capsule), hero: mk(a.library_hero), isDlc }
    }
  } catch {}
  return map
}

// appId optional: wenn gesetzt (z.B. vom Nutzer in der Cover-Suche gewählt),
// wird direkt diese Steam-App verwendet statt erneut zu raten.
async function fetchCoverSteam(gameName, appId) {
  const id = appId || await resolveSteamAppId(gameName)
  if (!id) return null
  const a = (await steamGetItems([id]))[String(id)]
  const urls = []
  if (a && a.cover) urls.push(a.cover)       // library_capsule = Hochformat
  urls.push(...steamCoverUrls(id))           // Hochformat-Fallback (kein header)
  return await fetchFirstImage(urls)
}

async function fetchSteamHero(gameName, appId) {
  try {
    const id = appId || await resolveSteamAppId(gameName)
    if (!id) return null
    const a = (await steamGetItems([id]))[String(id)]
    const urls = []
    if (a && a.hero) urls.push(a.hero)
    urls.push(
      `https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/${id}/library_hero.jpg`,
      `https://cdn.akamai.steamstatic.com/steam/apps/${id}/library_hero.jpg`,
    )
    return await fetchFirstImage(urls)
  } catch { return null }
}

// Liefert mehrere Treffer-Kandidaten mit echtem Cover zur Auswahl (Steam + MS Store).
async function steamCandidates(term) {
  try {
    const query = encodeURIComponent(cleanGameName(term))
    const res = await httpsGet(`https://store.steampowered.com/api/storesearch/?term=${query}&l=english&cc=US`)
    if (res.status !== 200) return []
    const data = JSON.parse(res.body)
    return (data?.items || []).map(i => ({ appId: String(i.id), name: i.name }))
  } catch { return [] }
}

async function msStoreCandidates(term) {
  try {
    const query = encodeURIComponent(cleanGameName(term))
    const res = await httpsGet(`https://storeedgefd.dsx.mp.microsoft.com/v9.0/search?query=${query}&market=US&locale=en-us&deviceFamily=Windows.Desktop`)
    if (res.status !== 200) return []
    const data = JSON.parse(res.body)
    return (data?.Payload?.SearchResults || []).map(r => {
      const images = r.Images || []
      const img = images.find(i => i.ImageType === 'Poster') || images.find(i => i.ImageType === 'BoxArt') || images[0]
      return img?.Url ? { name: r.Title, imageUrl: img.Url } : null
    }).filter(Boolean)
  } catch { return [] }
}

// MS-Store-Bild-URLs haben keinen Query-Teil → Resize-Parameter mit '?' anhängen
// (mit '&' liefert der Server 500). Fällt nötigenfalls auf die nackte URL zurück.
function msImageUrl(url) {
  return url + (url.includes('?') ? '&' : '?') + 'w=600&h=900&format=jpg'
}

async function fetchMsImage(url) {
  let bin = await httpsGetBinary(msImageUrl(url))
  if (!(bin.status === 200 && bin.buffer && bin.buffer.length > 3000)) bin = await httpsGetBinary(url)
  if (bin.status === 200 && bin.buffer && bin.buffer.length > 3000) {
    return 'data:image/jpeg;base64,' + bin.buffer.toString('base64')
  }
  return null
}

async function fetchCoverMSStore(gameName) {
  const query = encodeURIComponent(cleanGameName(gameName))
  const res = await httpsGet(`https://storeedgefd.dsx.mp.microsoft.com/v9.0/search?query=${query}&market=US&locale=en-us&deviceFamily=Windows.Desktop`)
  if (res.status !== 200) return null
  const data = JSON.parse(res.body)
  const result = data?.Payload?.SearchResults?.[0]
  if (!result) return null
  const images = result.Images || []
  const img = images.find(i => i.ImageType === 'Poster') || images.find(i => i.ImageType === 'BoxArt') || images[0]
  if (!img?.Url) return null
  return await fetchMsImage(img.Url)
}

ipcMain.handle('fetch-cover', async (event, gameName, appId) => {
  try {
    const steam = await fetchCoverSteam(gameName, appId)
    if (steam) return steam
    return appId ? null : await fetchCoverMSStore(gameName)
  } catch {
    return null
  }
})

ipcMain.handle('fetch-hero', async (event, gameName, appId) => {
  try {
    return await fetchSteamHero(gameName, appId)
  } catch {
    return null
  }
})

// ── SteamGridDB (optionale Zusatzquelle, benötigt nutzereigenen API-Key) ──────
// Lädt ein beliebiges Bild (JPEG/PNG/WebP) als dataURL – SGDB liefert nicht nur JPEG.
async function fetchAnyImage(url) {
  try {
    const bin = await httpsGetBinary(url)
    const b = bin.buffer
    if (bin.status !== 200 || !b || b.length < 1500) return null
    let mime = null
    if (b[0] === 0xFF && b[1] === 0xD8) mime = 'image/jpeg'
    else if (b[0] === 0x89 && b[1] === 0x50) mime = 'image/png'
    else if (b[0] === 0x52 && b[1] === 0x49 && b[8] === 0x57 && b[9] === 0x45) mime = 'image/webp'
    if (!mime) return null
    return `data:${mime};base64,` + b.toString('base64')
  } catch { return null }
}

async function sgdbApi(pathQuery, key) {
  try {
    const res = await httpsGet(`https://www.steamgriddb.com/api/v2${pathQuery}`, { Authorization: `Bearer ${key}` })
    if (res.status !== 200) return null
    const j = JSON.parse(res.body)
    return (j && j.success) ? j.data : null
  } catch { return null }
}

// kind: 'cover' → grids (Hochformat 600x900), 'hero' → heroes (Querformat 1920x620)
async function sgdbArtwork(term, kind, key) {
  if (!key) return []
  const games = await sgdbApi(`/search/autocomplete/${encodeURIComponent(cleanGameName(term))}`, key)
  if (!games || !games.length) return []
  const out = []
  for (const g of games.slice(0, 2)) {            // bis zu 2 passende Spiele
    const ep = kind === 'hero'
      ? `/heroes/game/${g.id}?dimensions=1920x620,3840x1240&types=static&nsfw=false&humor=false&limit=8`
      : `/grids/game/${g.id}?dimensions=600x900,660x930&types=static&nsfw=false&humor=false&limit=8`
    const items = await sgdbApi(ep, key)
    if (!items) continue
    for (const it of items) {
      if (out.length >= 8) break                  // Gesamt-Limit gegen zu viele/große Downloads
      if (!it.url) continue
      const img = await fetchAnyImage(it.url)
      if (img) out.push({ source: 'SteamGridDB', name: g.name, cover: img })
    }
    if (out.length >= 8) break
  }
  return out
}

// Einzelquellen der erweiterten Suche – getrennt aufrufbar, damit der Renderer
// alle PARALLEL anstößt und Treffer anzeigt, sobald die jeweilige Quelle fertig
// ist (Steam/MS schnell, SteamGridDB langsamer). kind: 'cover' | 'hero'.
async function searchSteamArt(term, wantHero) {
  const steam = await steamCandidates(term)
  const top = steam.slice(0, 12)
  const assets = top.length ? await steamGetItems(top.map(c => c.appId)) : {}
  const steamArt = await Promise.all(top.map(async c => {
    const a = assets[String(c.appId)]
    if (!a || a.isDlc) return []   // DLCs (VIP-Pack, Expansions …) ausblenden
    if (wantHero) {
      // GLEICHE Quellen wie fetchSteamHero: GetItems-library_hero UND der
      // un-gehashte library_hero.jpg (unterscheiden sich bei manchen Spielen).
      const heroUrls = [a.hero, `${STEAM_ASSET_BASE}steam/apps/${c.appId}/library_hero.jpg`].filter(Boolean)
      const imgs = await Promise.all(heroUrls.map(u => fetchFirstImage([u])))
      const seen = new Set(), out = []
      for (const img of imgs) {
        if (img && !seen.has(img)) { seen.add(img); out.push({ source: 'Steam', appId: c.appId, name: c.name, cover: img }) }
      }
      return out
    }
    // Cover: nur Hochformat (library_capsule + library_600x900), KEIN header-Querformat
    const img = await fetchFirstImage([a.cover, ...steamCoverUrls(c.appId)].filter(Boolean))
    return img ? [{ source: 'Steam', appId: c.appId, name: c.name, cover: img }] : []
  }))
  return steamArt.flat()
}

async function searchMsArt(term) {
  const ms = await msStoreCandidates(term)
  const msCovers = await Promise.all(ms.slice(0, 4).map(async c => {
    try {
      const cover = await fetchMsImage(c.imageUrl)
      if (cover) return { source: 'Microsoft Store', name: c.name, cover }
    } catch {}
    return null
  }))
  return msCovers.filter(Boolean)
}

ipcMain.handle('search-steam', async (event, term, kind) => {
  try { return await searchSteamArt(term, kind === 'hero') } catch { return [] }
})
ipcMain.handle('search-msstore', async (event, term, kind) => {
  if (kind === 'hero') return []   // MS Store hat keine Hero-Banner
  try { return await searchMsArt(term) } catch { return [] }
})
ipcMain.handle('search-sgdb', async (event, term, kind) => {
  if (!appSettings.steamGridDbKey) return []
  try { return await sgdbArtwork(term, kind === 'hero' ? 'hero' : 'cover', appSettings.steamGridDbKey) } catch { return [] }
})

// Cover aus einer beliebigen Bild-URL laden (Option „eigenes Bild").
ipcMain.handle('fetch-image-url', async (event, url) => {
  try {
    if (!/^https?:\/\//i.test(url || '')) return null
    const bin = await httpsGetBinary(url)
    const b = bin.buffer
    if (bin.status === 200 && b && b.length > 1000) {
      if (b[0] === 0xFF && b[1] === 0xD8) return 'data:image/jpeg;base64,' + b.toString('base64')
      if (b[0] === 0x89 && b[1] === 0x50) return 'data:image/png;base64,' + b.toString('base64')
    }
  } catch {}
  return null
})

async function fetchGameInfo(gameName, appId) {
  try {
    appId = appId || await resolveSteamAppId(gameName)
    if (!appId) return null
    const res = await httpsGet(`https://store.steampowered.com/api/appdetails?appids=${appId}&l=german`)
    if (res.status !== 200) return null
    const data = JSON.parse(res.body)
    const d = data && data[appId] && data[appId].data
    if (!d) return null
    const yearMatch = ((d.release_date && d.release_date.date) || '').match(/\d{4}/)
    return {
      description: d.short_description || '',
      genres: (d.genres || []).map(x => x.description).slice(0, 4),
      releaseYear: yearMatch ? yearMatch[0] : '',
      developer: (d.developers || [])[0] || '',
    }
  } catch { return null }
}

ipcMain.handle('fetch-game-info', async (event, gameName, appId) => {
  try { return await fetchGameInfo(gameName, appId) } catch { return null }
})

function tokenizeArgs(s) {
  if (!s) return []
  return (s.match(/"[^"]*"|\S+/g) || []).map(t => t.replace(/^"|"$/g, ''))
}

// Läuft das Steam-Spiel laut Steam selbst? (prozessnamen-unabhängig → zuverlässig
// auch bei DRM-Handoff, Launcher-Exes und langen Namen). null = unbekannt.
function steamAppRunning(appId) {
  return new Promise(resolve => {
    exec(`reg query "HKCU\\Software\\Valve\\Steam\\Apps\\${appId}" /v Running`, (err, stdout) => {
      const m = (stdout || '').match(/Running\s+REG_DWORD\s+0x([0-9a-fA-F]+)/)
      resolve(m ? (parseInt(m[1], 16) === 1) : null)
    })
  })
}

// Läuft irgendein Prozess aus dem Spielordner? (fängt DRM-freie/direkt gestartete
// Spiele sowie Spiele, die unter anderem Exe-Namen laufen).
function anyProcessInFolder(dir) {
  return new Promise(resolve => {
    const safe = dir.replace(/'/g, "''")
    exec(`powershell -NoProfile -Command "$ErrorActionPreference='SilentlyContinue';(Get-Process | Where-Object { $_.Path -like '${safe}\\*' } | Measure-Object).Count"`,
      (err, stdout) => resolve(parseInt((stdout || '').trim(), 10) > 0))
  })
}

// Xbox/Game-Pass-Spiele (UWP) sind als Exe nicht direkt startbar (Zugriff
// verweigert) – sie müssen über ihre AUMID gestartet werden. Die holen wir
// zuverlässig über Get-StartApps, gematcht über den XboxGames-Ordnernamen.
function xboxAumidForGame(gamePath) {
  return new Promise(resolve => {
    const m = gamePath.match(/\\XboxGames\\([^\\]+)\\/i)
    if (!m) return resolve(null)
    const folder = m[1].replace(/'/g, "''")
    exec(`powershell -NoProfile -Command "$ErrorActionPreference='SilentlyContinue';Get-StartApps | Where-Object { $_.Name -like '${folder}*' } | Sort-Object { $_.Name.Length } | Select-Object -First 1 -ExpandProperty AppID"`,
      (err, stdout) => resolve((stdout || '').trim() || null))
  })
}

// Fallback per Exe-Name – CSV vermeidet die 25-Zeichen-Kürzung von tasklist.
function processByName(exeName) {
  return new Promise(resolve => {
    exec(`tasklist /FI "IMAGENAME eq ${exeName}" /FO CSV /NH`, (err, stdout) => {
      resolve((stdout || '').toLowerCase().includes(exeName.toLowerCase()))
    })
  })
}

// Diagnose-Protokoll der Spielzeit-Erfassung (userData/playtime-log.txt).
function playLog(msg) {
  try { fs.appendFileSync(path.join(app.getPath('userData'), 'playtime-log.txt'), `${new Date().toISOString()}  ${msg}\n`) } catch {}
}

ipcMain.handle('launch-game', async (event, gamePath, opts = {}) => {
  const useHdr = opts.useHdr !== false
  const launchArgs = tokenizeArgs(opts.args || '')
  const admin = !!opts.admin
  try {
    if (useHdr) {
      setHDR(true)
      lastHdrState = true
      hdrEnabledByLauncher = true
      mainWindow.webContents.send('hdr-status', true)
      mainWindow.webContents.send('launch-status', 'hdr-wait')
      await new Promise(resolve => setTimeout(resolve, 3000))
    }

    mainWindow.webContents.send('launch-status', 'launching')

    const isLnk = gamePath.toLowerCase().endsWith('.lnk')
    const isXbox = /\\XboxGames\\/i.test(gamePath)
    const steamInfo = isXbox ? null : await steamAppIdForExe(gamePath)   // { appId, main } oder null

    if (isXbox) {
      // Xbox/Game Pass (UWP) über die AUMID starten – die Exe ist direkt gesperrt
      // ("Zugriff verweigert"). AUMID via Get-StartApps.
      const aumid = await xboxAumidForGame(gamePath)
      if (aumid) {
        exec(`explorer.exe "shell:appsFolder\\${aumid}"`)
      } else {
        try { spawn(gamePath, launchArgs, { detached: true, stdio: 'ignore', cwd: path.dirname(gamePath) }).unref() } catch {}
      }
    } else if (steamInfo && steamInfo.appId && !admin) {
      // Steam-Spiel ÜBER Steam starten – vermeidet die DRM-Fehlermeldung
      // („User has not permission to run this product") und sorgt dafür, dass
      // Steam das Spiel als laufend führt (Running=1 → korrekte Spielzeit).
      const url = launchArgs.length
        ? `steam://run/${steamInfo.appId}//${encodeURIComponent(launchArgs.join(' '))}`
        : `steam://rungameid/${steamInfo.appId}`
      shell.openExternal(url)
    } else if (isLnk) {
      shell.openPath(gamePath)
    } else if (admin) {
      // Elevated über PowerShell Start-Process -Verb RunAs (löst UAC aus)
      const psFile = gamePath.replace(/'/g, "''")
      const psCwd = path.dirname(gamePath).replace(/'/g, "''")
      const psArgList = launchArgs.length
        ? ` -ArgumentList ${launchArgs.map(a => `'${a.replace(/'/g, "''")}'`).join(',')}`
        : ''
      spawn('powershell', ['-NoProfile', '-Command',
        `Start-Process -FilePath '${psFile}' -WorkingDirectory '${psCwd}' -Verb RunAs${psArgList}`],
        { detached: true, stdio: 'ignore' }).unref()
    } else {
      const gameProcess = spawn(gamePath, launchArgs, { detached: true, stdio: 'ignore', cwd: path.dirname(gamePath) })
      gameProcess.unref()
    }

    const launchTs = Date.now()
    const gameDir = path.dirname(gamePath)
    const exeName = await resolveProcessName(gamePath)
    playLog(`LAUNCH ${exeName} kind=${isXbox ? 'xbox' : (steamInfo ? 'steam' : (isLnk ? 'lnk' : 'direct'))} appid=${(steamInfo && steamInfo.appId) || '-'}`)

    // „Läuft das Spiel?" – Steam-Registry (zuverlässig für Steam-Spiele) ODER ein
    // Prozess aus dem Spielordner ODER der Exe-Name. Deckt DRM, DRM-frei,
    // Launcher-Exes, Handoff und lange Exe-Namen ab.
    const probeRunning = async () => {
      // 1) Steam-Registry (leicht, zuverlässig für Steam-Spiele)
      if (steamInfo && steamInfo.appId) {
        const r = await steamAppRunning(steamInfo.appId)
        if (r === true) return true
      }
      // 2) Exe-Name via tasklist CSV (leicht – deckt den Normalfall ab)
      if (await processByName(exeName)) return true
      // 3) Ordner-Prozess (robuster Fallback: Launcher-Exes, anderer Prozessname,
      //    DRM-frei direkt gestartet) – nur wenn 1+2 nichts fanden.
      if (!isLnk && await anyProcessInFolder(gameDir)) return true
      return false
    }

    const endSession = (startTs) => {
      if (useHdr) {
        setHDR(false)
        lastHdrState = false
        hdrEnabledByLauncher = false
        mainWindow.webContents.send('hdr-status', false)
      }
      if (startTs) mainWindow.webContents.send('play-session', { gamePath, durationMs: Date.now() - startTs })
      mainWindow.webContents.send('launch-status', 'idle')
    }

    // Erst auf den echten Spielstart warten (Steam braucht ein paar Sekunden),
    // Spielzeit ab dann zählen; Ende erst nach 2 leeren Checks (gegen kurze Lücken).
    let started = false, startTs = null, absent = 0, reenabled = false
    const monitor = setInterval(async () => {
      const running = await probeRunning()
      if (!started) {
        if (running) {
          started = true; startTs = Date.now(); absent = 0
          mainWindow.webContents.send('launch-status', 'running')
          playLog(`STARTED nach +${Math.round((Date.now() - launchTs) / 1000)}s`)
        } else {
          const waited = Date.now() - launchTs
          // Nach 30 s ohne Erkennung den Button wieder freigeben, damit der Nutzer
          // nicht bei „Spiel wird gestartet" festhängt (z.B. Spiel sofort wieder
          // geschlossen). Der Monitor laeuft im Hintergrund weiter – taucht das
          // Spiel doch noch auf (schwere Titel), wird ab dann normal getrackt.
          if (!reenabled && waited > 30000) {
            reenabled = true
            mainWindow.webContents.send('launch-status', 'idle')
          }
          if (waited > 120000) {    // 2 Min endgueltig aufgeben
            clearInterval(monitor)
            playLog(`TIMEOUT – nie erkannt nach ${Math.round(waited / 1000)}s.  ` +
              `reg=${(steamInfo && steamInfo.appId) ? await steamAppRunning(steamInfo.appId) : 'n/a'}  ` +
              `name=${await processByName(exeName)}  folder=${isLnk ? 'n/a' : await anyProcessInFolder(gameDir)}`)
            endSession(null)   // HDR ggf. wieder aus; sendet erneut 'idle' (harmlos)
          }
        }
      } else if (running) {
        absent = 0
      } else if (++absent >= 2) {                       // ~8 s nicht mehr da → wirklich beendet
        clearInterval(monitor)
        playLog(`ENDED – Dauer ${Math.round((Date.now() - startTs) / 1000)}s`)
        endSession(startTs)
      }
    }, 4000)

    return { success: true }
  } catch (err) {
    if (useHdr) {
      setHDR(false)
      hdrEnabledByLauncher = false
    }
    mainWindow.webContents.send('launch-status', 'idle')
    return { success: false, error: err.message }
  }
})

function resolveProcessName(gamePath) {
  // Für .lnk: versuche per PowerShell das Ziel aufzulösen
  return new Promise((resolve) => {
    if (!gamePath.toLowerCase().endsWith('.lnk')) {
      resolve(path.basename(gamePath))
      return
    }
    exec(`powershell -NoProfile -Command "$s=(New-Object -COM WScript.Shell).CreateShortcut('${gamePath}');$s.TargetPath"`, (err, stdout) => {
      const target = stdout.trim()
      resolve(target ? path.basename(target) : path.basename(gamePath, '.lnk') + '.exe')
    })
  })
}

ipcMain.handle('browse-game', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    title: 'Spiel auswählen',
    filters: [
      { name: 'Spiele', extensions: ['exe', 'lnk'] },
      { name: 'Alle Dateien', extensions: ['*'] }
    ],
    properties: ['openFile']
  })
  if (!result.canceled) return result.filePaths[0]
  return null
})

// ─── Game Scanner ────────────────────────────────────────────

function regQuery(key, value) {
  return new Promise(resolve => {
    exec(`reg query "${key}" /v "${value}"`, (err, stdout) => {
      const match = stdout && stdout.match(/REG_\w+\s+(.+)/)
      resolve(match ? match[1].trim() : null)
    })
  })
}

// Liest einen kompletten Registry-Teilbaum in einem Aufruf (für große Bäume).
function regQueryTree(key) {
  return new Promise(resolve => {
    exec(`reg query "${key}" /s`, { maxBuffer: 1024 * 1024 * 24 }, (err, stdout) => resolve(stdout || ''))
  })
}

// Listet die direkten Unterschlüssel eines Registry-Schlüssels auf.
function regSubkeys(key) {
  return new Promise(resolve => {
    exec(`reg query "${key}"`, (err, stdout) => {
      if (err || !stdout) return resolve([])
      const norm = s => s.replace(/^HKEY_LOCAL_MACHINE/i, 'HKLM').replace(/^HKEY_CURRENT_USER/i, 'HKCU')
      const baseUpper = key.toUpperCase()
      const subs = stdout.split(/\r?\n/).map(l => l.trim())
        .filter(l => /^HKEY_/i.test(l) && norm(l).toUpperCase() !== baseUpper)
      resolve(subs)
    })
  })
}

function parseVdfValue(content, key) {
  const m = content.match(new RegExp(`"${key}"\\s+"([^"]+)"`, 'i'))
  return m ? m[1] : null
}

function parseVdfLibraryPaths(content) {
  const paths = []
  const re = /"path"\s+"([^"]+)"/gi
  let m
  while ((m = re.exec(content)) !== null) paths.push(m[1].replace(/\\\\/g, '\\'))
  return paths
}

const SKIP_EXE_RE = /\b(setup|install\w*|uninst|redist|vcredist|dotnet|dxsetup|directx|crash|report|handler|helper|register|physx|oalinst|vc_redist|launcher(?!.*game)|creativeengine|anticheat|easyanticheat|battleye|beservice|touchup|cleanup|activation)\b/i
const SKIP_DIR_RE = /^(redist|_commonredist|directx|support|crash|logs|__installer|temp|bin32$|appdata|prerequisites)\b/i

function findMainExe(gameDir, gameName) {
  function collectExes(dir, depth) {
    if (depth > 2) return []
    let exes = []
    try {
      for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
        if (e.isFile() && e.name.toLowerCase().endsWith('.exe') && !SKIP_EXE_RE.test(e.name)) {
          const full = path.join(dir, e.name)
          try { exes.push({ name: e.name, full, size: fs.statSync(full).size }) } catch {}
        } else if (e.isDirectory() && !SKIP_DIR_RE.test(e.name) && depth < 2) {
          exes = exes.concat(collectExes(path.join(dir, e.name), depth + 1))
        }
      }
    } catch {}
    return exes
  }

  const exes = collectExes(gameDir, 0)
  if (!exes.length) return null

  const nameBase = (gameName || path.basename(gameDir)).toLowerCase().replace(/[^a-z0-9]/g, '')
  const scored = exes.map(e => {
    const exeBase = e.name.toLowerCase().replace(/\.exe$/, '').replace(/[^a-z0-9]/g, '')
    let score = e.size
    if (exeBase === nameBase) score += 1e12
    else if (nameBase.includes(exeBase) || exeBase.includes(nameBase)) score += 1e9
    return { ...e, score }
  })
  scored.sort((a, b) => b.score - a.score)
  return scored[0].full
}

async function scanSteam() {
  try {
    const steamPath = await regQuery('HKCU\\Software\\Valve\\Steam', 'SteamPath')
    if (!steamPath) return []
    const libFile = path.join(steamPath, 'steamapps', 'libraryfolders.vdf')
    if (!fs.existsSync(libFile)) return []
    const libContent = fs.readFileSync(libFile, 'utf8')
    const libraryPaths = [
      path.join(steamPath, 'steamapps'),
      ...parseVdfLibraryPaths(libContent).map(p => path.join(p, 'steamapps'))
    ]
    const results = []
    for (const libPath of libraryPaths) {
      if (!fs.existsSync(libPath)) continue
      for (const f of fs.readdirSync(libPath).filter(f => f.startsWith('appmanifest_') && f.endsWith('.acf'))) {
        try {
          const content = fs.readFileSync(path.join(libPath, f), 'utf8')
          const name = parseVdfValue(content, 'name')
          const installdir = parseVdfValue(content, 'installdir')
          if (!name || !installdir) continue
          const gameDir = path.join(libPath, 'common', installdir)
          if (!fs.existsSync(gameDir)) continue
          const exe = findMainExe(gameDir, name)
          if (exe) results.push({ name, path: exe, source: 'Steam' })
        } catch {}
      }
    }
    return results
  } catch { return [] }
}

async function scanEpic() {
  try {
    const manifestDir = 'C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests'
    if (!fs.existsSync(manifestDir)) return []
    const results = []
    for (const f of fs.readdirSync(manifestDir).filter(f => f.endsWith('.item'))) {
      try {
        const data = JSON.parse(fs.readFileSync(path.join(manifestDir, f), 'utf8'))
        if (!data.bIsApplication || data.bIsIncompleteInstall) continue
        const { DisplayName: name, InstallLocation, LaunchExecutable } = data
        if (!name || !InstallLocation) continue
        const exePath = LaunchExecutable
          ? path.join(InstallLocation, LaunchExecutable)
          : findMainExe(InstallLocation, name)
        if (exePath && fs.existsSync(exePath)) results.push({ name, path: exePath, source: 'Epic' })
      } catch {}
    }
    return results
  } catch { return [] }
}

async function scanFolder(folderPath) {
  const results = []
  try {
    for (const e of fs.readdirSync(folderPath, { withFileTypes: true })) {
      if (!e.isDirectory() || SKIP_DIR_RE.test(e.name)) continue
      const gameDir = path.join(folderPath, e.name)
      const exe = findMainExe(gameDir, e.name)
      if (exe) results.push({ name: e.name, path: exe, source: 'Ordner' })
    }
  } catch {}
  return results
}

async function scanGOG() {
  const results = []
  const seen = new Set()
  try {
    const bases = ['HKLM\\SOFTWARE\\WOW6432Node\\GOG.com\\Games', 'HKLM\\SOFTWARE\\GOG.com\\Games']
    for (const base of bases) {
      for (const sub of await regSubkeys(base)) {
        const name = await regQuery(sub, 'gameName')
        if (!name) continue
        let exe = await regQuery(sub, 'exe')
        const dir = await regQuery(sub, 'path')
        if (exe && !exe.includes('\\') && dir) exe = path.join(dir, exe)
        if ((!exe || !fs.existsSync(exe)) && dir && fs.existsSync(dir)) exe = findMainExe(dir, name)
        if (exe && fs.existsSync(exe) && !seen.has(exe.toLowerCase())) {
          seen.add(exe.toLowerCase())
          results.push({ name, path: exe, source: 'GOG' })
        }
      }
    }
  } catch {}
  return results
}

async function scanUbisoft() {
  const results = []
  try {
    const base = 'HKLM\\SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs'
    for (const sub of await regSubkeys(base)) {
      const dir = await regQuery(sub, 'InstallDir')
      if (!dir || !fs.existsSync(dir)) continue
      const name = path.basename(dir.replace(/[\\/]+$/, ''))
      const exe = findMainExe(dir, name)
      if (exe) results.push({ name, path: exe, source: 'Ubisoft' })
    }
  } catch {}
  return results
}

// Alle vorhandenen Laufwerksbuchstaben (C..Z), die tatsächlich existieren.
function driveLetters() {
  const out = []
  for (let c = 67; c <= 90; c++) {
    const d = String.fromCharCode(c)
    try { if (fs.existsSync(`${d}:\\`)) out.push(d) } catch {}
  }
  return out.length ? out : ['C']
}

async function scanXbox() {
  // Game Pass für PC installiert nach <Laufwerk>:\XboxGames\<Name>\Content
  const results = []
  try {
    for (const d of driveLetters()) {
      const root = `${d}:\\XboxGames`
      if (!fs.existsSync(root)) continue
      for (const e of fs.readdirSync(root, { withFileTypes: true })) {
        if (!e.isDirectory()) continue
        const content = path.join(root, e.name, 'Content')
        const dir = fs.existsSync(content) ? content : path.join(root, e.name)
        const exe = findMainExe(dir, e.name)
        if (exe) results.push({ name: e.name, path: exe, source: 'Xbox' })
      }
    }
  } catch {}
  return results
}

async function scanEA() {
  // Best-Effort über die üblichen Installationsordner von EA/Origin
  const results = []
  const roots = []
  for (const d of driveLetters()) {
    roots.push(
      `${d}:\\Program Files\\EA Games`,
      `${d}:\\Program Files (x86)\\EA Games`,
      `${d}:\\Program Files\\Origin Games`,
      `${d}:\\Program Files (x86)\\Origin Games`,
      `${d}:\\EA Games`,
    )
  }
  for (const root of roots) {
    if (!fs.existsSync(root)) continue
    try {
      for (const e of fs.readdirSync(root, { withFileTypes: true })) {
        if (!e.isDirectory() || SKIP_DIR_RE.test(e.name)) continue
        const exe = findMainExe(path.join(root, e.name), e.name)
        if (exe) results.push({ name: e.name, path: exe, source: 'EA' })
      }
    } catch {}
  }
  return results
}

async function scanRockstar() {
  const results = []
  const skip = /^(launcher|social club|rockstar games launcher)$/i
  const seen = new Set()
  try {
    const bases = ['HKLM\\SOFTWARE\\WOW6432Node\\Rockstar Games', 'HKLM\\SOFTWARE\\Rockstar Games']
    for (const base of bases) {
      for (const sub of await regSubkeys(base)) {
        const name = path.basename(sub.replace(/[\\/]+$/, ''))
        if (skip.test(name)) continue
        const dir = await regQuery(sub, 'InstallFolder')
        if (!dir || !fs.existsSync(dir)) continue
        const exe = findMainExe(dir, name)
        if (exe && !seen.has(exe.toLowerCase())) {
          seen.add(exe.toLowerCase())
          results.push({ name, path: exe, source: 'Rockstar' })
        }
      }
    }
  } catch {}
  return results
}

async function scanBattleNet() {
  // Blizzard-Spiele über die Windows-Uninstall-Einträge (Publisher = Blizzard),
  // in einem einzigen Registry-Tree-Query je Hive (statt hunderter Einzelabfragen).
  const results = []
  const seen = new Set()
  try {
    const bases = [
      'HKLM\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall',
      'HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall',
    ]
    for (const base of bases) {
      const out = await regQueryTree(base)
      for (const blk of out.split(/\r?\n(?=HKEY_)/)) {
        if (!/blizzard/i.test(blk)) continue
        const pub = blk.match(/Publisher\s+REG_SZ\s+(.+)/i)
        if (!pub || !/blizzard/i.test(pub[1])) continue
        const name = (blk.match(/DisplayName\s+REG_SZ\s+(.+)/i) || [])[1]
        const dir = (blk.match(/InstallLocation\s+REG_SZ\s+(.+)/i) || [])[1]
        const cleanDir = dir && dir.trim()
        if (!cleanDir || !fs.existsSync(cleanDir)) continue
        if (name && /battle\.?net|agent/i.test(name)) continue
        const exe = findMainExe(cleanDir, (name && name.trim()) || path.basename(cleanDir))
        if (exe && !seen.has(exe.toLowerCase())) {
          seen.add(exe.toLowerCase())
          results.push({ name: (name && name.trim()) || path.basename(cleanDir), path: exe, source: 'Battle.net' })
        }
      }
    }
  } catch {}
  return results
}

async function scanFolderRoots(roots, source) {
  const results = []
  for (const root of roots) {
    if (!fs.existsSync(root)) continue
    try {
      for (const e of fs.readdirSync(root, { withFileTypes: true })) {
        if (!e.isDirectory() || SKIP_DIR_RE.test(e.name)) continue
        const exe = findMainExe(path.join(root, e.name), e.name)
        if (exe) results.push({ name: e.name, path: exe, source })
      }
    } catch {}
  }
  return results
}

async function scanAmazon() {
  const roots = []
  for (const d of driveLetters()) roots.push(`${d}:\\Amazon Games\\Library`)
  roots.push(path.join(process.env.USERPROFILE || 'C:\\', 'Amazon Games', 'Library'))
  return scanFolderRoots(roots, 'Amazon')
}

async function scanRiot() {
  const roots = []
  for (const d of driveLetters()) roots.push(`${d}:\\Riot Games`, `${d}:\\Program Files\\Riot Games`)
  return scanFolderRoots(roots, 'Riot')
}

ipcMain.handle('scan-games', async (event, extraFolders = []) => {
  const [steam, epic, gog, ubi, xbox, ea, rockstar, battlenet, amazon, riot] = await Promise.all([
    scanSteam(), scanEpic(), scanGOG(), scanUbisoft(), scanXbox(), scanEA(), scanRockstar(),
    scanBattleNet(), scanAmazon(), scanRiot()
  ])
  const folderResults = []
  for (const folder of extraFolders) folderResults.push(...await scanFolder(folder))

  // Nach Pfad deduplizieren (ein Spiel kann über mehrere Quellen auftauchen)
  const all = [...steam, ...epic, ...gog, ...ubi, ...xbox, ...ea, ...rockstar, ...battlenet, ...amazon, ...riot, ...folderResults]
  const seen = new Set()
  const deduped = []
  for (const g of all) {
    const k = g.path.toLowerCase()
    if (seen.has(k)) continue
    seen.add(k)
    deduped.push(g)
  }
  return deduped
})

ipcMain.handle('browse-scan-folder', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    title: 'Ordner scannen',
    properties: ['openDirectory']
  })
  if (!result.canceled) return result.filePaths[0]
  return null
})

ipcMain.handle('minimize-window', () => mainWindow.minimize())
ipcMain.handle('toggle-maximize', () => {
  if (mainWindow.isMaximized()) mainWindow.restore()
  else mainWindow.maximize()
})
ipcMain.handle('close-window', () => mainWindow.close())

// ─── Medien (Cover/Icons als Dateien statt im localStorage) ──

ipcMain.handle('store-media', async (event, { id, kind, dataUrl }) => {
  try {
    if (!mediaDir || !dataUrl) return null
    const m = /^data:(image\/[\w+.-]+);base64,(.+)$/.exec(dataUrl)
    if (!m) return null
    const ext = m[1].split('/')[1].replace('jpeg', 'jpg').replace('x-icon', 'ico').replace('svg+xml', 'svg')
    // alte Varianten desselben Spiels/Typs entfernen – sowohl den festen Namen
    // (`<id>-<kind>.ext`) als auch frühere versionierte (`<id>-<kind>-<stamp>.ext`).
    try {
      for (const f of fs.readdirSync(mediaDir)) {
        if (f.startsWith(`${id}-${kind}.`) || f.startsWith(`${id}-${kind}-`)) {
          try { fs.unlinkSync(path.join(mediaDir, f)) } catch {}
        }
      }
    } catch {}
    // Eindeutiger Dateiname pro Speicherung → neue file://-URL → KEIN Browser-Cache
    // des alten Bildes (sonst erscheint neues Cover/Hero erst nach Neustart).
    const stamp = Date.now().toString(36)
    const file = path.join(mediaDir, `${id}-${kind}-${stamp}.${ext}`)
    fs.writeFileSync(file, Buffer.from(m[2], 'base64'))
    return file
  } catch { return null }
})

ipcMain.handle('delete-media', async (event, id) => {
  try {
    if (!mediaDir || !id) return false
    for (const f of fs.readdirSync(mediaDir)) {
      if (f.startsWith(`${id}-`)) { try { fs.unlinkSync(path.join(mediaDir, f)) } catch {} }
    }
  } catch {}
  return true
})

ipcMain.handle('open-game-folder', (event, p) => {
  try { shell.showItemInFolder(p) } catch {}
})

// App-Version (aus package.json) für die Anzeige im UI
ipcMain.on('get-version-sync', (event) => { event.returnValue = app.getVersion() })

// Dauerhafte Speicherung der Spieleliste als Datei (zuverlässiger als file://-localStorage)
ipcMain.on('load-games-sync', (event) => {
  try { event.returnValue = stripBom(fs.readFileSync(gamesFile, 'utf8')) }
  catch { event.returnValue = null }
})

ipcMain.handle('save-games', (event, json) => {
  try { fs.writeFileSync(gamesFile, json) } catch {}
})

ipcMain.on('load-prefs-sync', (event) => {
  try { event.returnValue = stripBom(fs.readFileSync(prefsFile, 'utf8')) }
  catch { event.returnValue = null }
})

ipcMain.handle('save-prefs', (event, json) => {
  try { fs.writeFileSync(prefsFile, json) } catch {}
})

ipcMain.handle('get-app-settings', () => appSettings)

ipcMain.handle('set-app-settings', (event, partial) => {
  const prevSource = appSettings.osdFpsSource
  Object.assign(appSettings, partial)
  saveAppSettings()
  applyAutostart()
  if (appSettings.minimizeToTray) createTray()
  else destroyTray()
  // OSD live nachziehen, wenn eine OSD-Einstellung dabei war.
  if (partial && Object.keys(partial).some(k => k.startsWith('osd'))) {
    if (appSettings.osdEnabled) showOverlay()
    else hideOverlay()
    applyOsdConfig()
    // NUR bei echtem Quellenwechsel neu waehlen. Sonst wuerde jede Slider-
    // Bewegung stopFps/startFps ausloesen -> wanted flattert 0/1 -> der Broker
    // erwischt ein wanted=0 und beendet sich (genau der "…"-Bug).
    if (appSettings.osdFpsSource !== prevSource) stopFps()
    syncFps()
  }
  return appSettings
})

// ---------------------------------------------------------------------------
// Auto-Update (electron-updater, generischer Feed auf dem eigenen Webserver)
// Ablauf: beim Start still pruefen -> bei neuer Version den Nutzer FRAGEN ->
// erst auf dessen Wunsch herunterladen und installieren.
// ---------------------------------------------------------------------------
let updateManualCheck = false   // true = vom Nutzer manuell ausgeloest (dann auch "keine Updates"/Fehler zeigen)

function sendToUi(channel, payload) {
  if (mainWindow && !mainWindow.isDestroyed()) mainWindow.webContents.send(channel, payload)
}

function setupAutoUpdate() {
  if (!autoUpdater) return
  autoUpdater.autoDownload = false           // nie ungefragt laden
  autoUpdater.autoInstallOnAppQuit = false   // Installation steuern wir selbst

  autoUpdater.on('update-available', (info) => {
    // releaseNotes kann String ODER Liste ({version, note}) sein – beides zu Text.
    let notes = ''
    if (typeof info.releaseNotes === 'string') notes = info.releaseNotes
    else if (Array.isArray(info.releaseNotes)) notes = info.releaseNotes.map(n => (n && n.note) || '').join('\n\n')
    sendToUi('update-available', { version: info.version, notes })
  })
  autoUpdater.on('update-not-available', () => {
    if (updateManualCheck) sendToUi('update-none', {})
    updateManualCheck = false
  })
  autoUpdater.on('download-progress', (p) => {
    sendToUi('update-progress', { percent: Math.round(p.percent || 0) })
  })
  autoUpdater.on('update-downloaded', (info) => {
    sendToUi('update-ready', { version: info.version })
  })
  autoUpdater.on('error', (err) => {
    if (updateManualCheck) sendToUi('update-error', { message: String(err && err.message || err) })
    updateManualCheck = false
  })
}

function checkForUpdates(manual) {
  if (!autoUpdater || !app.isPackaged) {   // kein Modul oder Dev-Modus: kein Update-Paket
    if (manual) sendToUi('update-none', {})
    return
  }
  updateManualCheck = !!manual
  autoUpdater.checkForUpdates().catch((err) => {
    if (manual) sendToUi('update-error', { message: String(err && err.message || err) })
    updateManualCheck = false
  })
}

// Toggle-Hotkey aus den Einstellungen setzen. Leerer String = deaktiviert.
// Liefert { ok } zurueck – false, wenn die Kombination nicht registriert werden
// konnte (z.B. schon vom System/anderer App belegt).
ipcMain.handle('set-hotkey', (event, accelerator) => {
  appSettings.toggleHotkey = accelerator || ''
  saveAppSettings()
  const ok = registerToggleHotkey()
  return { ok }
})

// Gamepad-Hotkey (Renderer erkennt die Kombi und ruft dies zum Ein-/Ausblenden).
ipcMain.handle('toggle-window', () => { toggleMainWindow() })

// OSD-Overlay ein-/ausblenden (spaeter ueber einen Button/Hotkey in der App).
ipcMain.handle('toggle-osd', () => { toggleOverlay() })

// Live-Edit: Position (Ecke) und Groesse werden im Overlay geaendert und hier
// dauerhaft gespeichert.
ipcMain.handle('osd-edit-corner', (e, corner) => {
  if (['tl', 'tr', 'bl', 'br'].includes(corner)) { appSettings.osdCorner = corner; saveAppSettings() }
})
ipcMain.handle('osd-edit-scale', (e, delta) => {
  let z = (Number(appSettings.osdScale) || 1) + (delta > 0 ? 0.05 : -0.05)
  z = Math.max(0.5, Math.min(2, Math.round(z * 100) / 100))
  appSettings.osdScale = z
  saveAppSettings()
  if (overlayWindow && !overlayWindow.isDestroyed()) { try { overlayWindow.webContents.setZoomFactor(z) } catch {} }
})
ipcMain.handle('osd-edit-done', () => setOsdEditMode(false))

// Einmalige Freigabe fuer PresentMon ohne Admin: aktuellen Nutzer in die Gruppe
// "Leistungsprotokollbenutzer" (SID S-1-5-32-559, sprachunabhaengig) aufnehmen.
// Ein UAC-Dialog; danach muss sich der Nutzer einmal ab-/anmelden.
ipcMain.handle('fps-grant-access', () => {
  const inner = 'Add-LocalGroupMember -SID S-1-5-32-559 -Member $env:USERNAME'
  const b64 = Buffer.from(inner, 'utf16le').toString('base64')
  const outer = `Start-Process powershell -Verb RunAs -WindowStyle Hidden -ArgumentList '-NoProfile -EncodedCommand ${b64}'`
  try { spawn('powershell', ['-NoProfile', '-Command', outer], { windowsHide: true }) } catch (e) { return { ok: false, error: e && e.message } }
  return { ok: true }
})

ipcMain.handle('check-for-updates', () => { checkForUpdates(true) })
ipcMain.handle('download-update', () => { if (autoUpdater) autoUpdater.downloadUpdate().catch(() => {}) })
ipcMain.handle('install-update', () => { if (autoUpdater) { app.isQuitting = true; autoUpdater.quitAndInstall() } })

// Nur eine Instanz zulassen: ein zweiter Start holt das vorhandene (ggf. im Tray
// versteckte) Fenster nach vorne, statt eine neue Instanz zu öffnen.
if (process.argv.includes('--fps-broker')) {
  // Broker-Instanz: kein Fenster, keine Single-Instance-Sperre, nur FPS messen.
  try { app.disableHardwareAcceleration() } catch {}   // schlanker (keine GPU-Prozesse)
  app.whenReady().then(runFpsBroker)
} else if (!app.requestSingleInstanceLock()) {
  app.quit()
} else {
  app.on('second-instance', () => showMainWindow())

  app.whenReady().then(() => { setupAutoUpdate(); createWindow(); setupGamepadHotkey(); registerToggleHotkey(); setupNvml(); setupAdl(); setupRtss(); setupMahm(); setupCpuClock(); setupGpuName(); if (appSettings.osdEnabled) showOverlay() })

  // HDR abschalten, falls der Launcher es für ein noch laufendes Spiel aktiviert hatte
  app.on('before-quit', () => {
    if (hdrEnabledByLauncher) {
      try { execSync(`"${getHdrCmdPath()}" off`) } catch {}
      hdrEnabledByLauncher = false
    }
  })

  app.on('will-quit', () => { globalShortcut.unregisterAll(); if (xinputPoll) clearInterval(xinputPoll); stopFps() })

  app.on('window-all-closed', () => {
    if (hdrPollInterval) clearInterval(hdrPollInterval)
    app.quit()
  })
}
