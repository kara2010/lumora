const { app, BrowserWindow, ipcMain, dialog, shell, screen, Tray, Menu, nativeImage, globalShortcut, desktopCapturer, session } = require('electron')
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

// Sicherheitsnetz: einen unerwarteten Fehler (typischerweise aus einem Kind-
// prozess der Streaming-Pipeline – FFmpeg/mediamtx) protokollieren, statt die
// ganze App abstuerzen zu lassen. Ohne diesen Handler beendet Electron den
// Hauptprozess bei jeder uncaught exception.
try {
  process.on('uncaughtException', (err) => {
    try { fs.appendFileSync(path.join(app.getPath('temp'), 'lumora-crash.log'), Date.now() + ' uncaught: ' + ((err && err.stack) || err) + '\n') } catch (e) {}
  })
  process.on('unhandledRejection', (reason) => {
    try { fs.appendFileSync(path.join(app.getPath('temp'), 'lumora-crash.log'), Date.now() + ' unhandled: ' + ((reason && reason.stack) || reason) + '\n') } catch (e) {}
  })
} catch (e) {}

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
const appSettings = { autostart: false, startMinimized: false, minimizeToTray: false, steamGridDbKey: '', toggleHotkey: 'Alt+L', gamepadHotkey: [], gamepadOsdHotkey: [],
  // OSD-Overlay: sichtbar?, Skalierung (Zoomfaktor), Ecke (tl/tr/bl/br), Panel-Deckkraft, FPS-Messung
  // OSD an = alle Werte inkl. FPS (kein separater FPS-Schalter mehr). osdSetupDeclined
  // merkt sich nur, dass der Nutzer die einmalige Einrichtung (UAC) abgelehnt hat –
  // dann fehlen FPS/CPU-Sensorwerte und wir fragen nicht staendig neu
  // ("Jetzt einrichten" im Overlay-Tab holt es nach).
  osdEnabled: false, osdScale: 1, osdCorner: 'tl', osdOpacity: 0.55, osdSetupDeclined: false, osdFpsSource: 'auto', osdHotkey: 'Alt+O', osdEditHotkey: 'Alt+Shift+O',
  // Alt+B: natives Afterburner-OSD ein-/ausblenden (RTSS-Sichtbarkeits-Bit),
  // unabhaengig vom Lumora-Panel. Siehe toggleAbOsd().
  osdAbHotkey: 'Alt+B',
  // Live-Stream (native Pipeline): konstante Encode-Bitrate in kbit/s. Bei CBR
  // begrenzt sie zugleich, was der Zuschauer empfangen muss -> mobilfunktauglich
  // waehlen. Default 12 Mbit (gutes Full-HD). Dazu Ziel-Framerate + Aufloesung.
  // streamBufferMs: Empfangspuffer der Zuschauer. 120 ms war fuer Internet-Streams
  // zu knapp - jede kleine Sende-/Netz-Schwankung schlug direkt als Ruckler durch.
  // 300 ms ist weiterhin "live" (unter einer halben Sekunde), schluckt aber Jitter.
  streamUploadKbit: 8000, streamFps: 60, streamQuality: '1080', streamBufferMs: 300, streamHotkey: '',
  streamTurnEnabled: false, streamTurnUrl: '', streamTurnUser: '', streamTurnPass: '', streamTurnForce: false,
  streamForceIPv6: false,
  language: 'auto',    // UI-Sprache: 'auto' (Systemsprache), 'de', 'en' - Uebersetzung im Renderer (i18n-Block)
  groupRelayUrl: '',   // '' = eingebauter Standard (GROUP_RELAY_DEFAULT); austauschbar fuer Selbst-Hoster
  groupLastCode: '',   // zuletzt genutzter Raumcode (Beitreten-Feld vorbefuellen)
  streamSource: '',   // '' = Hauptbildschirm bzw. 'screen:<idx>' (ddagrab-Monitor)
  // Grafikkarte fuer die OSD-Sensoren: 'auto' (schnellste automatisch) oder feste ID 'nvml:<idx>' / 'adl:<idx>'
  osdGpu: 'auto',
  // Anzeige-Art des OSD: 'window' = eigenes Overlay-Fenster (Standard),
  // 'rtss' = IM Spiel gerendert via RTSS (streambar, exklusives Vollbild ok)
  osdRenderer: 'window',
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
  // Einmalige Migration (2.2.10): 120 ms war der alte Standard-Empfangspuffer -
  // nachweislich zu knapp (belegte Sender-Spitzen von 175-515 ms schlugen beim
  // Zuschauer als "Verschlucken" durch). Wer noch exakt auf dem alten Default
  // steht, wird auf den neuen (300 ms) gehoben; bewusst abweichend eingestellte
  // Werte bleiben unangetastet, der Regler bleibt frei bedienbar.
  if (appSettings.streamBufferMs === 120) appSettings.streamBufferMs = 300
}

function saveAppSettings() {
  try { fs.writeFileSync(appSettingsFile, JSON.stringify(appSettings)) } catch {}
}

function applyAutostart() {
  cleanupLegacyAutostart()
  app.setLoginItemSettings({
    openAtLogin: !!appSettings.autostart,
    args: appSettings.startMinimized ? ['--minimized'] : [],
  })
}
// Rebrand-Altlast: VOR dem Namenswechsel „HDR Launcher" -> „Lumora" legte die App
// unter dem alten Namen einen Autostart-Eintrag „electron.app.HDR Launcher" an.
// Den kennt die heutige App nicht (setLoginItemSettings verwaltet nur den Eintrag
// zum aktuellen app.name), also bleibt er liegen und startet bei jedem Login die
// uralte 1.3.0 aus dem alten Ordner — trotz Update auf Lumora. Einmal je Start
// gezielt entfernen (idempotent; schadet nicht, wenn er schon weg ist).
let _legacyAutostartCleaned = false
function cleanupLegacyAutostart() {
  if (_legacyAutostartCleaned || process.platform !== 'win32') return
  _legacyAutostartCleaned = true
  try {
    require('child_process').execFile('reg',
      ['delete', 'HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run', '/v', 'electron.app.HDR Launcher', '/f'],
      { windowsHide: true }, () => {})
  } catch {}
}

// HWND eines Electron-Fensters als Zahl (0 wenn weg) – fuer Foreground-Vergleiche.
function nativeHwnd(win) {
  try { return win && !win.isDestroyed() ? Number(win.getNativeWindowHandle().readBigUInt64LE(0)) : 0 } catch { return 0 }
}

// Gibt den Vordergrund EXPLIZIT an das Fenster zurueck, dem wir ihn beim Hoch-
// holen genommen haben (i.d.R. das laufende Spiel). Nur minimize()/hide() reicht
// insbesondere GDK-/Xbox-Spielen (Forza & Co.) nicht: Windows aktiviert dann zwar
// irgendein Fenster, aber ohne echten Foreground-Wechsel bleibt das Spiel halb
// aktiv haengen – das Menue reagiert erst wieder nach einem manuellen Alt+Tab.
// Dieser Aufruf ist genau dieses "Alt+Tab" in Software. Wichtig: Er muss laufen,
// SOLANGE Lumora noch das Vordergrundfenster ist – nur dann erlaubt Windows
// einem Prozess, den Vordergrund weiterzureichen.
function restoreGameFocus() {
  try {
    if (!fgWin || !prevGameHwnd) return
    if (!fgWin.isWin(prevGameHwnd)) { prevGameHwnd = 0; return }   // Spiel inzwischen beendet
    // Hat der erzwungene Wechsel das Vollbild-Spiel minimiert (Windows kickt
    // exklusives Vollbild beim Vordergrund-Verlust)? Dann erst wiederherstellen.
    if (fgWin.isIconic(prevGameHwnd)) fgWin.showWin(prevGameHwnd, 9 /*SW_RESTORE*/)
    const ok = fgWin.set(prevGameHwnd)
    osdDbg('[focus] Vordergrund-Rueckgabe an hwnd=' + prevGameHwnd + ' ok=' + ok)
  } catch {}
}

function showMainWindow() {
  // Fenster weg/zerstoert (z.B. wurde geschlossen, App lief noch)? Neu aufbauen,
  // statt auf einem toten Objekt zu operieren ("Object has been destroyed").
  if (!mainWindow || mainWindow.isDestroyed()) { createWindow(); return }
  // Merken, wem wir gleich den Fokus wegnehmen (z.B. dem laufenden Spiel) –
  // beim Verstecken gibt restoreGameFocus() ihn exakt dorthin zurueck.
  try {
    if (fgWin) {
      const fg = Number(fgWin.get())
      if (fg && fg !== nativeHwnd(mainWindow) && fg !== nativeHwnd(overlayWindow)) prevGameHwnd = fg
    }
  } catch {}
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
  // Kontrolle: Sind wir nach dem sanften Anheben WIRKLICH vorn? Vollbild-Spiele
  // mit Input-Besitz (z.B. das Forza-Intro) blocken es ueber die Windows-
  // Foreground-Sperre – ohne sichtbare Reaktion (Taskleiste versteckt), Lumora
  // wirkt dann "abgestuerzt". Dann: Sperre per synthetischem Alt-Tipp loesen und
  // den Vordergrund explizit erzwingen. Greift NUR im Fehlerfall.
  setTimeout(() => {
    try {
      if (!fgWin || !mainWindow || mainWindow.isDestroyed()) return
      const own = nativeHwnd(mainWindow)
      if (!own || Number(fgWin.get()) === own) return   // sanfter Weg hat gereicht
      osdDbg('[focus] sanftes Anheben geblockt -> erzwinge Vordergrund (Alt-Trick)')
      fgWin.kbd(0x12, 0, 0, 0); fgWin.kbd(0x12, 0, 2 /*KEYUP*/, 0)
      fgWin.set(own)
      mainWindow.show()
      mainWindow.focus()
    } catch {}
  }, 250)
}

// Globaler Hotkey: holt Lumora nach vorne bzw. versteckt es wieder (Toggle).
function toggleMainWindow() {
  if (!mainWindow) return
  if (mainWindow.isVisible() && !mainWindow.isMinimized() && mainWindow.isFocused()) {
    // ERST dem Spiel den Vordergrund sauber zurueckgeben (nur jetzt, als
    // Vordergrund-Prozess, duerfen wir das), DANN verstecken.
    restoreGameFocus()
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
  add(appSettings.osdHotkey, toggleOverlay)          // leer -> parseAccel null -> aus
  add(appSettings.osdEditHotkey, toggleOsdEdit)
  add(appSettings.osdAbHotkey, toggleAbOsd)
  add(appSettings.streamHotkey, toggleBroadcastHotkey)   // Stream/Freigabe schnell toggeln (auch im Spiel)
}
// (Neu-)Registriert die Hotkeys. Ist der native Poll (GetAsyncKeyState) aktiv,
// uebernimmt der die Tastatur-Hotkeys – auch im Spiel. Nur wenn der nicht
// verfuegbar ist, faellt es auf globalShortcut zurueck (greift NICHT im Vollbild).
function registerToggleHotkey() {
  globalShortcut.unregisterAll()
  rebuildHotkeys()
  if (getAsyncKey) return true   // Poll erledigt die Tastatur-Hotkeys
  if (appSettings.osdHotkey) try { globalShortcut.register(appSettings.osdHotkey, toggleOverlay) } catch {}
  if (appSettings.osdEditHotkey) try { globalShortcut.register(appSettings.osdEditHotkey, toggleOsdEdit) } catch {}
  if (appSettings.osdAbHotkey) try { globalShortcut.register(appSettings.osdAbHotkey, toggleAbOsd) } catch {}
  if (appSettings.streamHotkey) try { globalShortcut.register(appSettings.streamHotkey, toggleBroadcastHotkey) } catch {}
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
      count:  lib.func('int nvmlDeviceGetCount_v2(_Out_ uint32* count)'),
      handle: lib.func('int nvmlDeviceGetHandleByIndex_v2(uint32 index, _Out_ nvmlDevice_t* device)'),
      name:   lib.func('int nvmlDeviceGetName(nvmlDevice_t device, _Out_ char* name, uint32 length)'),
      util:   lib.func('int nvmlDeviceGetUtilizationRates(nvmlDevice_t device, _Out_ nvmlUtilization_t* u)'),
      temp:   lib.func('int nvmlDeviceGetTemperature(nvmlDevice_t device, uint32 sensor, _Out_ uint32* t)'),
      power:  lib.func('int nvmlDeviceGetPowerUsage(nvmlDevice_t device, _Out_ uint32* mw)'),
      clock:  lib.func('int nvmlDeviceGetClockInfo(nvmlDevice_t device, uint32 type, _Out_ uint32* mhz)'),
      mem:    lib.func('int nvmlDeviceGetMemoryInfo(nvmlDevice_t device, _Out_ nvmlMemory_t* m)'),
    }
    if (f.init() !== 0) throw new Error('nvmlInit_v2 fehlgeschlagen')
    // Alle NVIDIA-Karten aufzaehlen (fuer die manuelle GPU-Auswahl). Aktive =
    // Index 0 (bei einer dGPU immer diese; NVIDIA hat praktisch keine iGPUs).
    const cnt = [0]; try { f.count(cnt) } catch {}   // fehlt das Symbol -> nur Index 0
    const n = Math.max(1, cnt[0] || 1)
    const devices = []
    for (let i = 0; i < n; i++) {
      const dev = [null]
      if (f.handle(i, dev) !== 0) continue
      const nameBuf = Buffer.alloc(96)
      f.name(dev[0], nameBuf, 96)
      const model = nameBuf.toString('utf8').split('\0')[0].replace(/^NVIDIA\s+/i, '').replace(/GeForce\s+/i, '').trim()
      devices.push({ index: i, handle: dev[0], model })
    }
    if (!devices.length) throw new Error('keine NVIDIA-Geraete')
    nvml = { f, devices, device: devices[0].handle, model: devices[0].model }
    console.log('[osd] NVML aktiv:', devices.map(d => '#' + d.index + ' ' + d.model).join(', '))
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
    let found = null; const seen = []; const amdIdxs = []; const amdNames = {}
    for (let i = 0; i < num[0]; i++) {
      const o = i * INFO
      const vendor = buf.readInt32LE(o + 276), present = buf.readInt32LE(o + 792), idx = buf.readInt32LE(o + 4)
      const nz = buf.indexOf(0, o + 280); const name = buf.toString('latin1', o + 280, nz < 0 ? o + 280 : nz)
      seen.push('#' + idx + ' vendor=0x' + (vendor >>> 0).toString(16) + ' present=' + present + ' "' + name + '"')
      if (isAmd(vendor) && present) { if (!found) found = { idx, name }; if (!amdIdxs.includes(idx)) { amdIdxs.push(idx); amdNames[idx] = name } }
    }
    gpuDiag('[ADL] ' + num[0] + ' Adapter: ' + seen.join(' | '))
    adl = found ? { queryGet, ctx: ctx[0], idx: found.idx, name: found.name, _cb: mallocCb } : null
    if (adl) {
      // Je AMD-Adapter die Sensoren pruefen: liefert er ueberhaupt Werte (hasSensors),
      // und ist er eine *dedizierte* Radeon? Dedizierte melden Edge- (8) oder Hotspot-
      // Temp (27); iGPUs haben nur die GFX-Temp (28). So bevorzugen wir bei mehreren
      // AMD-Karten automatisch die schnelle dGPU. Bei nur einem Adapter bleibt alles
      // wie gehabt (erster mit Sensoren) -> kein Rueckschritt auf reinen iGPU-Systemen.
      const adapters = []
      for (const ai of amdIdxs) {
        const dump = Buffer.alloc(2052)
        if (queryGet(ctx[0], ai, dump) === 0) {
          const sup = (id) => dump.readInt32LE(4 + id * 8) !== 0
          const parts = []
          for (let id = 0; id < 256; id++) { const o = 4 + id * 8; if (dump.readInt32LE(o)) parts.push(id + '=' + dump.readInt32LE(o + 4)) }
          gpuDiag('[ADL] idx=' + ai + ' Sensoren [id=value]: ' + (parts.join(' ') || '(keine supported)'))
          adapters.push({ idx: ai, name: amdNames[ai] || found.name, hasSensors: parts.length > 0, dedicated: sup(ADL_PM.tempEdge) || sup(ADL_PM.tempHotspot) })
        } else {
          gpuDiag('[ADL] idx=' + ai + ' QueryPMLogData fehlgeschlagen')
          adapters.push({ idx: ai, name: amdNames[ai] || found.name, hasSensors: false, dedicated: false })
        }
      }
      adl.adapters = adapters.filter(a => a.hasSensors)   // nur auslesbare zur Auswahl anbieten
      const ded = adapters.find(a => a.hasSensors && a.dedicated)   // 1) dedizierte Radeon
      const anySens = adapters.find(a => a.hasSensors)              // 2) sonst erste mit Sensoren
      const pick = ded || anySens
      if (pick) { adl.idx = pick.idx; adl.name = pick.name }
      gpuDiag('[ADL] aktiver Index fuer Auslesen: ' + adl.idx + (ded ? ' (dediziert erkannt)' : ''))
      console.log('[osd] ADL aktiv:', adl.name, '(idx ' + adl.idx + ')')
    } else gpuDiag('[ADL] kein aktiver AMD-Adapter (VendorID 1002/0x1002) gefunden')
  } catch (e) { gpuDiag('[ADL] Setup-Fehler: ' + (e && e.message)); adl = null }
}
// Liest ADL-Sensoren fuer einen bestimmten Adapter-Index (name = Anzeigename).
function readAdlIdx(idx, name) {
  if (!adl) return null
  try {
    const buf = Buffer.alloc(2052)
    if (adl.queryGet(adl.ctx, idx, buf) !== 0) return null
    const val = (id) => { const o = 4 + id * 8; return buf.readInt32LE(o) ? buf.readInt32LE(o + 4) : null }
    // Temp: Edge/Hotspot (dedizierte Radeon) – fehlen bei iGPUs, dort GFX-Temp (28).
    const temp = val(ADL_PM.tempEdge) ?? val(ADL_PM.tempHotspot) ?? val(ADL_PM.tempGfx)
    const clock = val(ADL_PM.gfxclk)
    const power = val(ADL_PM.gfxPower) ?? val(ADL_PM.asicPower)
    const load = val(ADL_PM.gfxActivity)
    if (temp == null && clock == null && power == null && load == null) return null
    const u = (x) => x == null ? undefined : x
    const nm = (name || adl.name || gpuName || 'GPU').replace(/\((R|TM)\)/gi, '').replace(/^AMD\s+/i, '').replace(/\s+Graphics$/i, '').trim()
    // VRAM liefert ADL-PMLog nicht -> treiberfrei ueber den PDH-Zaehler nachreichen.
    return { brand: 'AMD', name: nm, load: u(load), temp: u(temp), power: u(power), clock: u(clock), vram: u(readPdhVramMb()) }
  } catch { return null }
}
function readAdlGpu() { return adl ? readAdlIdx(adl.idx, adl.name) : null }
// Liest NVML-Sensoren fuer ein bestimmtes Geraete-Handle.
function readNvmlHandle(handle, model) {
  if (!nvml) return null
  try {
    const { f } = nvml
    const u = {}, m = {}, t = [0], p = [0], c = [0]
    f.util(handle, u); f.temp(handle, 0, t); f.power(handle, p); f.clock(handle, 0, c); f.mem(handle, m)
    return { brand: 'NVIDIA', name: model, load: u.gpu, temp: t[0], power: +(p[0] / 1000).toFixed(1), clock: c[0], vram: Math.round(Number(m.used) / 1048576) }
  } catch { return null }
}

function readGpu() {
  const sel = appSettings.osdGpu || 'auto'
  // Feste Auswahl (manuell im Menue gesetzt). Ist die gewaehlte GPU nicht (mehr)
  // vorhanden oder liefert nichts, faellt es unten auf die Automatik zurueck.
  if (sel !== 'auto') {
    const s = /^(nvml|adl):(\d+)$/.exec(sel)
    if (s) {
      if (s[1] === 'nvml' && nvml) {
        const d = nvml.devices.find(x => x.index === +s[2])
        if (d) { const r = readNvmlHandle(d.handle, d.model); if (r) return r }
      }
      if (s[1] === 'adl' && adl) {
        const a = (adl.adapters || []).find(x => x.idx === +s[2])
        const r = readAdlIdx(+s[2], a && a.name); if (r) return r
      }
    }
  }
  // Automatik: NVIDIA (NVML) -> AMD (ADL) -> Afterburner (MAHM).
  if (nvml) { const r = readNvmlHandle(nvml.device, nvml.model); if (r) return r }
  const a = readAdlGpu()
  if (a) return a
  const m = readMahm()
  if (m && m.gpuTemp != null) {
    return { brand: 'GPU', name: gpuName, load: m.gpuLoad, temp: m.gpuTemp, power: m.gpuPower, clock: m.gpuClock, vram: m.gpuVram }
  }
  return null
}
// Alle auslesbaren GPUs fuer die manuelle Auswahl (NVIDIA via NVML, AMD via ADL).
// Fallback-GPUs (nur MAHM/WMI-Name, keine echten Sensor-Quellen) tauchen nicht auf –
// dort gibt es ohnehin nichts umzuschalten.
function listGpus() {
  const out = []
  if (nvml && nvml.devices) for (const d of nvml.devices) out.push({ id: 'nvml:' + d.index, label: d.model || 'NVIDIA' })
  if (adl && adl.adapters) for (const a of adl.adapters) {
    const nm = (a.name || 'AMD').replace(/\((R|TM)\)/gi, '').replace(/^AMD\s+/i, '').replace(/\s+Graphics$/i, '').trim()
    out.push({ id: 'adl:' + a.idx, label: nm + (a.dedicated ? '' : ' (Onboard)') })
  }
  return out
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
  // Temp/Takt/Power: Kette Afterburner (falls aktiv) -> PawnIO-Sensor-Broker
  // (falls eingerichtet) -> nur PDH-Takt. Fehlende Werte blendet das OSD aus.
  const m = readMahm()
  let temp = null, clock = null, power = null
  if (m && m.cpuTemp != null) { temp = m.cpuTemp; clock = m.cpuClock; power = m.cpuPower }
  else {
    const p = readSenseCpu()
    if (p) { temp = p.temp; power = p.power }
    clock = readCpuClockMhz()
  }
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
// WICHTIG: Lumora selbst wird von RTSS gehookt (Chromium praesentiert per DXGI).
// Ohne Selbst-Ausschluss gewinnt abwechselnd das eigene Fenster den "zuletzt
// aktiv"-Vergleich: falsche FPS und springende OSD-Positionen (gemessen).
const RTSS_SELF_EXE = (() => { try { return require('path').basename(process.execPath).toLowerCase() } catch { return '' } })()
function rtssIsSelf(e) {
  const nm = Buffer.from(e.szName)
  const z = nm.indexOf(0)
  const bn = nm.toString('latin1', 0, z < 0 ? nm.length : z).split('\\').pop().toLowerCase()
  return bn === RTSS_SELF_EXE
}
// Das "aktive Spiel" bestimmen: RTSS pflegt selbst den zuletzt im Vordergrund
// praesentierenden Eintrag (dwLastForegroundApp @64, v2.16+) – stabil und exakt
// die richtige Semantik. "Zuletzt aktualisiert" wechselte dagegen im Sekunden-
// takt zwischen ALLEN gehookten Fenstern (Lumora selbst, Chat-Fenster, ...) und
// liess FPS-Wert und OSD-Position springen (per Slot-Log gemessen 04.07.2026).
function rtssBestApp(base, hdr, now) {
  const fresh = (e) => e.dwProcessID && e.dwFrameTime && (((now - e.dwTime1) >>> 0) <= 1500) && !rtssIsSelf(e)
  try {
    if ((hdr.dwVersion >>> 0) >= 0x20010) {
      const idx = rtss.koffi.decode(base, 64, 'uint32')
      if (idx < (hdr.dwAppArrSize >>> 0)) {
        const off = hdr.dwAppArrOffset + idx * hdr.dwAppEntrySize
        const e = rtss.koffi.decode(base, off, rtss.APP)
        if (fresh(e)) return { e, off }
      }
    }
  } catch {}
  let best = null, bestOff = 0
  const n = Math.min(hdr.dwAppArrSize, 4096)
  for (let i = 0; i < n; i++) {
    const off = hdr.dwAppArrOffset + i * hdr.dwAppEntrySize
    const e = rtss.koffi.decode(base, off, rtss.APP)
    if (!fresh(e)) continue
    if (!best || e.dwTime1 > best.dwTime1) { best = e; bestOff = off }
  }
  return best ? { e: best, off: bestOff } : null
}
function readRtssFps() {
  if (!rtss) return null
  const h = rtss.open(0x0004, 0, 'RTSSSharedMemoryV2')
  if (!h) return null
  let base = 0
  try {
    base = rtss.map(h, 0x0004, 0, 0, 0)
    if (!base) return null
    const hdr = rtss.koffi.decode(base, rtss.HDR)
    const bestA = rtssBestApp(base, hdr, rtss.ticks())
    if (!bestA) return null
    const best = bestA.e
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

// --- RTSS-OSD-Schreiber: rendert Lumoras OSD IM Spiel --------------------------
// RTSS ist ein "OSD-Server": Programme schreiben Hypertext in einen OSD-Slot des
// Shared Memory, RTSS zeichnet ihn mit seiner signierten, bei Anti-Cheats
// etablierten Injection direkt in den Spiel-Frame (gleiche Technik wie
// Afterburner/HWiNFO). Dadurch ist das OSD in Discord/OBS-Streams sichtbar und
// funktioniert auch im exklusiven Vollbild. Wir belegen einen Slot (Owner
// "Lumora"), aktualisieren dwOSDFrame und raeumen den Slot beim Deaktivieren.
let rtssSlotIdx = -1
let rtssOsdActive = false
function rtssEnc(base, off, buf) {
  rtss.koffi.encode(base, off, rtss.koffi.array('uint8', buf.length), Array.from(buf))
}
function rtssOsdMap(cb) {
  if (!rtss) return false
  const h = rtss.open(0x0006 /*READ|WRITE*/, 0, 'RTSSSharedMemoryV2')
  if (!h) return false
  let base = 0, ok = false
  try {
    base = rtss.map(h, 0x0006, 0, 0, 0)
    if (base) {
      const hdr = rtss.koffi.decode(base, rtss.HDR)
      if (hdr.dwSignature === 0x52545353 /*'RTSS'*/) ok = cb(base, hdr) !== false
    }
  } catch (e) { osdDbg('[rtss-osd] map/write: ' + (e && e.message)) }
  finally {
    if (base) { try { rtss.unmap(base) } catch {} }
    try { rtss.close(h) } catch {}
  }
  return ok
}
const RTSS_EX2_OFF = 256 + 256 + 4096 + 262144   // szOSDEx2 @266752 (Format v2.20+)
function rtssOsdWrite(mainLines) {
  const res = rtssResolution()
  return rtssOsdMap((base, hdr) => {
    const n = Math.min(hdr.dwOSDArrSize >>> 0, 32)
    const es = hdr.dwOSDEntrySize >>> 0
    const off0 = hdr.dwOSDArrOffset >>> 0
    // Unseren Slot wiederfinden, sonst ersten freien belegen (Slot 0 gehoert
    // traditionell Afterburner und bleibt tabu).
    let mine = -1, free = -1
    for (let i = 0; i < n; i++) {
      const ob = rtss.koffi.decode(base, off0 + i * es + 256, rtss.koffi.array('uint8', 16))
      const owner = Buffer.from(ob).toString('latin1').split('\0')[0]
      if (owner === 'Lumora') { if (mine < 0) mine = i; continue }
      if (!owner && free < 0 && i > 0) free = i
    }
    const slot = mine >= 0 ? mine : free
    if (slot < 0) return false
    rtssSlotIdx = slot
    const so = off0 + slot * es
    if (mine < 0) rtssEnc(base, so + 256, Buffer.from('Lumora\0', 'latin1'))
    // RTSS rendert szOSD, szOSDEx UND szOSDEx2, wenn befuellt – also exakt EIN
    // Textfeld beschreiben und die anderen leeren (sonst Doppelbild, gemessen).
    const zero = Buffer.from([0])
    if (es >= RTSS_EX2_OFF + 32768) {
      // Modernes RTSS: Layout-Feld mit freier Positionierung (siehe unten).
      rtssEnc(base, so + RTSS_EX2_OFF, Buffer.from(rtssComposeEx2(mainLines, res).slice(0, 32000) + '\0', 'latin1'))
      rtssEnc(base, so + 512, zero)
      rtssEnc(base, so, zero)
    } else if (es >= 512 + 4096) {
      rtssEnc(base, so + 512, Buffer.from(mainLines.join('\n').slice(0, 4000) + '\0', 'latin1'))
      rtssEnc(base, so, zero)
    } else {
      rtssEnc(base, so, Buffer.from(mainLines.join('\n').slice(0, 255) + '\0', 'latin1'))
    }
    const frame = rtss.koffi.decode(base, 32, 'uint32')
    rtss.koffi.encode(base, 32, 'uint32', (frame + 1) >>> 0)        // RTSS neu zeichnen lassen
    return true
  })
}
function rtssOsdClear() {
  if (rtssSlotIdx < 0) return
  rtssOsdMap((base, hdr) => {
    const es = hdr.dwOSDEntrySize >>> 0
    const so = (hdr.dwOSDArrOffset >>> 0) + rtssSlotIdx * es
    const zero = Buffer.from([0])
    rtssEnc(base, so, zero)
    rtssEnc(base, so + 256, zero)
    if (es >= 512 + 4096) rtssEnc(base, so + 512, zero)
    if (es >= RTSS_EX2_OFF + 32768) rtssEnc(base, so + RTSS_EX2_OFF, zero)
    const frame = rtss.koffi.decode(base, 32, 'uint32')
    rtss.koffi.encode(base, 32, 'uint32', (frame + 1) >>> 0)
  })
  rtssSlotIdx = -1
}

// RTSS-Profil-API (RTSSHooks64.dll, gleiche Schnittstelle wie CapFrameX & Co.):
// stellt beim Einschalten unseres Im-Spiel-OSD die GLOBALE OSD-Sichtbarkeit von
// RTSS sicher. Ohne das haengt unser Block am Afterburner-OSD-Hotkey (der schaltet
// den ganzen Kanal) – mit diesem Aufruf ist Lumoras Hotkey autark: AN heisst AN.
let rtssHooks = null
function setupRtssHooks() {
  if (rtssHooks !== null) return rtssHooks
  rtssHooks = false
  try {
    const koffi = require('koffi')
    let lib = null
    for (const p of [
      'C:\\Program Files (x86)\\RivaTuner Statistics Server\\RTSSHooks64.dll',
      'C:\\Program Files\\RivaTuner Statistics Server\\RTSSHooks64.dll',
    ]) { try { lib = koffi.load(p); break } catch {} }
    if (lib) {
      // Nur die Laufzeit-Flags: Die Profil-API (Load/Save/SetProfileProperty)
      // schreibt nach Program Files und scheitert ohne Adminrechte still –
      // fuer Profil-Aenderungen ist sie aus einem Nutzerprozess wertlos.
      rtssHooks = {
        getFlags: lib.func('uint32 __cdecl GetFlags()'),
        setFlags: lib.func('void __cdecl SetFlags(uint32 dwAND, uint32 dwXOR)'),
      }
    }
  } catch (e) { osdDbg('[rtss-osd] Hooks nicht ladbar: ' + (e && e.message)) }
  return rtssHooks
}
// Freie Positionierung (Format v2.20+, am 04.07.2026 gegen RTSS 7.3.7 gemessen):
// Im vierten Slot-Textfeld szOSDEx2 fuehrt der RTSS-Renderer die Layout-Tags des
// Overlay-Editors aus – <P=x,y> setzt ABSOLUTE Pixel im Swapchain-Raum (Pixel ==
// Client-Pixel), <FNT=..> erzwingt einen festen Font und ignoriert Nutzer-Font/
// Zoom. Damit steht unser Block in jeder Ecke, waehrend Afterburners Block am
// klassischen Anker unberuehrt bleibt. (In szOSD/szOSDEx werden dieselben Tags
// geparst, aber verschluckt – deshalb schlugen fruehere <P>-Versuche fehl.)
// Die Swapchain-Aufloesung liefert der App-Eintrag: dwResolutionX/Y ab Format
// v2.20 bei Offset 9224 im pack(1)-Layout (per SDK-Header + Messung verifiziert).
function readRtssResolution() {
  if (!rtss) return null
  const h = rtss.open(0x0004, 0, 'RTSSSharedMemoryV2')
  if (!h) return null
  let base = 0
  try {
    base = rtss.map(h, 0x0004, 0, 0, 0)
    if (!base) return null
    const hdr = rtss.koffi.decode(base, rtss.HDR)
    if ((hdr.dwVersion >>> 0) < 0x20014 || hdr.dwAppEntrySize < 9232) return null
    const bestA = rtssBestApp(base, hdr, rtss.ticks())
    if (!bestA) return null
    const rb = Buffer.from(rtss.koffi.decode(base, bestA.off + 9224, rtss.koffi.array('uint8', 8)))
    const x = rb.readUInt32LE(0), y = rb.readUInt32LE(4)
    return x >= 320 && x <= 16384 && y >= 240 && y <= 16384 ? { x, y } : null
  } catch { return null }
  finally {
    if (base) { try { rtss.unmap(base) } catch {} }
    try { rtss.close(h) } catch {}
  }
}
// Aufloesung mit Gedaechtnis: Spiele in Menues/Dialogen praesentieren oft nur
// sporadisch Frames – dann liefert der Live-Read nichts und das Panel wuerde
// zwischen Ecke und Stapel-Fluss springen (gemessen in Groschengrab). Also die
// letzte bekannte Aufloesung behalten; vor dem ersten Spielkontakt naehert der
// Primaermonitor (physische Pixel) die Vollbild-Groesse an.
let rtssResCache = null
function rtssResolution() {
  const r = readRtssResolution()
  if (r) rtssResCache = r
  if (rtssResCache) return rtssResCache
  try {
    const d = screen.getPrimaryDisplay()
    return { x: Math.round(d.size.width * d.scaleFactor), y: Math.round(d.size.height * d.scaleFactor) }
  } catch { return null }
}
// Setzt das Lumora-Panel frei positioniert an die Wunsch-Ecke. Ohne Aufloesung
// (altes RTSS) faellt es in den klassischen Stapel-Fluss zurueck.
function rtssComposeEx2(mainLines, res) {
  const corner = appSettings.osdCorner || 'tl'
  // Klassischer Stapel-Fluss (nativer Font, dockt automatisch UNTER fremde
  // Anbieter wie ein aktives natives Afterburner-OSD -> keine Ueberlappung):
  // wenn freie Positionierung nicht moeglich (altes RTSS) oder Ecke "oben links".
  if (!res || corner === 'tl') return mainLines.join('\n')
  const fh = Math.max(14, Math.round(res.y / 68))   // Fonthoehe skaliert mit der Spielaufloesung
  const charW = fh * 0.55                           // Consolas-Vorschub (gemessen: 8.8 px bei -16)
  const lineH = Math.round(fh * 1.3)
  const margin = fh
  const vis = (s) => String(s).replace(/<[^>]*>/g, '').length
  let out = `<FNT=Consolas,${-fh},700,1>`
  if (mainLines.length) {
    const wMax = Math.max(...mainLines.map(vis)) * charW
    const x = (corner === 'tr' || corner === 'br') ? Math.max(0, Math.round(res.x - margin - wMax)) : margin
    const y0 = (corner === 'bl' || corner === 'br') ? Math.max(0, res.y - margin - mainLines.length * lineH) : margin
    mainLines.forEach((l, i) => { out += `<P=${x},${y0 + i * lineH}>` + l })
  }
  return out
}
// Stellt die LAUFZEIT-Sichtbarkeit des RTSS-OSD sicher (RTSSHOOKSFLAG_OSD_VISIBLE,
// Bit 0). GENAU dieses Flag schaltet Afterburners OSD-Hotkey um – nicht das
// Profil-Setting "EnableOSD" (per Diagnose verifiziert: Profil stand auf 1,
// waehrend das Flag 0 war und der Kanal dunkel blieb). Flags = (Flags & AND) ^ XOR.
let rtssFlagWasOff = false   // wir haben das Master-Bit von 0 auf 1 gehoben
function rtssEnsureOsdVisible() {
  try {
    const h = setupRtssHooks()
    if (!h) return
    const before = h.getFlags()
    if (before & 1) return                        // schon sichtbar
    rtssFlagWasOff = true                         // beim Ausschalten wiederherstellen
    h.setFlags((~1) >>> 0, 1)
    osdDbg('[rtss-osd] OSD_VISIBLE gesetzt (Flags 0x' + before.toString(16) + ' -> 0x' + h.getFlags().toString(16) + ')')
  } catch (e) { osdDbg('[rtss-osd] SetFlags fehlgeschlagen: ' + (e && e.message)) }
}
// Gegenstueck: Hatten WIR das Master-Bit angehoben, stellen wir beim Ausschalten
// den vorherigen Zustand wieder her – sonst bleibt z.B. ein zuvor per Hotkey
// ausgeschaltetes Afterburner-OSD nach unserem Aus dauerhaft sichtbar (gemessen).
function rtssReleaseVisible() {
  if (!rtssFlagWasOff) return
  rtssFlagWasOff = false
  try {
    const h = setupRtssHooks()
    if (h && (h.getFlags() & 1)) {
      h.setFlags((~1) >>> 0, 0)
      osdDbg('[rtss-osd] OSD_VISIBLE restauriert (aus)')
    }
  } catch (e) { osdDbg('[rtss-osd] Restore fehlgeschlagen: ' + (e && e.message)) }
}

// Hypertext-Fassungen unserer Themes (Kompakt/Minimal/Balken) fuer den RTSS-Modus.
// RTSS-Tags: <C=AARRGGBB> Farbe, <C> Reset, <S=..> Groesse (50/100/200 = native
// Sprites, alles andere wird matschig skaliert -> nur native Stufen verwenden).
// Der RTSS-Rasterfont ist MONOSPACE: saubere Spalten entstehen zuverlaessig durch
// Zeichen-Padding, nicht durch fragile Alignment-Tags.
const RTAG = { nv: '<C=FF74E857>', amd: '<C=FFFF8A1E>', val: '<C=FFFFE100>', dim: '<C=FF8A8A96>', fps: '<C=FFFFFFFF>', ft: '<C=FF6FE86F>', off: '<C>' }
function rtssAsciiBar(pct, width) {
  const p = Math.max(0, Math.min(100, Math.round(pct || 0)))
  const full = Math.round(p / 100 * width)
  return '='.repeat(full) + '-'.repeat(width - full)
}
function buildRtssOsd(theme, gpu, cpu, f, fields) {
  const gf = (fields && fields.gpu) || [], cf = (fields && fields.cpu) || [], ff = (fields && fields.fps) || []
  const num = (v, w) => String(v).padStart(w)
  // Ein Messwert als feste Spalte: Zahl gelb, Einheit gedimmt. null -> Leerraum
  // gleicher Breite, damit GPU-/CPU-Zeile spaltengenau untereinander stehen.
  const cell = (on, v, unit, w) => (on && v != null)
    ? `${RTAG.val}${num(v, w)}${RTAG.dim}${unit}${RTAG.off}`
    : ' '.repeat(w + unit.length)
  const mark = (tag, s) => `${tag}${s}${RTAG.off}`
  const gcol = gpu && gpu.brand === 'AMD' ? RTAG.amd : RTAG.nv
  const L = []

  if (theme === 'min') {
    const parts = []
    if (gpu && gf.includes('load') && gpu.load != null) {
      let s = `${gcol}GPU${RTAG.off} ${RTAG.val}${num(gpu.load, 3)}%${RTAG.off}`
      if (gf.includes('temp') && gpu.temp != null) s += ` ${RTAG.val}${num(Math.round(gpu.temp), 3)}\xB0${RTAG.off}`
      parts.push(s)
    }
    if (cpu && cf.includes('load') && cpu.load != null) {
      let s = `${RTAG.amd}CPU${RTAG.off} ${RTAG.val}${num(cpu.load, 3)}%${RTAG.off}`
      if (cf.includes('temp') && cpu.temp != null) s += ` ${RTAG.val}${num(Math.round(cpu.temp), 3)}\xB0${RTAG.off}`
      parts.push(s)
    }
    if (f && ff.includes('fps')) parts.push(`${RTAG.fps}${num(f.fps, 4)} FPS${RTAG.off}`)
    return [parts.join('  ')]
  }

  // 'compact'/'bar': Tabellenlayout – Markenspalte, dann spaltengenaue Werte.
  // Dezente Kennzeile: trennt unseren Block sichtbar von anderen OSD-Lieferanten
  // (z.B. Afterburner direkt darueber) und macht den A/B-Vergleich eindeutig.
  L.push('<S=50>' + mark(gcol, 'LUMORA') + '<S>')
  const gpuRow = gpu ? mark(gcol, 'GPU ') + mark(RTAG.dim, String(gpu.name || '').slice(0, 14).padEnd(14)) +
    cell(gf.includes('load'), gpu.load, '%', 4) +
    cell(gf.includes('temp'), gpu.temp != null ? Math.round(gpu.temp) : null, '\xB0C', 4) +
    cell(gf.includes('power'), gpu.power != null ? Math.round(gpu.power) : null, 'W', 5) +
    cell(gf.includes('clock'), gpu.clock, 'MHz', 5) +
    cell(gf.includes('vram'), gpu.vram, 'MB', 6) : null
  const cpuRow = cpu ? mark(RTAG.amd, 'CPU ') + mark(RTAG.dim, String(cpu.name || '').slice(0, 14).padEnd(14)) +
    cell(cf.includes('load'), cpu.load, '%', 4) +
    cell(cf.includes('temp'), cpu.temp != null ? Math.round(cpu.temp) : null, '\xB0C', 4) +
    cell(cf.includes('power'), cpu.power != null ? Math.round(cpu.power) : null, 'W', 5) +
    cell(cf.includes('clock'), cpu.clock, 'MHz', 5) +
    cell(cf.includes('ram'), cpu.ram, 'MB', 6) : null
  if (gpuRow) L.push(gpuRow)
  if (theme === 'bar' && gpu && gf.includes('load') && gpu.load != null) L.push('    ' + mark(gcol, rtssAsciiBar(gpu.load, 28)))
  if (cpuRow) L.push(cpuRow)
  if (theme === 'bar' && cpu && cf.includes('load') && cpu.load != null) L.push('    ' + mark(RTAG.amd, rtssAsciiBar(cpu.load, 28)))
  if (f && (ff.includes('fps') || ff.includes('frametime'))) {
    let row = ''
    if (ff.includes('fps')) row += `${RTAG.fps}${num(f.fps, 4)} FPS${RTAG.off}`
    if (ff.includes('frametime') && f.frametime != null) row += `   ${RTAG.ft}${num(f.frametime.toFixed(1), 5)} ms${RTAG.off}`
    L.push(row)
  }
  return L.filter(Boolean)
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
// (Der fruehere "Afterburner-Block" – Lumora rendert ABs OSD-Werte selbst nach –
// ist raus: Alt+B schaltet jetzt das NATIVE Afterburner-OSD, siehe toggleAbOsd.)

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

// --- VRAM-Belegung treiberfrei via PDH ("GPU Adapter Memory / Dedicated Usage") --
// Fuer GPUs, deren Sensor-API keinen VRAM liefert (AMD/ADL-PMLog). Zaehler-Instanzen
// heissen "luid_0x..._phys_0" (eine je Adapter); wir nehmen den groessten Wert –
// die dedizierte Karte belegt stets deutlich mehr als iGPU-Reservate. Kein Treiber,
// kein Admin: gleiche Technik wie der PDH-CPU-Takt oben.
let vramPdh = null
function setupVramCounter() {
  let koffi
  try { koffi = require('koffi') } catch { return }
  try {
    const pdh = koffi.load('pdh.dll')
    const openQ = pdh.func('long PdhOpenQueryA(str src, uintptr user, _Out_ void** q)')
    const addC = pdh.func('long PdhAddEnglishCounterA(void* q, str path, uintptr user, _Out_ void** c)')
    const collect = pdh.func('long PdhCollectQueryData(void* q)')
    const getArr = pdh.func('long PdhGetFormattedCounterArrayA(void* c, uint32 fmt, _Inout_ uint32* size, _Out_ uint32* count, void* buf)')
    const q = [null]
    if (openQ(null, 0, q) !== 0) throw new Error('PdhOpenQuery')
    const c = [null]
    if (addC(q[0], '\\GPU Adapter Memory(*)\\Dedicated Usage', 0, c) !== 0) throw new Error('PdhAddEnglishCounter')
    collect(q[0])
    vramPdh = { collect, getArr, q: q[0], c: c[0] }
    console.log('[osd] PDH-VRAM-Leser bereit (treiberfrei)')
  } catch (e) { console.log('[osd] PDH-VRAM nicht verfuegbar:', e && e.message); vramPdh = null }
}
function readPdhVramMb() {
  if (!vramPdh) return null
  try {
    vramPdh.collect(vramPdh.q)
    const size = [0], count = [0]
    const r0 = vramPdh.getArr(vramPdh.c, 0x00000400 /*PDH_FMT_LARGE*/, size, count, null)
    if ((r0 >>> 0) !== 0x800007D2 /*PDH_MORE_DATA*/ || !size[0]) return null
    const buf = Buffer.alloc(size[0])
    if (vramPdh.getArr(vramPdh.c, 0x00000400, size, count, buf) !== 0) return null
    // PDH_FMT_COUNTERVALUE_ITEM_A (x64): szName-Zeiger (8) + {CStatus u32, Pad u32,
    // largeValue i64} (16) = 24 Bytes je Instanz; Wert liegt bei Offset 16.
    let best = 0
    for (let i = 0; i < count[0]; i++) {
      const v = Number(buf.readBigInt64LE(i * 24 + 16))
      if (v > best) best = v
    }
    return best > 0 ? Math.round(best / 1048576) : null
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
function runFpsTask() {
  osdDbg('startBroker: schtasks /run ' + FPS_TASK)
  const p = spawn('schtasks', ['/run', '/tn', FPS_TASK], { windowsHide: true })
  let bad = false, errOut = ''
  p.stderr.on('data', (d) => { bad = true; errOut += d.toString() })
  p.on('error', (e) => { bad = true; errOut += (e && e.message) })
  p.on('exit', (code) => osdDbg('startBroker: schtasks exit code=' + code + ' bad=' + bad + ' ' + errOut.trim()))
}
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
  // Aufgabe noch nicht da (frisches System)? Die gemeinsame OSD-Einrichtung
  // uebernimmt (Erklaer-Dialog + EIN UAC fuer alles, was fehlt).
  if (!fpsTaskPresent()) { ensureOsdSetup(); return }
  runFpsTask()
}
function stopBroker() {
  shmWriteApp(0, 0)                           // Broker beendet sich (+ PresentMon)
}

// === CPU-Sensor-Broker (PawnIO) ===============================================
// CPU-Temp/-Verbrauch OHNE Afterburner: PawnIO (signierter Kernel-Treiber, der
// WinRing0-Nachfolger; pawnio.eu) liefert MSR/SMN-Lesezugriff ueber signierte
// Module. pawnio_open braucht Adminrechte -> das Auslesen laeuft, wie die FPS,
// in einem elevated Broker (geplante Aufgabe LumoraOSD-Sensors) und die Werte
// wandern per Shared Memory zur App. Gleiches Heartbeat-Muster wie der FPS-Broker.
const SENSE_TASK = 'LumoraOSD-Sensors'
const SENSE_SHM_NAME = 'Local\\LumoraOSDSense'
const SENSE_MAGIC = 0x4C4F5345   // 'LOSE'
let senseShm = null
let senseStructs = null
function senseInit() {
  if (senseShm) return true
  let koffi
  try { koffi = require('koffi') } catch { return false }
  try {
    const k32 = koffi.load('kernel32.dll')
    const CreateFileMappingA = k32.func('void* CreateFileMappingA(void* hFile, void* sec, uint32 protect, uint32 maxHi, uint32 maxLo, str name)')
    if (!senseStructs) {
      // Broker schreibt @0 (magic..pid), App @24 (appTick,wanted) – wie beim FPS-SHM.
      senseStructs = {
        FULL: koffi.struct('LumoraSenseFull', { magic: 'uint32', brokerTick: 'uint32', tempX10: 'int32', powerX10: 'int32', pid: 'uint32', _r: 'uint32', appTick: 'uint32', wanted: 'uint32' }),
        BROKER: koffi.struct('LumoraSenseBroker', { magic: 'uint32', brokerTick: 'uint32', tempX10: 'int32', powerX10: 'int32', pid: 'uint32', _r: 'uint32' }),
        APP: koffi.struct('LumoraSenseApp', { appTick: 'uint32', wanted: 'uint32' }),
      }
    }
    const h = CreateFileMappingA(koffi.as(-1, 'void*'), null, 0x04 /*PAGE_READWRITE*/, 0, 64, SENSE_SHM_NAME)
    if (!h) return false
    senseShm = {
      koffi, handle: h, FULL: senseStructs.FULL, BROKER: senseStructs.BROKER, APP: senseStructs.APP,
      map: k32.func('void* MapViewOfFile(void* h, uint32 access, uint32 offHi, uint32 offLo, size_t bytes)'),
      unmap: k32.func('int UnmapViewOfFile(void* p)'),
      ticks: k32.func('uint32 GetTickCount()'),
    }
    return true
  } catch (e) { osdDbg('senseInit throw: ' + (e && e.message)); return false }
}
function senseReadFull() {
  if (!senseShm) return null
  const base = senseShm.map(senseShm.handle, 0xF001F, 0, 0, 0)
  if (!base) return null
  try { return senseShm.koffi.decode(base, senseShm.FULL) }
  catch { return null }
  finally { try { senseShm.unmap(base) } catch {} }
}
function senseWriteBroker(o) {
  if (!senseShm) return
  const base = senseShm.map(senseShm.handle, 0xF001F, 0, 0, 0)
  if (!base) return
  try { senseShm.koffi.encode(base, 0, senseShm.BROKER, o) } catch {}
  finally { try { senseShm.unmap(base) } catch {} }
}
function senseWriteApp(appTick, wanted) {
  if (!senseShm) return
  const base = senseShm.map(senseShm.handle, 0xF001F, 0, 0, 0)
  if (!base) return
  try { senseShm.koffi.encode(base, 24, senseShm.APP, { appTick, wanted }) } catch {}
  finally { try { senseShm.unmap(base) } catch {} }
}
function getPawnioDir() { return path.join(process.env.ProgramFiles || 'C:\\Program Files', 'PawnIO') }
function pawnioInstalled() { try { return fs.existsSync(path.join(getPawnioDir(), 'PawnIOLib.dll')) } catch { return false } }
function getPawnioModulePath(name) {
  return app.isPackaged ? path.join(process.resourcesPath, name) : path.join(__dirname, name)
}
// Passendes (gebuendeltes, LGPL-lizenziertes) PawnIO-Modul fuer diese CPU.
function cpuPawnioModule() {
  const m = (os.cpus()[0] && os.cpus()[0].model) || ''
  if (/amd|ryzen/i.test(m)) return 'AMDFamily17.bin'   // Zen 1-5 (Family 17h-1Ah, prueft selbst)
  if (/intel/i.test(m)) return 'IntelMSR.bin'
  return null
}

// Broker-Prozess (elevated, Lumora.exe --sensor-broker): PawnIO oeffnen, Modul
// laden, 1x/s CPU-Temp/-Power in den Shared Memory schreiben. Beendet sich selbst,
// sobald der App-Heartbeat ausbleibt (App zu / OSD aus).
function runSensorBroker() {
  if (!senseInit()) { app.quit(); return }
  const s0 = senseReadFull()
  if (s0 && s0.magic === SENSE_MAGIC && s0.brokerTick && ((senseShm.ticks() - s0.brokerTick) >>> 0) < 3500) {
    osdDbg('[sense] anderer Sensor-Broker aktiv – beende mich'); app.quit(); return
  }
  let pio = null
  try {
    const koffi = require('koffi')
    const lib = koffi.load(path.join(getPawnioDir(), 'PawnIOLib.dll'))
    const pioOpen = lib.func('long __stdcall pawnio_open(_Out_ void** handle)')
    const pioLoad = lib.func('long __stdcall pawnio_load(void* h, void* blob, size_t size)')
    const pioExec = lib.func('long __stdcall pawnio_execute(void* h, str name, void* inBuf, size_t inCount, void* outBuf, size_t outCount, _Out_ size_t* retCount)')
    const pioClose = lib.func('long __stdcall pawnio_close(void* h)')
    const modName = cpuPawnioModule()
    if (!modName) throw new Error('CPU nicht unterstuetzt')
    const h = [null]
    const rO = pioOpen(h)
    if (rO !== 0) throw new Error('pawnio_open 0x' + (rO >>> 0).toString(16) + ' (Adminrechte?)')
    const blob = fs.readFileSync(getPawnioModulePath(modName))
    const rL = pioLoad(h[0], blob, blob.length)
    if (rL !== 0) throw new Error('pawnio_load ' + modName + ' 0x' + (rL >>> 0).toString(16))
    const inB = Buffer.alloc(8), outB = Buffer.alloc(8), retC = [0]
    const rd = (name, v) => {
      inB.writeBigUInt64LE(BigInt.asUintN(64, BigInt(v)))
      try { return pioExec(h[0], name, inB, 1, outB, 1, retC) === 0 ? outB.readBigUInt64LE(0) : null } catch { return null }
    }
    pio = { close: () => { try { pioClose(h[0]) } catch {} }, rd, amd: modName === 'AMDFamily17.bin' }
    osdDbg('[sense] PawnIO bereit, Modul ' + modName)
  } catch (e) { osdDbg('[sense] Init-Fehler: ' + (e && e.message)); app.quit(); return }

  // Intel: TjMax einmalig (MSR_TEMPERATURE_TARGET 0x1A2, Bits 23:16); AMD braucht keins.
  let tjMax = 100
  if (!pio.amd) {
    const t = pio.rd('ioctl_read_msr', 0x1A2)
    if (t != null) { const v = Number((t >> 16n) & 0xffn); if (v > 40 && v < 130) tjMax = v }
  }
  // RAPL-Energie-Einheit (AMD 0xC0010299 / Intel 0x606, ESU in Bits 12:8).
  const energyMsr = pio.amd ? 0xC001029B : 0x611
  let esu = 16
  { const u = pio.rd('ioctl_read_msr', pio.amd ? 0xC0010299 : 0x606); if (u != null) esu = Number((u >> 8n) & 0x1fn) }
  const jPerTick = 1 / Math.pow(2, esu)

  let lastE = null, lastT = 0, watts = -1
  const startTick = senseShm.ticks()
  let lastFreshApp = senseShm.ticks()
  const timer = setInterval(() => {
    const now = senseShm.ticks()
    const s = senseReadFull()
    if (s && s.appTick && ((now - s.appTick) >>> 0) < 3000) lastFreshApp = now
    if (((now - startTick) >>> 0) > 8000 && ((now - lastFreshApp) >>> 0) > 5000) {
      osdDbg('[sense] Exit: kein App-Heartbeat'); cleanup(); return
    }
    // Temperatur: AMD Tctl via SMN THM_TCON_CUR_TMP; Intel via IA32_THERM_STATUS (TjMax-DTS).
    let temp = null
    if (pio.amd) {
      const r = pio.rd('ioctl_read_smn', 0x00059800)
      if (r != null) {
        const raw = Number(r & 0xffffffffn)
        let t = (raw >>> 21) * 0.125
        if (raw & 0x80000) t -= 49
        if (t > -20 && t < 150) temp = t
      }
    } else {
      const r = pio.rd('ioctl_read_msr', 0x19C)
      if (r != null) {
        const raw = Number(r & 0xffffffffn)
        if (raw & 0x80000000) temp = tjMax - ((raw >>> 16) & 0x7f)   // Reading-Valid-Bit
      }
    }
    // Package-Power: kumulativer Energiezaehler -> Watt = Delta-Energie / Delta-Zeit.
    const e = pio.rd('ioctl_read_msr', energyMsr)
    const tNow = Date.now()
    if (e != null) {
      const cur = Number(BigInt.asUintN(32, e))
      if (lastE != null && tNow > lastT) {
        let dE = cur - lastE; if (dE < 0) dE += 0x100000000   // 32-bit-Wrap
        const w = dE * jPerTick / ((tNow - lastT) / 1000)
        if (w >= 0 && w < 1000) watts = w
      }
      lastE = cur; lastT = tNow
    }
    senseWriteBroker({
      magic: SENSE_MAGIC, brokerTick: now,
      tempX10: temp != null ? Math.round(temp * 10) : -1,
      powerX10: watts >= 0 ? Math.round(watts * 10) : -1,
      pid: process.pid, _r: 0,
    })
  }, 1000)
  let cleaned = false
  function cleanup() { if (cleaned) return; cleaned = true; clearInterval(timer); if (pio) pio.close(); app.quit() }
  app.on('window-all-closed', (e) => e.preventDefault())   // Broker hat kein Fenster
}

// --- App-Seite des Sensor-Brokers ---------------------------------------------
let senseSpawnAt = 0
function senseTaskPresent() {
  try { return require('child_process').spawnSync('schtasks', ['/query', '/tn', SENSE_TASK], { windowsHide: true }).status === 0 }
  catch { return false }
}
function runSenseTask() { try { spawn('schtasks', ['/run', '/tn', SENSE_TASK], { windowsHide: true }) } catch {} }
function senseBrokerAlive() {
  if (!senseShm) return false
  const s = senseReadFull()
  return !!(s && s.magic === SENSE_MAGIC && s.brokerTick && ((senseShm.ticks() - s.brokerTick) >>> 0) < 3500)
}
function stopSensorBroker() { if (senseShm) senseWriteApp(0, 0) }
// Liest CPU-Temp/-Power des Brokers; startet ihn bei Bedarf (lazy, gedrosselt).
// Zeigt KEINEN Setup-Dialog – der laeuft nur beim OSD-Einschalten (ensureOsdSetup),
// damit mitten im Spiel niemals ungefragt ein Dialog aufpoppt.
function readSenseCpu() {
  if (appSettings.osdSetupDeclined || !cpuPawnioModule()) return null
  if (!senseInit()) return null
  senseWriteApp(senseShm.ticks(), 1)           // Heartbeat: App will Sensorwerte
  if (!senseBrokerAlive()) {
    const now = Date.now()
    if (now - senseSpawnAt > 5000 && pawnioInstalled() && senseTaskPresent()) {
      senseSpawnAt = now
      runSenseTask()
    }
    return null
  }
  const s = senseReadFull()
  if (!s) return null
  return { temp: s.tempX10 >= 0 ? s.tempX10 / 10 : null, power: s.powerX10 >= 0 ? s.powerX10 / 10 : null }
}

// --- Gemeinsame OSD-Einrichtung (EIN Erklaer-Dialog, EIN UAC) -------------------
// Richtet beim OSD-Einschalten alles ein, was fuer die Vollausstattung fehlt:
// FPS-Task (wenn PresentMon gebraucht wird), PawnIO-Treiber (Download von der
// offiziellen Quelle + Signaturpruefung + Silent-Install) und Sensor-Task.
// Alles Elevated laeuft in EINEM PowerShell-Aufruf -> ein einziger UAC-Dialog.
let osdSetupRunning = false
function psRun(cmd) {
  return new Promise((resolve) => {
    try {
      require('child_process').execFile('powershell', ['-NoProfile', '-Command', cmd], { windowsHide: true, timeout: 180000 }, (err, stdout) => resolve(String(stdout || '') + (err ? ' ERR:' + err.message : '')))
    } catch (e) { resolve('ERR:' + (e && e.message)) }
  })
}
function fpsNeedsSetup() {
  const src = appSettings.osdFpsSource || 'auto'
  const useRtss = src === 'rtss' ? true : src === 'presentmon' ? false : rtssAvailable()
  return !useRtss && !fpsTaskPresent()
}
function sensorsNeedSetup() {
  if (!cpuPawnioModule()) return false
  const m = readMahm()
  if (m && m.cpuTemp != null) return false     // Afterburner liefert bereits
  return !pawnioInstalled() || !senseTaskPresent()
}
async function ensureOsdSetup() {
  if (osdSetupRunning || appSettings.osdSetupDeclined) return
  const needFps = fpsNeedsSetup()
  const sensorGap = sensorsNeedSetup()
  const needPawnio = sensorGap && !pawnioInstalled()
  const needSense = sensorGap && !senseTaskPresent()
  if (!needFps && !needPawnio && !needSense) return
  osdSetupRunning = true
  try {
    const parts = []
    if (needFps) parts.push('• FPS-Messung: kleiner Hintergrunddienst (PresentMon, liegt Lumora bei)')
    if (needPawnio) parts.push('• CPU-Temperatur & -Verbrauch: signierter Open-Source-Treiber PawnIO (wird von der offiziellen Quelle geladen)')
    else if (needSense) parts.push('• CPU-Temperatur & -Verbrauch: Hintergrunddienst fuer den PawnIO-Treiber')
    const parent = (mainWindow && !mainWindow.isDestroyed()) ? mainWindow : null
    const r = await dialog.showMessageBox(parent, {
      type: 'info', noLink: true,
      title: 'OSD einrichten',
      message: 'OSD vollstaendig einrichten',
      detail: 'Damit das OSD alle Werte anzeigen kann, richtet Lumora einmalig ein:\n\n' + parts.join('\n') + '\n\nGleich fragt Windows EINMAL nach deiner Bestaetigung (Administratorrechte). Danach laeuft alles automatisch – ohne weitere Nachfragen.',
      buttons: ['Einrichten', 'Abbrechen'], defaultId: 0, cancelId: 1,
    })
    if (r.response !== 0) {                       // abgelehnt -> merken, nicht staendig neu fragen
      appSettings.osdSetupDeclined = true; saveAppSettings(); stopFps()
      if (parent) parent.webContents.send('osd-fps-off')
      osdDbg('ensureOsdSetup: abgelehnt')
      return
    }
    // Status-Toast im Hauptfenster: macht jede Phase sichtbar (Download dauert
    // einen Moment, bis der UAC-Dialog kommt – ohne Anzeige wirkt das wie haengen).
    const status = (msg, done) => { try { if (parent && !parent.isDestroyed()) parent.webContents.send('osd-setup-status', msg, !!done) } catch {} }
    // 1) PawnIO-Installer non-elevated laden + Authenticode-Signatur pruefen.
    let setupExe = null
    if (needPawnio) {
      status('OSD-Einrichtung: Lade PawnIO-Treiber herunter und pruefe die Signatur …')
      setupExe = path.join(app.getPath('temp'), 'PawnIO_setup.exe')
      const q = (s) => s.replace(/'/g, "''")
      const out = await psRun(`Invoke-WebRequest -Uri 'https://github.com/namazso/PawnIO.Setup/releases/latest/download/PawnIO_setup.exe' -OutFile '${q(setupExe)}' -UseBasicParsing; (Get-AuthenticodeSignature '${q(setupExe)}').Status`)
      if (!/\bValid\b/.test(out)) {
        osdDbg('ensureOsdSetup: PawnIO-Download/Signatur FEHLER: ' + out.trim())
        status(null)
        if (parent) dialog.showMessageBox(parent, { type: 'error', noLink: true, message: 'PawnIO-Download fehlgeschlagen', detail: 'Der Treiber konnte nicht geladen oder verifiziert werden. Bitte Internetverbindung pruefen – die Einrichtung wird beim naechsten Einschalten des OSD erneut angeboten.' })
        return
      }
      osdDbg('ensureOsdSetup: PawnIO geladen, Signatur Valid')
    }
    // 2) EIN elevated Aufruf: [PawnIO-Silent-Install] + [FPS-Task] + [Sensor-Task].
    const exe = process.execPath.replace(/'/g, "''")
    const taskPs = (task, arg, v) => [
      `$a${v}=New-ScheduledTaskAction -Execute '${exe}' -Argument '${arg}'`,
      `$p${v}=New-ScheduledTaskPrincipal -UserId $env:USERNAME -RunLevel Highest -LogonType Interactive`,
      `$s${v}=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew`,
      `Register-ScheduledTask -TaskName '${task}' -Action $a${v} -Principal $p${v} -Settings $s${v} -Force`,
    ]
    const ps = []
    if (needPawnio) ps.push(`Start-Process -FilePath '${setupExe.replace(/'/g, "''")}' -ArgumentList '-install','-silent' -Wait`)
    if (needFps) ps.push(...taskPs(FPS_TASK, '--fps-broker', ''))
    if (needSense || needPawnio) ps.push(...taskPs(SENSE_TASK, '--sensor-broker', '2'))
    const b64 = Buffer.from(ps.join('; '), 'utf16le').toString('base64')
    const outer = `Start-Process powershell -Verb RunAs -WindowStyle Hidden -ArgumentList '-NoProfile -EncodedCommand ${b64}'`
    status('OSD-Einrichtung: Warte auf deine Bestaetigung (Windows-Sicherheitsabfrage) …')
    try { spawn('powershell', ['-NoProfile', '-Command', outer], { windowsHide: true }) } catch (e) { osdDbg('ensureOsdSetup spawn: ' + (e && e.message)); status(null); return }
    // 3) Warten, bis alles da ist (Install braucht Momente), dann Broker anwerfen.
    let tries = 0, working = false
    const ok = await new Promise((resolve) => {
      const wait = setInterval(() => {
        const fpsOk = !needFps || fpsTaskPresent()
        const pioOk = !needPawnio || pawnioInstalled()
        const senseOk = (!needSense && !needPawnio) || senseTaskPresent()
        // Sobald das ERSTE tatsaechlich benoetigte Teil auftaucht, wurde der UAC
        // bestaetigt -> Statuswechsel von "warte auf Bestaetigung" zu "installiere".
        const progressed = (needPawnio && pawnioInstalled()) || (needFps && fpsTaskPresent()) || ((needSense || needPawnio) && senseTaskPresent())
        if (!working && progressed) { working = true; status('OSD-Einrichtung: Installiere und richte Hintergrunddienste ein …') }
        if (fpsOk && pioOk && senseOk) { clearInterval(wait); osdDbg('ensureOsdSetup: Einrichtung komplett'); resolve(true) }
        else if (++tries >= 90) { clearInterval(wait); osdDbg('ensureOsdSetup: Timeout (UAC abgelehnt?)'); resolve(false) }
      }, 1000)
    })
    if (ok) status('OSD-Einrichtung abgeschlossen – alle Werte kommen gleich rein. ✓', true)
    else status('OSD-Einrichtung nicht abgeschlossen – erneut ueber Einstellungen → Overlay.', true)
    syncFps()          // FPS-Broker starten, falls gebraucht (Sensor-Broker startet lazy via readSenseCpu)
  } finally { osdSetupRunning = false }
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
  // Im RTSS-Renderer-Modus kommen die FPS zwingend von RTSS (laeuft dort per
  // Definition) – kein PresentMon-Broker noetig, egal was die Quelle sagt.
  const src = rtssOsdActive ? 'rtss' : (appSettings.osdFpsSource || 'auto')
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
  const vis = rtssOsdActive || (overlayWindow && !overlayWindow.isDestroyed() && overlayWindow.isVisible())
  const want = !!(appSettings.osdEnabled && !appSettings.osdSetupDeclined && vis)
  if (want && !fpsTimer) startFps()
  else if (!want && fpsTimer) stopFps()
}

// Sensor-Pump: solange das OSD aktiv ist, Werte liefern – im Fenster-Modus als
// IPC ans Overlay, im RTSS-Modus als Hypertext in den RTSS-OSD-Slot.
let osdDataInterval = null
function startOsdData() {
  if (osdDataInterval) return
  prevCpu = null
  let lastRtssWrite = 0
  const tick = () => {
    if (rtssOsdActive) {
      // Im-Spiel-Rendering: FPS kommen direkt von RTSS (das laeuft hier per
      // Definition). Auf ~2 Hz gedrosselt – ruhige Zahlen statt Flackern.
      const now = Date.now()
      if (now - lastRtssWrite < 450) return
      lastRtssWrite = now
      // Das globale OSD_VISIBLE-Bit ist RTSS' EINZIGER, GEMEINSAMER Sichtbarkeits-
      // schalter: Afterburners OSD-Hotkey schaltet genau dieses Bit, sein Slot-
      // Text bleibt dabei stehen (gemessen 04.07.2026). Wuerden wir es hier
      // zurueckzwingen, waere der AB-Hotkey wirkungslos. Also respektieren:
      // Bit extern aus -> unser OSD folgt sauber (Slot raeumen, Setting aus).
      // Getrennt schalten: AB ueber seine eigene "Show OSD"-Option (leert den
      // Slot), Lumora ueber Alt+O – dann funkt keiner dem anderen dazwischen.
      try {
        const h = setupRtssHooks()
        if (h && (h.getFlags() & 1) === 0) {
          osdDbg('[rtss-osd] Master-Bit extern ausgeschaltet -> Lumora-OSD folgt')
          appSettings.osdEnabled = false
          saveAppSettings()
          hideOverlay()
          return
        }
      } catch {}
      const mainLines = appSettings.osdEnabled
        ? buildRtssOsd(appSettings.osdTheme, readGpu(), readCpu(), readRtssFps(), appSettings.osdFields) : []
      rtssOsdWrite(mainLines)
      return
    }
    if (!overlayWindow || overlayWindow.isDestroyed() || !overlayWindow.isVisible()) return
    const payload = { gpu: readGpu(), cpu: readCpu() }
    if (appSettings.osdSetupDeclined) payload.fps = '—'   // Einrichtung abgelehnt; sonst sendet die FPS-Schleife
    overlayWindow.webContents.send('osd-data', payload)
  }
  tick()
  osdDataInterval = setInterval(tick, 200)   // 5 Hz – Temps/Power/Takt fluessig
}
function stopOsdData() {
  if (osdDataInterval) { clearInterval(osdDataInterval); osdDataInterval = null }
  stopSensorBroker()   // wanted=0 -> Sensor-Broker beendet sich zeitnah
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
  // Im-Spiel-Rendering (RTSS): kein eigenes Fenster – wir beliefern den RTSS-Slot.
  // Faellt automatisch auf das Overlay-Fenster zurueck, wenn RTSS nicht laeuft.
  if (appSettings.osdRenderer === 'rtss' && rtssAvailable()) {
    rtssOsdActive = true
    if (overlayWindow && !overlayWindow.isDestroyed()) overlayWindow.hide()
    // Beim bewussten Einschalten heben wir das globale Sichtbarkeits-Bit an
    // (sonst koennten wir gar nicht rendern) und merken uns den Vorzustand.
    // Systembedingt wird dabei auch ein per RTSS-Hotkey ausgeblendetes
    // Afterburner-OSD wieder sichtbar, denn dessen Slot-Text bleibt stehen –
    // wer AB getrennt aus haben will, schaltet es in Afterburner selbst aus.
    rtssEnsureOsdVisible()
    startOsdData()
    syncFps()
    ensureOsdSetup()
    osdDbg('showOverlay: RTSS-Modus aktiv (im Spiel gerendert)')
    return
  }
  if (rtssOsdActive) { rtssOsdActive = false; rtssOsdClear() }
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
  ensureOsdSetup()   // fehlt noch etwas fuer die Vollausstattung? EIN Dialog, EIN UAC.
  try { osdDbg('showOverlay: isVisible=' + w.isVisible() + ' onTop=' + w.isAlwaysOnTop() + ' bounds=' + JSON.stringify(w.getBounds())) } catch {}
}

function hideOverlay() {
  if (rtssOsdActive) { rtssOsdActive = false; rtssOsdClear(); rtssReleaseVisible() }
  if (overlayWindow && !overlayWindow.isDestroyed()) overlayWindow.hide()
  stopOsdData()
  syncFps()
}

// Zentrale Sichtbarkeit des Lumora-Panels (osdEnabled, Alt+O). Das native
// Afterburner-OSD schaltet Alt+B (toggleAbOsd) direkt ueber RTSS – unabhaengig.
function syncOsdVisibility() {
  if (appSettings.osdEnabled) showOverlay(); else hideOverlay()
}
function toggleOverlay() {
  appSettings.osdEnabled = !appSettings.osdEnabled
  osdDbg('toggleOverlay -> osdEnabled=' + appSettings.osdEnabled)
  saveAppSettings()
  syncOsdVisibility()
}
// Alt+B: das NATIVE Afterburner-OSD ein-/ausblenden. Schaltet das globale
// RTSS-Sichtbarkeits-Bit – exakt das Bit, das auch Afterburners eigener
// OSD-Hotkey nutzt (04.07. vermessen). Im Fenster-Modus ist Lumoras OSD davon
// unabhaengig (eigenes Fenster) -> AB laesst sich getrennt schalten. Im
// RTSS-Renderer haengt auch unser Block an diesem Bit (RTSS hat nur EINEN
// Sichtbarkeits-Kanal) -> dort bewusst kein Toggle, sonst killt Alt+B das
// Lumora-OSD gleich mit (die Folgen-Logik in startOsdData schaltet es dann ab).
function toggleAbOsd() {
  if (rtssOsdActive) { osdDbg('toggleAbOsd: RTSS-Renderer aktiv – Lumora+Afterburner teilen sich den Kanal, kein getrenntes Schalten'); return }
  try {
    const h = setupRtssHooks()
    if (!h) { osdDbg('toggleAbOsd: RTSSHooks nicht verfuegbar (RTSS/Afterburner installiert?)'); return }
    const before = h.getFlags()
    const on = (before & 1) === 1
    h.setFlags((~1) >>> 0, on ? 0 : 1)
    osdDbg('toggleAbOsd: Afterburner-OSD -> ' + (on ? 'AUS' : 'AN') + ' (Flags 0x' + before.toString(16) + ' -> 0x' + h.getFlags().toString(16) + ')')
  } catch (e) { osdDbg('toggleAbOsd: ' + (e && e.message)) }
}

// --- Live-Edit-Modus: OSD direkt ueber dem Spiel anfassbar machen -------------
// Der Hotkey macht das Overlay kurz interaktiv (faengt Maus): ziehen = Position
// (rastet in die naechste Ecke), Mausrad = Groesse. "Fertig" schaltet zurueck.
let osdEditActive = false
function setOsdEditMode(on) {
  // Im RTSS-Modus gibt es kein anfassbares Overlay-Fenster – Layout/Position
  // steuert dort RTSS. Live-Edit gilt nur fuer den Fenster-Modus.
  if (rtssOsdActive) { osdDbg('Live-Edit im RTSS-Modus nicht verfuegbar'); return }
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
let timeEndPeriodFn = null       // winmm.timeEndPeriod – bei Quit die 1-ms-Timer-Aufloesung freigeben
let lastPollAt = 0               // Diagnose: Abstand zwischen Poll-Ticks (Timer-Traegheit erkennen)
let fgWin = null                 // user32 GetForegroundWindow/SetForegroundWindow/IsWindow (HWNDs als Zahlen)
let prevGameHwnd = 0             // Fenster, dem wir beim Hochholen den Fokus genommen haben (i.d.R. das Spiel)
let gpHotkeyDown = false
let gpOsdHotkeyDown = false
// Release-Debounce statt Cooldown: XInput liest eine gehaltene Kombi nicht
// durchgehend – einzelne Button-Bits fallen fuer einen Poll-Zyklus weg. Ohne
// Entprellung erzeugt das am laufenden Band neue "steigende Flanken" -> der
// Hotkey feuert mehrfach pro Druck. Loesung: die Kombi gilt erst als LOSGELASSEN,
// wenn sie GP_RELEASE_MS durchgehend nicht mehr gelesen wurde. Ein Aussetzer wird
// so ueberbrueckt -> ein Druck loest genau EINMAL aus, egal wie stark es flackert.
const GP_RELEASE_MS = 150
let gpHotkeyLastSeen = 0
let gpOsdHotkeyLastSeen = 0
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
  // Windows-Timer-Aufloesung auf 1 ms halten, solange der Poll laeuft. Sonst gibt
  // Chromium sie frei, sobald Lumora minimiert ist -> der 40-ms-Timer feuert im
  // Hintergrund nur noch alle paar hundert ms und der Gamepad-Hotkey reagiert
  // spuerbar verzoegert. timeBeginPeriod(1) haelt setInterval praezise (genau das
  // tun Afterburner/RTSS & andere Hintergrund-Overlays ebenfalls).
  try {
    const winmm = koffi.load('winmm.dll')
    const timeBeginPeriod = winmm.func('uint32 __stdcall timeBeginPeriod(uint32 uPeriod)')
    timeEndPeriodFn = winmm.func('uint32 __stdcall timeEndPeriod(uint32 uPeriod)')
    timeBeginPeriod(1)
    console.log('[hotkey] Timer-Aufloesung auf 1 ms gesetzt (praeziser Hintergrund-Poll)')
  } catch (e) { console.log('[hotkey] timeBeginPeriod nicht verfuegbar:', e && e.message) }
  // Foreground-API fuer die saubere Fokus-Rueckgabe ans Spiel (siehe
  // restoreGameFocus). HWNDs bewusst als Zahlen (uintptr) statt Pointer –
  // einfacher zu merken und zu vergleichen.
  try {
    const u32 = koffi.load('user32.dll')
    fgWin = {
      get: u32.func('uintptr_t __stdcall GetForegroundWindow()'),
      set: u32.func('int __stdcall SetForegroundWindow(uintptr_t hWnd)'),
      isWin: u32.func('int __stdcall IsWindow(uintptr_t hWnd)'),
      isIconic: u32.func('int __stdcall IsIconic(uintptr_t hWnd)'),
      showWin: u32.func('int __stdcall ShowWindow(uintptr_t hWnd, int nCmdShow)'),
      // Synthetischer Tastendruck: ein kurzes Alt "entsperrt" SetForegroundWindow
      // (dokumentiertes Windows-Verhalten) – noetig gegen die Foreground-Sperre
      // von Vollbild-Spielen (z.B. Forza-Intro).
      kbd: u32.func('void __stdcall keybd_event(uint8 bVk, uint8 bScan, uint32 dwFlags, uintptr_t dwExtraInfo)'),
    }
  } catch (e) { fgWin = null; console.log('[hotkey] Foreground-API nicht ladbar:', e && e.message) }
  // Poll starten, sobald wenigstens EINE native Eingabe verfuegbar ist. 25 Hz reichen
  // fuer reaktive Hotkeys und halten die XInput-Last niedrig.
  if ((xinputGetState || getAsyncKey) && !xinputPoll) xinputPoll = setInterval(pollNativeHotkeys, 40)
}

// Ist die gegebene Knopf-Kombi auf irgendeinem der 4 XInput-Slots vollstaendig gedrueckt?
function gamepadComboDown(combo) {
  if (!xinputGetState || !combo || !combo.length || combo.some(i => !XI_MASK[i])) return false
  for (let slot = 0; slot < 4; slot++) {
    const st = {}; let ret
    try { ret = xinputGetState(slot, st) } catch { continue }
    if (ret !== 0) continue
    if (combo.every(i => XI_MASK[i](st.Gamepad))) return true
  }
  return false
}

function pollNativeHotkeys() {
  // 1) Gamepad-Kombis (XInput) – Oberflaeche + OSD, je auf steigende Flanke, mit
  //    Release-Debounce (GP_RELEASE_MS) gegen Read-Aussetzer einer gehaltenen Kombi.
  const now = Date.now()
  // Diagnose (nur mit Debug-Flag): feuert der 40-ms-Timer im Hintergrund traege?
  if (lastPollAt && (now - lastPollAt) > 120) osdDbg('[poll] Timer-Traegheit: ' + (now - lastPollAt) + ' ms seit letztem Tick (Soll ~40)')
  lastPollAt = now
  const rawMain = gamepadComboDown(appSettings.gamepadHotkey)
  if (rawMain) gpHotkeyLastSeen = now
  const heldMain = rawMain || (gpHotkeyLastSeen && (now - gpHotkeyLastSeen) < GP_RELEASE_MS)   // Read-Aussetzer ueberbruecken
  if (heldMain && !gpHotkeyDown) { osdDbg('[hk] Haupt-Hotkey AUSGELOEST'); toggleMainWindow() }
  gpHotkeyDown = heldMain
  const rawOsd = gamepadComboDown(appSettings.gamepadOsdHotkey)
  if (rawOsd) gpOsdHotkeyLastSeen = now
  const heldOsd = rawOsd || (gpOsdHotkeyLastSeen && (now - gpOsdHotkeyLastSeen) < GP_RELEASE_MS)
  if (heldOsd && !gpOsdHotkeyDown) { osdDbg('[hk] OSD-Hotkey AUSGELOEST'); toggleOverlay() }
  gpOsdHotkeyDown = heldOsd

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

  // Rechtsklick in Eingabefeldern: Electron bringt von Haus aus KEIN Kontextmenue
  // mit - ohne diesen Handler geht "Einfuegen" per rechter Maustaste nirgendwo
  // (aufgefallen beim Gruppen-Beitreten-Feld, gilt aber fuer alle Textfelder).
  // WICHTIG: readonly-Felder (Gruppen-Code, Link-Anzeigen) melden isEditable=false,
  // sollen aber trotzdem ein Kopieren-Menue bekommen - daher zusaetzlich auf
  // inputFieldType (Rechtsklick IN einem Feld) bzw. markierten Text pruefen.
  mainWindow.webContents.on('context-menu', (e, params) => {
    const inField = params.isEditable || (params.inputFieldType && params.inputFieldType !== 'none')
    if (!inField && !params.selectionText) return
    Menu.buildFromTemplate([
      { label: 'Ausschneiden', role: 'cut', enabled: !!params.editFlags.canCut },
      { label: 'Kopieren', role: 'copy', enabled: !!(params.editFlags.canCopy || params.selectionText) },
      { label: 'Einfügen', role: 'paste', enabled: !!params.editFlags.canPaste },
      { type: 'separator' },
      { label: 'Alles auswählen', role: 'selectAll', enabled: !!params.editFlags.canSelectAll },
    ]).popup({ window: mainWindow })
  })

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
    activeLaunchExes.add(exeName.toLowerCase())   // Fremdstart-Watcher: dieses Spiel trackt der Eigenstart-Monitor
    // Lief das Spiel bereits fremd (Watcher-Session)? Dann dort sauber abschliessen –
    // ab jetzt zaehlt der Eigenstart-Monitor (keine Doppelzaehlung).
    const extSession = externalSessions.get(exeName.toLowerCase())
    if (extSession) { externalSessions.delete(exeName.toLowerCase()); sendExternalRunning(); creditPlaySession(extSession.gamePath, Date.now() - extSession.startTs) }
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
      activeLaunchExes.delete(exeName.toLowerCase())   // Fremdstart-Watcher darf wieder uebernehmen
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

// === Fremdstart-Watcher ========================================================
// Erkennt Bibliotheksspiele, die NICHT ueber Lumora gestartet wurden (z.B. per
// Xbox-Gamebar-Einladung, Steam-Client, Desktop-Verknuepfung), und liefert ihnen
// dieselben Dienste wie beim Eigenstart: HDR-Automatik + Spielzeit/"zuletzt
// gespielt". Ein tasklist-Scan alle 15 s (ein leichter Aufruf fuer ALLE Spiele);
// Session-Ende erst nach 2 leeren Scans (gegen kurze Prozess-Luecken).
const activeLaunchExes = new Set()    // vom Eigenstart-Monitor belegt -> Watcher laesst die Finger davon
const externalSessions = new Map()    // exe(lower) -> { gamePath, name, startTs, absent, hdrOn }
let extWatchTimer = null

function readLibraryGames() {
  try { return JSON.parse(fs.readFileSync(gamesFile, 'utf8')) || [] } catch { return [] }
}
// .lnk-Eintraege: Ziel-Exe einmalig (async) aufloesen und cachen, damit auch
// Verknuepfungs-Spiele vom Watcher erfasst werden. Bis die Aufloesung fertig ist,
// liefert der Cache null -> das Spiel wird ab dem naechsten Tick beruecksichtigt.
const lnkExeCache = new Map()   // lnk-Pfad -> exeName(lower) | '' (in Arbeit/fehlgeschlagen)
function lnkExeFor(p) {
  const cached = lnkExeCache.get(p)
  if (cached !== undefined) return cached || null
  lnkExeCache.set(p, '')                          // Aufloesung nur einmal anstossen
  resolveProcessName(p).then(n => lnkExeCache.set(p, (n || '').toLowerCase())).catch(() => {})
  return null
}
// Nativer Prozess-Schnappschuss (Toolhelp-API): alle laufenden Exe-Namen in <1 ms,
// ohne Subprozess. Erlaubt den 2-s-Watcher-Takt -> HDR geht beim Fremdstart an,
// BEVOR das Spiel seine Display-Faehigkeiten prueft (sonst cachet z.B. Forza
// "kein HDR" und bietet es nicht an). tasklist bleibt als Fallback.
let procSnap = null
function setupProcScan() {
  try {
    const koffi = require('koffi')
    const k32 = koffi.load('kernel32.dll')
    procSnap = {
      create: k32.func('intptr_t __stdcall CreateToolhelp32Snapshot(uint32 flags, uint32 pid)'),
      first: k32.func('int __stdcall Process32FirstW(intptr_t snap, void* pe)'),
      next: k32.func('int __stdcall Process32NextW(intptr_t snap, void* pe)'),
      close: k32.func('int __stdcall CloseHandle(intptr_t h)'),
    }
  } catch (e) { procSnap = null; console.log('[watch] Toolhelp nicht ladbar (nutze tasklist):', e && e.message) }
}
function listRunningExesNative() {
  if (!procSnap) return null
  const snap = procSnap.create(0x2 /*TH32CS_SNAPPROCESS*/, 0)
  if (!snap || snap === -1) return null
  const set = new Set()
  try {
    // PROCESSENTRY32W (x64): 568 Bytes; szExeFile (UTF-16) ab Offset 44.
    const pe = Buffer.alloc(568)
    pe.writeUInt32LE(568, 0)
    let ok = procSnap.first(snap, pe)
    while (ok) {
      let end = 44
      while (end < 564 && pe.readUInt16LE(end) !== 0) end += 2
      const name = pe.toString('utf16le', 44, end).toLowerCase()
      if (name) set.add(name)
      pe.writeUInt32LE(568, 0)
      ok = procSnap.next(snap, pe)
    }
  } catch { /* Teilergebnis reicht */ }
  try { procSnap.close(snap) } catch {}
  return set
}
function listRunningExes() {
  const native = listRunningExesNative()
  if (native && native.size) return Promise.resolve(native)
  return new Promise(resolve => {
    exec('tasklist /FO CSV /NH', { maxBuffer: 8 * 1024 * 1024 }, (err, stdout) => {
      const set = new Set()
      for (const line of String(stdout || '').split('\n')) {
        const m = line.match(/^"([^"]+)"/)
        if (m) set.add(m[1].toLowerCase())
      }
      resolve(set)
    })
  })
}
// Spielzeit gutschreiben: bevorzugt ueber den Renderer (haelt Liste/Detail live);
// ohne Fenster direkt in games.json. WICHTIG: writeFileSync schreibt UTF-8 OHNE
// BOM – niemals per PowerShell Set-Content (BOM zerstoert JSON.parse).
function creditPlaySession(gamePath, durationMs) {
  try {
    if (mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.webContents.send('play-session', { gamePath, durationMs })
      return
    }
  } catch {}
  try {
    const games = readLibraryGames()
    const g = games.find(x => x && x.path === gamePath)
    if (!g) return
    g.playtime = (g.playtime || 0) + Math.round(durationMs / 1000)   // playtime ist in Sekunden
    g.lastPlayed = Date.now()
    fs.writeFileSync(gamesFile, JSON.stringify(games))
  } catch {}
}
// Aktuelle Fremd-Sessions an den Renderer melden (Start-Knopf zeigt "läuft").
function sendExternalRunning() {
  try { if (mainWindow && !mainWindow.isDestroyed()) mainWindow.webContents.send('external-running', [...externalSessions.values()].map(s => s.gamePath)) } catch {}
}
function startExternalWatcher() {
  if (extWatchTimer) return
  setupProcScan()
  // 2-s-Takt (nativer Schnappschuss, <1 ms): Fremdstarts werden so frueh erkannt,
  // dass HDR VOR dem Grafik-Init des Spiels an ist (Forza & Co. pruefen die
  // Display-Faehigkeit nur beim Start). Session-Ende nach 2 leeren Scans (~4-6 s).
  extWatchTimer = setInterval(async () => {
    try {
      if (!gamesFile) return
      const running = await listRunningExes()
      if (!running.size) return                       // Scan-Aussetzer -> nichts beenden
      // 1) Neue fremd gestartete Bibliotheksspiele erkennen (.exe direkt,
      //    .lnk ueber den einmalig aufgeloesten Ziel-Exe-Namen)
      for (const g of readLibraryGames()) {
        if (!g || !g.path) continue
        let exe = null
        if (/\.exe$/i.test(g.path)) exe = path.basename(g.path).toLowerCase()
        else if (/\.lnk$/i.test(g.path)) exe = lnkExeFor(g.path)
        if (!exe || !exe.endsWith('.exe')) continue
        if (activeLaunchExes.has(exe) || externalSessions.has(exe)) continue
        if (!running.has(exe)) continue
        const s = { gamePath: g.path, name: g.name, startTs: Date.now(), absent: 0, hdrOn: false }
        externalSessions.set(exe, s)
        sendExternalRunning()
        playLog(`EXTERN erkannt: ${exe} (${g.name}) hdr=${g.hdr !== false}`)
        // HDR wie beim Eigenstart – aber nur, wenn nicht schon eine andere
        // Session (Eigenstart/anderes Spiel) das HDR verwaltet.
        if (g.hdr !== false && !hdrEnabledByLauncher) {
          setHDR(true); lastHdrState = true; hdrEnabledByLauncher = true; s.hdrOn = true
          try { if (mainWindow && !mainWindow.isDestroyed()) mainWindow.webContents.send('hdr-status', true) } catch {}
        }
      }
      // 2) Laufende Fremd-Sessions pruefen / beenden
      for (const [exe, s] of [...externalSessions]) {
        if (running.has(exe)) { s.absent = 0; continue }
        if (++s.absent < 3) continue                  // ~6 s Prozess-Luecke ueberbruecken (DRM-Handoff)
        externalSessions.delete(exe)
        sendExternalRunning()
        playLog(`EXTERN beendet: ${exe} Dauer ${Math.round((Date.now() - s.startTs) / 1000)}s`)
        if (s.hdrOn) {
          setHDR(false); lastHdrState = false; hdrEnabledByLauncher = false
          try { if (mainWindow && !mainWindow.isDestroyed()) mainWindow.webContents.send('hdr-status', false) } catch {}
        }
        creditPlaySession(s.gamePath, Date.now() - s.startTs)
      }
    } catch (e) { playLog('EXTERN Watcher-Fehler: ' + (e && e.message)) }
  }, 2000)
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
ipcMain.handle('list-gpus', () => listGpus())
// "Jetzt einrichten" (FPS): fruehere Ablehnung zuruecknehmen und die Einrichtung
// (Erklaer-Dialog + UAC) erneut anstossen – laeuft ueber den normalen syncFps-Weg.
// Datenquellen-Transparenz fuer den Overlay-Tab: Woher kommt (bzw. kaeme) welcher
// Wert gerade? Beantwortet "warum fehlt X" ohne Ferndiagnose.
ipcMain.handle('osd-sources', () => {
  const m = readMahm()
  const gpu = nvml ? 'NVIDIA-Treiber (NVML)'
    : adl ? 'AMD-Treiber (ADL)'
    : (m && m.gpuTemp != null) ? 'MSI Afterburner'
    : 'keine erkannt'
  const cpu = (m && m.cpuTemp != null) ? 'MSI Afterburner'
    : senseBrokerAlive() ? 'PawnIO-Treiber'
    : (cpuPawnioModule() && pawnioInstalled() && senseTaskPresent()) ? 'PawnIO-Treiber (startet mit dem OSD)'
    : appSettings.osdSetupDeclined ? 'nicht eingerichtet (nur Last/RAM/Takt)'
    : 'Einrichtung folgt beim OSD-Start (bis dahin Last/RAM/Takt)'
  const src = appSettings.osdFpsSource || 'auto'
  const useRtss = src === 'rtss' ? true : src === 'presentmon' ? false : rtssAvailable()
  const fps = useRtss ? 'RTSS/Afterburner'
    : fpsTaskPresent() ? 'PresentMon'
    : appSettings.osdSetupDeclined ? 'nicht eingerichtet'
    : 'PresentMon (Einrichtung folgt beim OSD-Start)'
  return { gpu, cpu, fps }
})

ipcMain.handle('setup-osd', () => {
  appSettings.osdSetupDeclined = false
  saveAppSettings()
  ensureOsdSetup()   // Dialog+UAC sofort anbieten (zeigt nur, was wirklich fehlt)
  syncFps()
  return appSettings
})

ipcMain.handle('set-app-settings', (event, partial) => {
  const prevSource = appSettings.osdFpsSource
  Object.assign(appSettings, partial)
  saveAppSettings()
  applyAutostart()
  if (appSettings.minimizeToTray) createTray()
  else destroyTray()
  // OSD live nachziehen, wenn eine OSD-Einstellung dabei war.
  if (partial && Object.keys(partial).some(k => k.startsWith('osd'))) {
    syncOsdVisibility()
    applyOsdConfig()
    // NUR bei echtem Quellenwechsel neu waehlen. Sonst wuerde jede Slider-
    // Bewegung stopFps/startFps ausloesen -> wanted flattert 0/1 -> der Broker
    // erwischt ein wanted=0 und beendet sich (genau der "…"-Bug).
    if (appSettings.osdFpsSource !== prevSource) stopFps()
    syncFps()
  }
  // Stream-Einstellung geaendert, waehrend ein Stream laeuft: FFmpeg mit neuen
  // Parametern (Aufloesung/Bitrate/fps/Quelle) neu starten. Der kurze Aussetzer
  // ist unkritisch; mediamtx + der WHEP-Server laufen durch, die Zuschauer
  // verbinden nach dem Neustart automatisch weiter.
  // AUSGENOMMEN sind alle Einstellungen, die KEINE Encoder-Parameter sind
  // (Audit-Befund: TURN-/IPv6-Umschalten riss den Stream grundlos ab):
  // Puffer (Zuschauer-seitig live via /cfg), TURN (wirkt live via /cfg bzw.
  // erst beim naechsten Start), IPv6-Testschalter, Hosts-Cache, Hotkey,
  // Adaptiv-Schalter (Sonderfall direkt darunter).
  const bcNoRestart = new Set(['streamBufferMs', 'streamAdaptive', 'streamTurnEnabled', 'streamTurnUrl', 'streamTurnUser', 'streamTurnPass', 'streamTurnForce', 'streamForceIPv6', 'streamLastHosts', 'streamHotkey'])
  // Adaptiv ausgeschaltet, waehrend die Bitrate gerade abgesenkt ist: zurueck
  // zur vollen eingestellten Bitrate (dafuer ist EIN Neustart gerechtfertigt).
  if (partial && partial.streamAdaptive === false && bcAdaptLevel > 0 && broadcastState.active) {
    bcAdaptLevel = 0; bcAdaptUpAt = 0; bcAdaptUpHold = 0
    bcRestartFfmpeg()
  }
  if (partial && Object.keys(partial).some(k => k.startsWith('stream') && !bcNoRestart.has(k)) && broadcastState.active) {
    bcRestartFfmpeg()
    bcPushState()
  }
  return appSettings
})

// ===========================================================================
// Live-Stream: native Pipeline (FFmpeg + mediamtx) — OBS/Discord-Klasse
// -------------------------------------------------------------------------
// FFmpeg nimmt den Bildschirm per GPU auf (ddagrab), encodet per Hardware-
// Encoder (NVENC/AMF/QSV) mit KONSTANTER Bitrate und schiebt den Stream per
// RTSP an einen lokalen mediamtx-Server. mediamtx macht daraus WebRTC (WHEP)
// und verteilt es an die Zuschauer. Ein kleiner HTTP-Server liefert die
// Player-Seite und proxyt die WHEP-Signalisierung, damit der Zuschauer nur
// EINEN Port braucht. Vorteil ggü. der alten Chromium-Aufnahme: der Hardware-
// Encoder laeuft auf eigenen GPU-Bloecken und konkurriert NICHT mit dem Spiel
// um die 3D-Shader -> stabile Bitrate/Framerate auch bei schweren Spielen.
// ===========================================================================
const http = require('http')
const BROADCAST_PORT = 8787       // TCP: player.html + WHEP-Signalisierung (Proxy vor mediamtx)
const MTX_RTSP_PORT = 8554        // localhost: FFmpeg -> mediamtx (RTSP-Ingest)
const MTX_WHEP_PORT = 8889        // localhost: mediamtx WHEP-HTTP (hinter dem Proxy)
const MTX_API_PORT = 9997         // localhost: mediamtx-API (Zuschauerzahl)
const MTX_ICE_UDP = 8189          // UDP: WebRTC-Medien (muss ans Internet)
const MTX_RTP_UDP = 8556          // localhost-UDP: RTP-Ingest FFmpeg -> mediamtx (s. bcWriteMtxConfig)
const MTX_RTCP_UDP = 8557         // localhost-UDP: zugehoeriges RTCP
const MTX_PATH = 'live'           // mediamtx-Pfadname
let broadcastServer = null        // HTTP-Server (player + WHEP-Proxy)
let mtxProc = null                // mediamtx-Kindprozess
let bcPinholeIds = []             // IPv6-Firewall-Pinholes (UniqueIDs) zum Aufraeumen
let bcV4Mapped = false            // haben WIR das IPv4-Port-Mapping gesetzt? (nie fremde Mappings loeschen)
let ffProc = null                 // FFmpeg-Kindprozess
let capProc = null                // WGC-Aufnahme-Helfer (nur bei Fenster-Aufnahme)
let audProc = null                // Audio-Helfer (WASAPI-Loopback, System-Audio)
let ffRestartTimer = null         // Auto-Neustart bei FFmpeg-Absturz
let ffRestartDebounce = null      // sammelt schnelle Einstellungsaenderungen
let ffStopping = false            // true = gewollt beendet (kein Auto-Neustart)
let bcViewerPoll = null           // Intervall: Zuschauerzahl von der mediamtx-API
let broadcastState = { active: false, port: 0, link: '', linkV4: '', linkV6: '', lanLink: '', viewers: 0, quality: '', internet: false, opening: false }
let bcPlayerHtmlCache = null
let bcEncoderCache = null         // { vendor, encoder, hw } – einmal ermittelt

function bcPlayerHtml() {
  if (bcPlayerHtmlCache == null) {
    try { bcPlayerHtmlCache = require('fs').readFileSync(require('path').join(__dirname, 'player.html'), 'utf8') }
    catch (e) { bcPlayerHtmlCache = '<!doctype html><meta charset=utf-8><body style="font-family:sans-serif">Player nicht gefunden.</body>' }
  }
  return bcPlayerHtmlCache
}
function bcLanIp() {
  const ifs = require('os').networkInterfaces()
  for (const n of Object.keys(ifs)) for (const a of ifs[n]) if (a.family === 'IPv4' && !a.internal) return a.address
  return '127.0.0.1'
}

// --- UPnP-IGD: Portfreigabe am Router automatisch (reines Node, keine Lib) ---
// Beim Streamstart oeffnen wir TCP 8787 (HTTP-Signaling) am Router und schliessen
// ihn beim Stop wieder. WebRTC-Medien brauchen KEIN Mapping: dank Cone-NAT (per
// nat-probe verifiziert) reicht STUN – Lumora sendet zuerst raus, der Rueckkanal
// bleibt offen. GetExternalIPAddress liefert die oeffentliche IP fuer den Link.
const dgram = require('dgram')
let upnpCtrl = null   // { controlURL, serviceType, localIp } – einmal aufgeloest
let upnpRouter = null // { friendlyName, manufacturer, modelName } aus der Geraete-Beschreibung
// Normalisierte TURN-Angaben (oder null) – gemeinsam fuer mediamtx UND Browser-Player.
// Der Nutzer tippt "host:port"; wir ergaenzen das noetige "turn:"-Schema.
function bcTurnServer() {
  if (!appSettings.streamTurnEnabled) return null
  let url = (appSettings.streamTurnUrl || '').trim()
  if (!url) return null
  if (!/^turns?:/i.test(url)) url = 'turn:' + url
  return { url, username: appSettings.streamTurnUser || '', password: appSettings.streamTurnPass || '' }
}
// Herstellerspezifische Kurzanleitung, wenn die UPnP-Freigabe scheitert.
function routerUpnpHint() {
  const name = (upnpRouter && (upnpRouter.modelName || upnpRouter.friendlyName)) || ''
  const nm = (name + ' ' + ((upnpRouter && upnpRouter.manufacturer) || '')).toLowerCase()
  const ports = 'Port ' + BROADCAST_PORT + ' (TCP) und ' + MTX_ICE_UDP + ' (UDP)'
  if (/avm|fritz/.test(nm)) return { router: name || 'FRITZ!Box', steps: 'In der FRITZ!Box: „Internet → Freigaben → Portfreigaben“. Beim Eintrag für diesen PC „Selbstständige Portfreigaben für dieses Gerät erlauben“ anhaken – und ganz unten „Selbstständige Portfreigaben für alle Geräte erlauben“ aktivieren.' }
  if (/speedport|telekom/.test(nm)) return { router: name || 'Speedport', steps: 'Im Speedport: „Internet → Portfreigaben“ (bzw. „Netzwerk → NAT“) öffnen und „UPnP“ bzw. „Portfreigaben automatisch zulassen“ einschalten.' }
  if (/vodafone|arris|technicolor|hitron/.test(nm)) return { router: name || 'Vodafone Station', steps: 'In der Vodafone Station den Experten-/Erweitert-Modus öffnen und „UPnP“ aktivieren (je nach Modell unter „Firewall“ oder „NAT“).' }
  return { router: name || 'deinem Router', steps: 'Aktiviere im Router „UPnP“ bzw. „automatische/selbstständige Portfreigaben“ – oder gib ' + ports + ' manuell auf diesen PC frei.' }
}
function upnpDiscover(timeoutMs) {
  return new Promise((resolve) => {
    let sock
    try { sock = dgram.createSocket({ type: 'udp4', reuseAddr: true }) } catch { return resolve([]) }
    const localIp = bcLanIp()
    const locations = []
    // Early-Exit: der Router antwortet typisch in <300 ms. Nach dem ersten
    // IGD-Fund nur noch eine kurze Nachfrist fuer weitere Antworten abwarten,
    // statt immer die volle Wartezeit auszusitzen (der Stream-Start hing sonst
    // pro Discovery-Runde fast 3 s, obwohl die Antwort laengst da war).
    let done = false, grace = null
    const finish = () => { if (done) return; done = true; if (grace) clearTimeout(grace); try { sock.close() } catch {}; resolve(locations) }
    sock.on('message', (buf) => {
      const s = buf.toString('latin1')
      if (/InternetGatewayDevice|WAN(IP|PPP)Connection/i.test(s)) {
        const loc = /LOCATION:\s*(\S+)/i.exec(s)
        if (loc && !locations.includes(loc[1])) {
          locations.push(loc[1])
          if (!grace) grace = setTimeout(finish, 250)
        }
      }
    })
    sock.on('error', () => {})
    try {
      sock.bind(0, localIp, () => {
        try { sock.setMulticastTTL(4); sock.setMulticastInterface(localIp) } catch {}
        for (const st of ['urn:schemas-upnp-org:device:InternetGatewayDevice:1', 'urn:schemas-upnp-org:service:WANIPConnection:1', 'urn:schemas-upnp-org:service:WANPPPConnection:1']) {
          const m = Buffer.from('M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "ssdp:discover"\r\nMX: 2\r\nST: ' + st + '\r\n\r\n')
          try { sock.send(m, 1900, '239.255.255.250') } catch {}
        }
      })
    } catch { return resolve([]) }
    setTimeout(finish, timeoutMs || 3000)
  })
}
function upnpHttpGet(u) {
  return new Promise((resolve, reject) => {
    const req = http.get(u, { timeout: 4000 }, (res) => { let d = ''; res.on('data', (c) => d += c); res.on('end', () => resolve({ status: res.statusCode, body: d })) })
    req.on('error', reject); req.on('timeout', () => { req.destroy(); reject(new Error('timeout')) })
  })
}
function upnpSoap(controlURL, serviceType, action, inner) {
  return new Promise((resolve, reject) => {
    const u = new (require('url').URL)(controlURL)
    const xml = '<?xml version="1.0"?>' +
      '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">' +
      '<s:Body><u:' + action + ' xmlns:u="' + serviceType + '">' + (inner || '') + '</u:' + action + '></s:Body></s:Envelope>'
    const req = http.request({
      host: u.hostname, port: u.port, path: u.pathname, method: 'POST',
      headers: { 'Content-Type': 'text/xml; charset="utf-8"', 'SOAPAction': '"' + serviceType + '#' + action + '"', 'Content-Length': Buffer.byteLength(xml) },
      timeout: 4000,
    }, (res) => { let d = ''; res.on('data', (c) => d += c); res.on('end', () => resolve({ status: res.statusCode, body: d })) })
    req.on('error', reject); req.on('timeout', () => { req.destroy(); reject(new Error('timeout')) })
    req.write(xml); req.end()
  })
}
async function upnpResolveControl() {
  if (upnpCtrl) return upnpCtrl
  const locs = await upnpDiscover(3000)
  for (const loc of locs) {
    let desc
    try { desc = await upnpHttpGet(loc) } catch { continue }
    if (!/InternetGatewayDevice/i.test(desc.body)) continue
    if (!upnpRouter) {
      const f = (t) => (new RegExp('<' + t + '>([^<]+)</' + t + '>', 'i').exec(desc.body) || [])[1] || ''
      upnpRouter = { friendlyName: f('friendlyName'), manufacturer: f('manufacturer'), modelName: f('modelName') }
    }
    const svcRe = /<service>([\s\S]*?)<\/service>/g
    let m
    while ((m = svcRe.exec(desc.body))) {
      const type = (/<serviceType>([^<]+)<\/serviceType>/i.exec(m[1]) || [])[1]
      const ctrl = (/<controlURL>([^<]+)<\/controlURL>/i.exec(m[1]) || [])[1]
      if (!type || !ctrl) continue
      const base = new (require('url').URL)(loc)
      const controlURL = ctrl.startsWith('http') ? ctrl : base.protocol + '//' + base.host + (ctrl.startsWith('/') ? '' : '/') + ctrl
      // IGDv2 (z.B. FritzBox): dieselbe Geraetebeschreibung enthaelt auch die
      // IPv6-Firewall-Control - gleich mitcachen, das erspart dem Stream-Start
      // die zweite komplette Discovery-Runde. Fehlt sie hier, discovert
      // upnpResolveV6Firewall spaeter wie bisher selbst.
      if (/WANIPv6FirewallControl/i.test(m[1])) { if (!upnpV6Ctrl) upnpV6Ctrl = { controlURL, serviceType: type }; continue }
      if (!/WAN(IP|PPP)Connection/i.test(m[1])) continue
      if (!upnpCtrl) upnpCtrl = { controlURL, serviceType: type, localIp: bcLanIp() }
    }
    if (upnpCtrl) return upnpCtrl
  }
  return null
}
async function upnpGetExternalIp() {
  const c = await upnpResolveControl(); if (!c) return null
  try {
    const r = await upnpSoap(c.controlURL, c.serviceType, 'GetExternalIPAddress')
    return (/<NewExternalIPAddress>([^<]*)<\/NewExternalIPAddress>/i.exec(r.body) || [])[1] || null
  } catch { return null }
}
async function upnpMapPort(port, proto, desc) {
  const c = await upnpResolveControl(); if (!c) return false
  const inner = '<NewRemoteHost></NewRemoteHost><NewExternalPort>' + port + '</NewExternalPort><NewProtocol>' + proto +
    '</NewProtocol><NewInternalPort>' + port + '</NewInternalPort><NewInternalClient>' + c.localIp +
    '</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>' + desc + '</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>'
  try { const r = await upnpSoap(c.controlURL, c.serviceType, 'AddPortMapping', inner); return r.status === 200 } catch { return false }
}
async function upnpUnmapPort(port, proto) {
  const c = await upnpResolveControl(); if (!c) return
  const inner = '<NewRemoteHost></NewRemoteHost><NewExternalPort>' + port + '</NewExternalPort><NewProtocol>' + proto + '</NewProtocol>'
  try { await upnpSoap(c.controlURL, c.serviceType, 'DeletePortMapping', inner) } catch {}
}

// --- IPv6-Direktweg -----------------------------------------------------------
// Bei DS-Lite/CGNAT gibt es kein oeffentliches IPv4, aber (fast immer) globales
// IPv6. Dann laeuft der Stream direkt ueber IPv6 – ohne Relay. Noetig: (1) die
// richtige Quell-IPv6 (nur die kennt der Router), (2) ein Firewall-Pinhole.

// Globale Quell-IPv6 fuer ausgehenden Verkehr. WICHTIG: Windows nutzt dafuer die
// temporaere Privacy-Adresse – und NUR die kennt der Router als Geraeteadresse,
// nur fuer die akzeptiert er einen Pinhole. Die "stabile" IPv6 wird abgelehnt.
function bcPublicIPv6() {
  return new Promise((resolve) => {
    let s
    try { s = dgram.createSocket('udp6') } catch { return resolve(null) }
    s.on('error', () => { try { s.close() } catch {}; resolve(null) })
    try {
      s.connect(53, '2001:4860:4860::8888', () => {   // Google IPv6-DNS: nur Routenwahl, kein Paket
        let a = null
        try { a = s.address().address } catch {}
        try { s.close() } catch {}
        resolve(a && /^(2|3)/.test(a) ? a : null)   // nur global unicast (2000::/3)
      })
    } catch { try { s.close() } catch {}; resolve(null) }
  })
}
let upnpV6Ctrl = null   // WANIPv6FirewallControl (IGDv2) – einmal aufgeloest
async function upnpResolveV6Firewall() {
  if (upnpV6Ctrl) return upnpV6Ctrl
  const locs = await upnpDiscover(3000)
  for (const loc of locs) {
    let desc
    try { desc = await upnpHttpGet(loc) } catch { continue }
    const svcRe = /<service>([\s\S]*?)<\/service>/g
    let m
    while ((m = svcRe.exec(desc.body))) {
      if (!/WANIPv6FirewallControl/i.test(m[1])) continue
      const type = (/<serviceType>([^<]+)<\/serviceType>/i.exec(m[1]) || [])[1]
      const ctrl = (/<controlURL>([^<]+)<\/controlURL>/i.exec(m[1]) || [])[1]
      if (!type || !ctrl) continue
      const base = new (require('url').URL)(loc)
      const controlURL = ctrl.startsWith('http') ? ctrl : base.protocol + '//' + base.host + (ctrl.startsWith('/') ? '' : '/') + ctrl
      upnpV6Ctrl = { controlURL, serviceType: type }
      return upnpV6Ctrl
    }
  }
  return null
}
// Firewall-Pinhole oeffnen (RemoteHost/Port leer = beliebiger Absender). Router
// verlangen eine endliche Lease (LeaseTime 0 lehnt die FritzBox mit 402 ab) ->
// 24h; laenger streamt praktisch niemand am Stueck. Gibt UniqueID oder null.
async function upnpAddPinhole(v6, port, proto) {
  const c = await upnpResolveV6Firewall(); if (!c || !v6) return null
  const pr = proto === 'TCP' ? 6 : 17
  const inner = '<RemoteHost></RemoteHost><RemotePort>0</RemotePort><InternalClient>' + v6 +
    '</InternalClient><InternalPort>' + port + '</InternalPort><Protocol>' + pr + '</Protocol><LeaseTime>86400</LeaseTime>'
  try {
    const r = await upnpSoap(c.controlURL, c.serviceType, 'AddPinhole', inner)
    if (r.status === 200) return (/<UniqueID>([^<]*)<\/UniqueID>/i.exec(r.body) || [])[1] || null
  } catch {}
  return null
}
async function upnpDeletePinhole(id) {
  const c = await upnpResolveV6Firewall(); if (!c || !id) return
  try { await upnpSoap(c.controlURL, c.serviceType, 'DeletePinhole', '<UniqueID>' + id + '</UniqueID>') } catch {}
}

// --- Verbindungstest: pruefen, ob Streaming beim Anwender funktioniert --------
// STUN (gleicher Socket an mehrere Server) -> oeffentliche IP + NAT-Typ.
function stunQuery(sock, host, port) {
  return new Promise((resolve) => {
    const tid = require('crypto').randomBytes(12)
    const req = Buffer.alloc(20)
    req.writeUInt16BE(0x0001, 0); req.writeUInt16BE(0, 2); req.writeUInt32BE(0x2112a442, 4); tid.copy(req, 8)
    let done = false
    const onMsg = (msg) => {
      let off = 20
      while (off + 4 <= msg.length) {
        const type = msg.readUInt16BE(off), len = msg.readUInt16BE(off + 2)
        const val = msg.slice(off + 4, off + 4 + len)
        if (type === 0x0020 || type === 0x0001) {
          const xor = type === 0x0020
          const p = val.readUInt16BE(2) ^ (xor ? 0x2112 : 0)
          const b = Buffer.from(val.slice(4, 8))
          if (xor) { b[0] ^= 0x21; b[1] ^= 0x12; b[2] ^= 0xa4; b[3] ^= 0x42 }
          return fin({ ip: b[0] + '.' + b[1] + '.' + b[2] + '.' + b[3], port: p })
        }
        off += 4 + len + ((4 - (len % 4)) % 4)
      }
      fin(null)
    }
    const fin = (r) => { if (done) return; done = true; clearTimeout(t); try { sock.removeListener('message', onMsg) } catch {}; resolve(r) }
    const t = setTimeout(() => fin(null), 3000)
    sock.on('message', onMsg)
    try { sock.send(req, port, host) } catch { fin(null) }
  })
}
function detectNat() {
  return new Promise((resolve) => {
    let sock
    try { sock = dgram.createSocket('udp4') } catch { return resolve({ ok: false }) }
    sock.on('error', () => { try { sock.close() } catch {}; resolve({ ok: false }) })
    sock.bind(async () => {
      const servers = [['stun.l.google.com', 19302], ['stun1.l.google.com', 19302], ['stun.cloudflare.com', 3478]]
      const res = []
      for (const [h, p] of servers) { const r = await stunQuery(sock, h, p); if (r) res.push(r) }
      try { sock.close() } catch {}
      if (!res.length) return resolve({ ok: false })
      const ip = res[0].ip
      const o = ip.split('.').map(Number)
      const cgn = o[0] === 100 && o[1] >= 64 && o[1] <= 127
      const priv = o[0] === 10 || (o[0] === 172 && o[1] >= 16 && o[1] <= 31) || (o[0] === 192 && o[1] === 168)
      const ports = [...new Set(res.map(r => r.port))]
      resolve({ ok: true, ip, cgn, priv, symmetric: ports.length > 1 })
    })
  })
}
async function runConnectivityTest() {
  const steps = []
  const nat = await detectNat()
  if (!nat.ok) {
    steps.push({ key: 'ip', state: 'error', label: 'Öffentliche Adresse', detail: 'Keine Antwort vom STUN-Server – ausgehendes UDP scheint blockiert (Firewall/Netzwerk). Streaming ist so nicht möglich.' })
    return { verdict: 'error', steps }
  }
  // IPv6-Direktweg vorab pruefen: globale Quell-IPv6 + laesst sich die Firewall
  // per Pinhole oeffnen? (testweise oeffnen und sofort wieder schliessen)
  const v6 = await bcPublicIPv6()
  let v6Ok = false
  if (v6) { try { const id = await upnpAddPinhole(v6, MTX_ICE_UDP, 'UDP'); if (id) { v6Ok = true; await upnpDeletePinhole(id) } } catch {} }
  if (nat.cgn || nat.priv) {
    // Kein oeffentliches IPv4 – mit funktionierendem IPv6 aber KEIN K.o. mehr.
    if (v6Ok) steps.push({ key: 'ip', state: 'warn', label: 'Öffentliche IPv4', detail: 'Kein eigenes öffentliches IPv4 (DS-Lite / Carrier-NAT, ' + nat.ip + '). Das ist aber ok – über IPv6 (siehe unten) bist du trotzdem direkt erreichbar.' })
    else steps.push({ key: 'ip', state: 'error', label: 'Öffentliche IPv4', detail: 'Dein Anschluss hat keine eigene öffentliche IPv4 (DS-Lite / Carrier-NAT, ' + nat.ip + ') und auch kein nutzbares IPv6. Direktes Streaming ist so nicht möglich – ein Relay-Server wäre nötig, oder beim Provider echtes IPv4 anfragen (oft kostenlos).' })
  } else {
    steps.push({ key: 'ip', state: 'ok', label: 'Öffentliche IPv4', detail: nat.ip })
  }
  if (nat.symmetric) {
    steps.push({ key: 'nat', state: 'warn', label: 'NAT-Typ', detail: 'Symmetrisches NAT – manche Zuschauer erreichen dich evtl. nur über eine feste Portfreigabe oder einen Relay.' })
  } else {
    steps.push({ key: 'nat', state: 'ok', label: 'NAT-Typ', detail: 'Cone-NAT – direkte Verbindung möglich.' })
  }
  // UPnP-Portfreigabe testweise: beide noetigen Ports oeffnen und sofort wieder
  // schliessen — TCP (Signalisierung) UND UDP (WebRTC-Medien).
  let tcpOk = false, udpOk = false
  try {
    tcpOk = await upnpMapPort(BROADCAST_PORT, 'TCP', 'Lumora Verbindungstest'); if (tcpOk) await upnpUnmapPort(BROADCAST_PORT, 'TCP')
    udpOk = await upnpMapPort(MTX_ICE_UDP, 'UDP', 'Lumora Verbindungstest'); if (udpOk) await upnpUnmapPort(MTX_ICE_UDP, 'UDP')
  } catch {}
  if (tcpOk && udpOk) {
    steps.push({ key: 'upnp', state: 'ok', label: 'Router-Portfreigabe (UPnP)', detail: 'Lumora kann die nötigen IPv4-Ports beim Streamstart automatisch öffnen.' })
  } else {
    const hint = routerUpnpHint()
    const found = upnpRouter ? ('Erkannter Router: ' + hint.router + '. ') : ''
    const alt = v6Ok ? ' (Zur Not läuft es aber über den IPv6-Direktweg – siehe unten.)' : (appSettings.streamTurnEnabled ? '' : ' Alternativ hilft ein Relay-Server (siehe unten).')
    steps.push({ key: 'upnp', state: 'warn', label: 'Router-Portfreigabe (UPnP)', detail: 'Der Router öffnet die IPv4-Ports nicht selbst. ' + found + hint.steps + alt })
  }
  // IPv6-Direktweg: der kostenlose Ausweg bei DS-Lite/CGNAT.
  if (v6 && v6Ok) {
    steps.push({ key: 'ipv6', state: 'ok', label: 'IPv6-Direktweg', detail: 'Globales IPv6 vorhanden und die Firewall lässt sich automatisch öffnen. Zuschauer mit IPv6 (Mobilfunk, moderne Anschlüsse) erreichen dich direkt – auch ohne IPv4-Portfreigabe.' })
  } else if (v6 && !v6Ok) {
    const hint = routerUpnpHint()
    steps.push({ key: 'ipv6', state: 'warn', label: 'IPv6-Direktweg', detail: 'Globales IPv6 ist da, aber die Firewall lässt sich nicht automatisch öffnen. ' + (upnpRouter ? 'Router: ' + hint.router + '. ' : '') + 'Erlaube im Router die selbstständigen (IPv6-)Freigaben für diesen PC.' })
  } else {
    steps.push({ key: 'ipv6', state: 'warn', label: 'IPv6-Direktweg', detail: 'Kein globales IPv6 an diesem Anschluss – dieser Weg steht nicht zur Verfügung.' })
  }
  const hasError = steps.some(s => s.state === 'error')
  const hasWarn = steps.some(s => s.state === 'warn')
  return { verdict: hasError ? 'error' : hasWarn ? 'warn' : 'ok', publicIp: nat.ip, steps }
}
ipcMain.handle('test-connectivity', () => runConnectivityTest())
// Aufnahme-Quellen: ganze Monitore (ddagrab, output_idx) UND einzelne Fenster
// (WGC-Helfer, per HWND). Die HWND steckt in Electrons Fenster-Source-ID
// "window:<hwnd>:0" – so brauchen wir keine eigene Fenster-Enumeration.
ipcMain.handle('list-sources', async () => {
  try {
    const out = []
    const displays = screen.getAllDisplays()
    const primaryId = String(screen.getPrimaryDisplay().id)
    displays.forEach((d, i) => {
      let label = 'Bildschirm ' + (i + 1) + ' – ' + Math.round(d.size.width * d.scaleFactor) + '×' + Math.round(d.size.height * d.scaleFactor)
      if (String(d.id) === primaryId) label += ' · Haupt'
      out.push({ value: 'screen:' + i, label, icon: '', kind: 'screen' })   // i = ddagrab output_idx
    })
    // Fenster per eigenem WGC-Helfer enumerieren (EnumWindows). Electrons
    // desktopCapturer verschluckt viele Fenster (Explorer/Edge u.a.); der Helfer
    // findet sie alle, und die HWND passt exakt zu dem, was er aufnehmen kann.
    const raw = await new Promise((res) => {
      try {
        require('child_process').execFile(streamBin('lumora-capture.exe'), ['--list'],
          { windowsHide: true, timeout: 6000, maxBuffer: 8 << 20 }, (e, so) => res(String(so || '')))
      } catch { res('') }
    })
    const seen = new Set()
    raw.split(/\r?\n/).forEach((line) => {
      const parts = line.split('\t')
      if (parts.length < 2) return
      const hwnd = (parts[0] || '').trim(), n = (parts[1] || '').trim(), icon = (parts[2] || '').trim()
      if (!hwnd || !n || /^lumora$/i.test(n) || /^Program Manager$/.test(n) || seen.has(hwnd)) return
      seen.add(hwnd)
      out.push({ value: 'window:' + hwnd, label: n, icon: icon ? 'data:image/png;base64,' + icon : '', kind: 'window' })
    })
    return out
  } catch { return [] }
})

// Pfad zu einer gebuendelten Binary (bin/ neben der App bzw. in resources/bin).
function streamBin(name) {
  return app.isPackaged ? path.join(process.resourcesPath, 'bin', name) : path.join(__dirname, 'bin', name)
}
// Kurzzeile ins Stream-Log (Diagnose).
let bcLogRotated = false
function bcLogStream(line) {
  if (!line) return
  try {
    const p = path.join(app.getPath('temp'), 'lumora-stream.log')
    if (!bcLogRotated) {
      bcLogRotated = true
      // Rotation (Audit-Befund: wuchs unbegrenzt, ~2-5 MB je 12-h-Stream).
      // Einmal pro App-Lauf: ueber 5 MB -> nach .old verschieben; eine
      // Vorgaenger-Generation bleibt fuer Diagnosen erhalten.
      try {
        const fs2 = require('fs')
        if (fs2.existsSync(p) && fs2.statSync(p).size > 5 * 1024 * 1024) {
          const old = path.join(app.getPath('temp'), 'lumora-stream.old.log')
          try { fs2.rmSync(old, { force: true }) } catch {}
          fs2.renameSync(p, old)
        }
      } catch {}
    }
    require('fs').appendFileSync(p, Date.now() + ' ' + line + '\n')
  } catch {}
}
// Aufloesungs-Obergrenzen (Hoehe). 'auto'/2160 = native Aufloesung (kein Scale).
const STREAM_Q_H = { '2160': 2160, '1440': 1440, '1080': 1080, '720': 720 }

// GPU-Hersteller ermitteln und den passenden Hardware-Encoder waehlen. Die
// Marke der GPU bestimmt den Hardware-Encoder (NVENC/AMF/QSV). KEIN Software-
// Fallback: der FFmpeg-Build ist LGPL und hat bewusst kein libx264 (das waere
// GPL). Ohne HW-Encoder bleibt encoder=null -> startBroadcast bricht mit Hinweis
// ab. Einmal ermittelt und gecacht.
async function bcDetectEncoder() {
  if (bcEncoderCache) return bcEncoderCache
  let vendor = ''
  try {
    const out = await new Promise((res) => {
      require('child_process').execFile('powershell', ['-NoProfile', '-Command',
        "(Get-CimInstance Win32_VideoController).Name -join '|'"], { windowsHide: true, timeout: 8000 },
        (e, so) => res(String(so || '')))
    })
    const s = out.toLowerCase()
    if (/nvidia|geforce|rtx|gtx/.test(s)) vendor = 'nvidia'
    else if (/radeon|\bamd\b/.test(s)) vendor = 'amd'
    else if (/intel|\barc\b|iris|uhd/.test(s)) vendor = 'intel'
  } catch {}
  let enc = null, hw = false
  try {
    const encs = await new Promise((res) => {
      require('child_process').execFile(streamBin('ffmpeg.exe'), ['-hide_banner', '-encoders'],
        { windowsHide: true, timeout: 8000 }, (e, so) => res(String(so || '')))
    })
    const has = (n) => encs.includes(n)
    if (vendor === 'nvidia' && has('h264_nvenc')) { enc = 'h264_nvenc'; hw = true }
    else if (vendor === 'amd' && has('h264_amf')) { enc = 'h264_amf'; hw = true }
    else if (vendor === 'intel' && has('h264_qsv')) { enc = 'h264_qsv'; hw = true }
    else if (has('h264_nvenc')) { enc = 'h264_nvenc'; hw = true }   // GPU unklar: NVENC zuerst
    else if (has('h264_amf')) { enc = 'h264_amf'; hw = true }
    else if (has('h264_qsv')) { enc = 'h264_qsv'; hw = true }
  } catch {}
  bcEncoderCache = { vendor, encoder: enc, hw }
  osdDbg('[stream] Encoder: ' + enc + ' (GPU: ' + (vendor || '?') + ', hw=' + hw + ')')
  return bcEncoderCache
}
// Encoder-spezifische CBR-/Low-Latency-Parameter. Konstante Bitrate (CBR) ist
// der Schluessel: keine adaptive Drossel, die bei Sende-Jitter einbricht ->
// gleichmaessiges Bild ohne Klotz-Artefakte bei Bewegung.
// AMF-Sonderfall (WHEP-Kernbug): h264_amf ignoriert im 'lowlatency'-Usage die
// GOP-Laenge (-g) und sendet nur EINEN IDR-Keyframe beim Stream-Start, danach
// ausschliesslich P-Frames. Jeder WebRTC-Zuschauer verbindet sich aber SPAETER
// -> er bekommt nie einen Referenz-Keyframe -> Bild bleibt schwarz (Monitor)
// bzw. der Player kommt gar nicht erst hoch (Fenster). NVENC/QSV honorieren -g
// und liefern alle 2s ein IDR, daher dort kein Problem. Fix: -force_key_frames
// erzwingt encoderseitig alle 2s einen Keyframe, -forced_idr macht ihn zum
// echten IDR (AMF-Default: normales I-Frame, das den Decoder NICHT resettet).
function bcEncoderArgs(enc, kbit, fps) {
  const rate = kbit + 'k', buf = Math.round(kbit / 2) + 'k'
  // GOP = 1 Sekunde (frueher 2 s): Jeder neue Zuschauer (und die Vorschau) kann
  // erst ab einem Keyframe Bild zeigen – 1s-GOP halbiert diese Wartezeit auf im
  // Schnitt 0,5 s. Kostet bei CBR etwas Effizienz, fuer fluessiges Join-Gefuehl.
  const gop = String(Math.max(1, Math.round(fps || 60)))
  if (enc === 'h264_amf') return ['-c:v', 'h264_amf', '-usage', 'lowlatency', '-rc', 'cbr', '-b:v', rate, '-maxrate', rate, '-bufsize', buf, '-g', gop, '-bf', '0', '-forced_idr', '1', '-force_key_frames', 'expr:gte(t,n_forced*1)']
  if (enc === 'h264_qsv') return ['-c:v', 'h264_qsv', '-preset', 'veryfast', '-look_ahead', '0', '-b:v', rate, '-maxrate', rate, '-bufsize', buf, '-g', gop, '-bf', '0']
  // Default NVENC (haeufigster HW-Encoder; auch defensiver Fallback). Ohne
  // HW-Encoder startet der Stream gar nicht erst – kein libx264 im LGPL-Build.
  return ['-c:v', 'h264_nvenc', '-preset', 'p4', '-tune', 'll', '-rc', 'cbr', '-b:v', rate, '-maxrate', rate, '-bufsize', buf, '-g', gop, '-bf', '0', '-no-scenecut', '1']
}
// Streaming-Parameter aus den Einstellungen ableiten.
function bcStreamCfg(enc) {
  const q = appSettings.streamQuality || '1080'
  let scaleH = STREAM_Q_H[q] || 0
  if (scaleH === 2160) scaleH = 0            // 4K = native Aufnahme, kein Scale
  // Adaptive Bitrate: bei leidenden Zuschauern stufenweise unter der Einstellung
  // (bcAdaptKbit ist neutral, solange Stufe 0 oder der Schalter aus ist).
  const kbit = bcAdaptKbit(Math.max(1000, Math.round(appSettings.streamUploadKbit || 8000)))
  const src = String(appSettings.streamSource || '')
  let mode = 'monitor', outputIdx = 0, hwnd = 0
  if (src.startsWith('window:')) { hwnd = parseInt(src.slice(7), 10) || 0; if (hwnd) mode = 'window' }
  else if (src.startsWith('screen:')) { const n = parseInt(src.slice(7), 10); if (!isNaN(n)) outputIdx = n }
  // Monitor-Weg: Preset-Hoehe >= native Monitorhoehe -> NICHT skalieren (Audit-
  // Befund). Sonst liefe der haeufigste Fall (1080p-Monitor + 1080p-Preset)
  // unnoetig durch die CPU-Kette statt Zero-Copy, und ein Preset UEBER der
  // Monitorhoehe wuerde sinnlos HOCHskaliert (weicheres Bild, mehr Last).
  // Physische Pixel (size*scaleFactor) wie im Quellen-Label; Index-Konvention
  // Electron-Display == ddagrab output_idx ist im Quellen-Picker etabliert.
  if (mode === 'monitor' && scaleH) {
    try {
      const d = require('electron').screen.getAllDisplays()[outputIdx]
      if (d && scaleH >= Math.round(d.size.height * d.scaleFactor)) scaleH = 0
    } catch {}
  }
  return { encoder: enc.encoder, fps: appSettings.streamFps || 60, kbit, scaleH, mode, outputIdx, hwnd }
}
function bcQualityLabel(cfg) {
  const h = cfg.scaleH ? cfg.scaleH + 'p' : 'nativ'
  return h + ' · ' + cfg.fps + ' fps · ' + Math.round(cfg.kbit / 1000) + ' Mbit · ' + cfg.encoder.replace('h264_', '').toUpperCase()
}
// FFmpeg-Kommandozeile. Zwei Quellen:
//  - Monitor: ddagrab (GPU-Desktop-Duplication).
//  - Fenster: BGRA-Rohframes vom WGC-Helfer ueber stdin (pipe:0); capW/capH ist
//    die vom Helfer gemeldete Fenstergroesse.
// HINWEIS: GPU-Scale/HDR-Tonemapping kommen als Verfeinerung; hier der Basis-Pfad.
function bcBuildFfmpegArgs(cfg, capW, capH, withAudio) {
  // UDP-Transport + 8-MB-Sendepuffer: der TCP-Ingest von mediamtx blockierte
  // den Publisher periodisch (Beweiskette in bcWriteMtxConfig).
  const rtsp = ['-f', 'rtsp', '-rtsp_transport', 'udp', '-buffer_size', '8388608', 'rtsp://127.0.0.1:' + MTX_RTSP_PORT + '/' + MTX_PATH]
  // System-Audio (fd 3, f32le 48k stereo vom Helfer) -> Opus. -map bindet Bild
  // (Input 0) + Ton (Input 1) explizit zusammen.
  const aIn = withAudio ? ['-thread_queue_size', '4096', '-f', 'f32le', '-ar', '48000', '-ac', '2', '-i', 'pipe:3'] : []
  // -max_interleave_delta 0: der Muxer haelt KEINE Videopakete zurueck, um auf
  // Audio zu warten -> das Bild stockt nicht, wenn der (ueber Node laufende)
  // Ton kurz jittert. A/V bleiben ueber die RTP-Timestamps trotzdem synchron.
  const aEnc = withAudio ? ['-map', '0:v:0', '-map', '1:a:0', '-c:a', 'libopus', '-b:a', '128k', '-max_interleave_delta', '0'] : []
  if (cfg.mode === 'window') {
    const vf = []
    if (cfg.scaleH && capH && cfg.scaleH < capH) vf.push('scale=-2:' + cfg.scaleH + ':flags=bicubic')
    vf.push('format=yuv420p')
    // -progress: Encoder-Eigenauskunft (frame/fps/speed) alle 5s auf stderr -
    // Diagnose der maxwrite-Stalls: bricht der AUSSTOSS ein (GPU-These) oder
    // nur die Pipe-Annahme? Wird in bcSpawnEncoder kompakt geloggt.
    return ['-hide_banner', '-loglevel', 'warning', '-progress', 'pipe:2', '-stats_period', '5',
      '-thread_queue_size', '4096', '-f', 'rawvideo', '-pixel_format', 'bgra', '-video_size', capW + 'x' + capH, '-framerate', String(cfg.fps),
      // Named Pipe des Helfers (grosser Puffer) statt stdin - s. bcStartWindowCapture.
      '-i', cfg.vidPipe ? '\\\\.\\pipe\\' + cfg.vidPipe : 'pipe:0',
      ...aIn,
      '-vf', vf.join(','),
      ...bcEncoderArgs(cfg.encoder, cfg.kbit, cfg.fps),
      ...aEnc, ...rtsp]
  }
  // Monitor-Weg (ddagrab). Bei Ton wird der Audio-Helfer zu Input 0 (pipe:3); das
  // ddagrab-Bild kommt aus dem Filtergraphen und wird per Label [v] explizit
  // gemappt (sonst zieht FFmpeg ohne -map das Audio als "Video" heran).
  // GPU-Direktkette (NVENC, keine Skalierung noetig): ddagrabs D3D11-Texturen
  // gehen unveraendert in den Encoder, der BGRA->NV12 auf der GPU wandelt.
  // hwdownload+format+scale (CPU-Umweg, bei 4K ~2 GB/s memcpy) entfallen
  // komplett. Nur fuer diesen Fall: GPU-Skalierung kann das mitgelieferte
  // FFmpeg nicht (scale_d3d11 bricht beim Konfigurieren ab, D3D11->CUDA-
  // Ableitung nicht einkompiliert - beides real getestet), und AMF/QSV sind
  // mit D3D11-Frames ungetestet -> ueberall sonst bleibt die CPU-Kette.
  const zeroCopy = cfg.encoder === 'h264_nvenc' && !cfg.scaleH && !bcZeroCopyBroken
  bcLastZeroCopy = zeroCopy
  const vf = ['ddagrab=output_idx=' + cfg.outputIdx + ':framerate=' + cfg.fps]
  if (!zeroCopy) {
    vf.push('hwdownload', 'format=bgra')
    if (cfg.scaleH) vf.push('scale=-2:' + cfg.scaleH + ':flags=bicubic')
    vf.push('format=yuv420p')
  }
  bcLogStream('video: ddagrab ' + (zeroCopy ? 'GPU-direkt (zero-copy)' : 'CPU-Kette' + (cfg.scaleH ? ', scale ' + cfg.scaleH + 'p' : '')))
  const mMap = withAudio ? ['-map', '[v]', '-map', '0:a:0', '-c:a', 'libopus', '-b:a', '128k', '-max_interleave_delta', '0'] : []
  return ['-hide_banner', '-loglevel', 'warning', '-progress', 'pipe:2', '-stats_period', '5',
    ...aIn,
    '-filter_complex', vf.join(',') + (withAudio ? '[v]' : ''),
    ...bcEncoderArgs(cfg.encoder, cfg.kbit, cfg.fps),
    ...mMap, ...rtsp]
}
// mediamtx-Konfiguration schreiben (Node fs -> UTF-8 OHNE BOM). Nur die noetigen
// Dienste: RTSP-Ingest (localhost), WebRTC/WHEP-Egress + API (Zuschauerzahl).
// hosts -> oeffentliche IPv4 UND/ODER globale IPv6 als zusaetzliche ICE-Hosts,
// damit Internet-Zuschauer die externe(n) Adresse(n) als Kandidaten bekommen.
function bcWriteMtxConfig(hosts) {
  const hostList = (Array.isArray(hosts) ? hosts : [hosts]).filter(Boolean)
  const lines = [
    'logLevel: error',
    'rtspAddress: 127.0.0.1:' + MTX_RTSP_PORT,
    // RTP-Ingest ueber UDP statt TCP: mediamtx' TCP-Ingest-Pfad blockiert den
    // Publisher periodisch fuer 100-400 ms (isoliert belegt: identische 4K-
    // Testkette 4 min ohne Zuschauer -> TCP: 167 Pipe-Stalls/speed 0.9x,
    // UDP: 0 Stalls/speed 1.0x; tcp_nodelay half NICHT). Auf localhost ist
    // UDP verlustfrei, solange die Puffer Keyframe-Bursts schlucken - dafuer
    // udpReadBufferSize hier + buffer_size auf der FFmpeg-Seite (ohne diese
    // Puffer real gemessen: "RTP packets lost" + FU-A-Fehler, mit: 0).
    'rtspTransports: [udp, tcp]',
    'rtpAddress: 127.0.0.1:' + MTX_RTP_UDP,
    'rtcpAddress: 127.0.0.1:' + MTX_RTCP_UDP,
    'udpReadBufferSize: 26214400',
    'rtmp: no', 'hls: no', 'srt: no', 'playback: no', 'metrics: no', 'pprof: no',
    'api: yes', 'apiAddress: 127.0.0.1:' + MTX_API_PORT,
    'webrtcAddress: 127.0.0.1:' + MTX_WHEP_PORT,
    'webrtcLocalUDPAddress: :' + MTX_ICE_UDP,
    "webrtcLocalTCPAddress: ''",
    'webrtcIPsFromInterfaces: yes',
    // Sende-Warteschlange je Zuschauer: der Default (512 Pakete) kann bei
    // Keyframe-Bursts + mehreren Zuschauern ueberlaufen -> mediamtx verwirft
    // dann Pakete ("verschluckte" Stellen beim Zuschauer). Grosszuegig dimensionieren.
    'writeQueueSize: 2048',
    // IPv6 MUSS gequotet werden (Doppelpunkte sonst = YAML-Mapping).
    hostList.length ? 'webrtcAdditionalHosts: [' + hostList.map((h) => "'" + h + "'").join(', ') + ']' : '',
    'webrtcHandshakeTimeout: 10s',
  ].filter((x) => x !== '')
  // TURN als ICE-Server fuer mediamtx: liefert einen relay-Kandidaten, wenn der
  // Streamer nicht direkt erreichbar ist (CGNAT/IPv6-only). So laeuft das Medium
  // ueber den Relay statt zu scheitern.
  const turn = bcTurnServer()
  if (turn) {
    lines.push('webrtcICEServers2:', '  - url: ' + turn.url)
    if (turn.username) lines.push('    username: ' + turn.username)
    if (turn.password) lines.push('    password: ' + turn.password)
  }
  lines.push('paths:', '  ' + MTX_PATH + ':', '    source: publisher', '')
  const p = path.join(app.getPath('temp'), 'lumora-mediamtx.yml')
  require('fs').writeFileSync(p, lines.join('\n'), 'utf8')
  return p
}
// --- Adaptive Bitrate (QoS-gesteuert) -------------------------------------------
// Die Player melden alle 5 s ihre Empfangsqualitaet an /qos (Paketverlust,
// Jitter, verworfene/eingefrorene Frames; Grid-Kacheln via gruppe.php-Relay).
// Leidet ein Zuschauer anhaltend, senkt Lumora die Encoder-Bitrate stufenweise
// (FFmpeg-Neustart; mediamtx + Server laufen durch, die Player-Watchdogs
// verbinden automatisch neu); bleibt der Empfang laenger sauber, geht es
// stufenweise zurueck zur eingestellten Bitrate. Hintergrund: feste CBR ueber
// schwankenden Mobilfunk = Funkstau -> Einfrieren + Schnelllauf + Ton-Risse
// beim Zuschauer (real diagnostiziert: 1440p/16 Mbit auf Handy; Sender
// nachweislich makellos). Ein Encode fuer alle -> es zaehlt der schlechteste
// aktive Zuschauer; die eigene Vorschau (localhost) regelt NICHT mit.
// Stufen-Faktoren auf die eingestellte Bitrate. Zwei tiefe Stufen ergaenzt
// (0.24/0.16), damit hohe Basiswerte fuer schwaches Mobilfunknetz weit genug
// heruntergeregelt werden: 25 Mbit -> 17,5 / 12,5 / 8,8 / 6,0 / 4,0 Mbit.
// (Vorher endete die Kette bei 8,8 Mbit - fuer 4K auf Mobilfunk zu hoch,
// Log-belegt 39-50% Paketverlust.) Fuer niedrige Basiswerte fangen tiefe
// Stufen ohnehin am 3-Mbit-Boden ab (bcAdaptKbit); der No-op-Neustart-Schutz
// verhindert dort wirkungslose Neustarts.
const BC_ADAPT_F = [1, 0.7, 0.5, 0.35, 0.24, 0.16]
let bcAdaptLevel = 0                     // aktuelle Stufe (0 = volle Bitrate)
let bcAdaptLastChange = 0                // letzter Stufenwechsel (Hysterese)
let bcAdaptBadSince = 0, bcAdaptGoodSince = 0
let bcAdaptUpAt = 0                      // Zeitpunkt der letzten Rauf-Probe
let bcAdaptUpHold = 0                    // Rauf-Sperre nach gescheiterter Probe (Anti-Pendeln)
let bcQosMap = new Map()                 // viewerId/IP -> juengster QoS-Report
let bcQosLogLast = new Map()             // Drossel fuer OK-Logzeilen
let bcAdaptTimer = null
function bcQosReport(ip, q) {
  if (!q || typeof q !== 'object') return
  if (/^(::1|127\.|::ffff:127\.)/.test(String(ip))) return   // eigene Vorschau
  const recv = Math.max(0, q.recv | 0), lost = Math.max(0, q.lost | 0)
  const drop = Math.max(0, q.drop | 0), frz = Math.max(0, q.frz | 0)
  const lossRate = (lost + recv) > 0 ? lost / (lost + recv) : 0
  // "leidet": spuerbarer Paketverlust ODER eingefrorene Wiedergabe ODER viele
  // verworfene Frames im 5-s-Fenster.
  const bad = lossRate > 0.02 || frz > 0 || drop > 15
  const key = String(q.id || ip).slice(0, 40)
  // badStreak: erst ZWEI schlechte Reports in Folge zaehlen als "leidet" -
  // ein einzelner Freeze (Tab-Wechsel am Handy, kurzer Funk-Blip) drosselt
  // sonst sofort alle Zuschauer (Audit-Befund).
  const prev = bcQosMap.get(key)
  bcQosMap.set(key, { t: Date.now(), bad, lossRate, badStreak: bad ? (((prev && prev.badStreak) || 0) + 1) : 0 })
  const last = bcQosLogLast.get(key) || 0
  if (bad || Date.now() - last > 60000) {
    bcLogStream('qos: ' + key + ' lost=' + lost + '/' + (lost + recv) + ' jit=' + (q.jit | 0) + 'ms fps=' + (q.fps == null ? '?' : Math.round(q.fps)) + ' drop=' + drop + ' frz=' + frz + (bad ? ' !' : ''))
    bcQosLogLast.set(key, Date.now())
  }
}
function bcAdaptKbit(baseKbit, level) {
  const lv = level == null ? bcAdaptLevel : level
  if (lv <= 0 || appSettings.streamAdaptive === false) return baseKbit
  return Math.max(3000, Math.round(baseKbit * BC_ADAPT_F[lv] / 100) * 100)
}
function bcAdaptTick() {
  if (!broadcastState.active || appSettings.streamAdaptive === false) return
  const now = Date.now()
  let bad = false, worstLoss = 0
  for (const [k, r] of bcQosMap) {
    if (now - r.t > 15000) { bcQosMap.delete(k); bcQosLogLast.delete(k); continue }
    if ((r.badStreak || 0) >= 2) bad = true
    if (now - r.t <= 8000 && (r.lossRate || 0) > worstLoss) worstLoss = r.lossRate || 0
  }
  const maxLevel = BC_ADAPT_F.length - 1
  const base = Math.max(1000, Math.round(appSettings.streamUploadKbit || 8000))
  // NOTABSTIEG: katastrophaler Paketverlust (>=12%, normale Schwelle ist 2%) ->
  // SOFORT mehrere Stufen tiefer, OHNE die 2-Meldungen-/8s-/20s-Gates der sanften
  // Regelung. Genau der Fall 4K@25 Mbit auf schwaches Mobilfunknetz (real:
  // 39-50% Verlust, erster Abstieg bisher erst nach ~19s und nur EINE Stufe).
  // Ueberschiessen ist unkritisch - die sanfte Rauf-Logik (90s sauber) holt die
  // Bitrate danach zurueck. 6s-Sperre + Map-Leerung verhindern, dass STALE
  // Severe-Reports (Bitrate VOR dem Neustart) mehrfach nachfeuern.
  if (worstLoss >= 0.12 && bcAdaptLevel < maxLevel && now - bcAdaptLastChange >= 6000) {
    const oldKbit = bcAdaptKbit(base)
    const target = Math.min(maxLevel, bcAdaptLevel + (worstLoss >= 0.30 ? 3 : 2))
    const targetKbit = bcAdaptKbit(base, target)
    if (targetKbit < oldKbit) {
      bcAdaptLevel = target
      bcAdaptLastChange = now
      bcAdaptBadSince = 0; bcAdaptGoodSince = 0
      bcQosMap.clear(); bcQosLogLast.clear()
      bcLogStream('adapt: NOTABSTIEG bei ' + Math.round(worstLoss * 100) + '% Verlust -> Stufe ' + target + ' -> ' + targetKbit + ' kbit')
      bcRestartFfmpeg()
    }
    return
  }
  if (bad) { if (!bcAdaptBadSince) bcAdaptBadSince = now; bcAdaptGoodSince = 0 }
  else { bcAdaptBadSince = 0; if (!bcAdaptGoodSince) bcAdaptGoodSince = now }
  // Runter: >=8 s anhaltend schlecht (>=2 Reports in Folge). Rauf: 90 s sauber
  // UND keine Rauf-Sperre - scheitert eine Rauf-Probe (danach binnen 60 s
  // wieder schlecht), pausieren weitere Versuche 10 min. Sonst pendelt ein
  // dauerhaft schwacher Zuschauer die Kette ewig rauf/runter, mit einem
  // Doppel-Aussetzer fuer ALLE alle ~2 Minuten (Audit-Befund).
  // Zwischen Wechseln >=20 s Ruhe - jeder Wechsel ist ein kurzer Neustart.
  let next = bcAdaptLevel
  if (bad && bcAdaptBadSince && now - bcAdaptBadSince >= 8000 && bcAdaptLevel < BC_ADAPT_F.length - 1) next = bcAdaptLevel + 1
  else if (!bad && bcAdaptGoodSince && now - bcAdaptGoodSince >= 90000 && bcAdaptLevel > 0 && now >= bcAdaptUpHold) next = bcAdaptLevel - 1
  if (next === bcAdaptLevel || now - bcAdaptLastChange < 20000) return
  const oldKbit = bcAdaptKbit(base)
  const goingDown = next > bcAdaptLevel   // hoehere Stufe = niedrigere Bitrate
  if (goingDown && bcAdaptUpAt && now - bcAdaptUpAt < 60000) {
    bcAdaptUpHold = now + 600000
    bcLogStream('adapt: Rauf-Probe gescheitert -> Empfang traegt die hoehere Stufe nicht, naechster Versuch in 10 min')
  }
  if (!goingDown) bcAdaptUpAt = now
  bcAdaptLevel = next
  bcAdaptLastChange = now
  bcAdaptBadSince = 0; bcAdaptGoodSince = 0
  const newKbit = bcAdaptKbit(base)
  // Identische Bitrate (z.B. 6-Mbit-Basis: Stufe 2 und 3 = je 3000-Floor):
  // Stufe merken, aber KEIN wirkungsloser Neustart mit 10-s-Aussetzer (Audit).
  if (newKbit === oldKbit) { bcLogStream('adapt: Stufe ' + next + ' (Bitrate unveraendert ' + newKbit + ' kbit - kein Neustart)'); return }
  bcLogStream('adapt: Stufe ' + next + ' -> ' + newKbit + ' kbit (' + (goingDown ? 'Zuschauer-Empfang schlecht' : 'Empfang wieder stabil') + ')')
  bcRestartFfmpeg()
}

// HTTP-Server auf dem freigegebenen Port: liefert die Player-Seite und proxyt
// die WHEP-Signalisierung an mediamtx (localhost). So braucht der Zuschauer nur
// DIESEN einen TCP-Port fuer die Signalisierung; die Medien laufen per WebRTC
// direkt ueber mediamtx' ICE-UDP-Port.
function bcStartServer(port) {
  return new Promise((resolve, reject) => {
    const srv = http.createServer((req, res) => {
      // CORS: player.html/preview riefen bisher immer NUR den eigenen Server auf
      // (same-origin, kein CORS noetig). Die Grid-Seite verbindet dagegen zu den
      // Servern ANDERER Gruppenmitglieder (echtes Cross-Origin) - daher generell
      // erlauben. Der Endpunkt ist ohnehin oeffentlich/unauthentifiziert per Design.
      res.setHeader('Access-Control-Allow-Origin', '*')
      res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PATCH, DELETE, OPTIONS')
      res.setHeader('Access-Control-Allow-Headers', 'Content-Type')
      if (req.method === 'OPTIONS') { res.writeHead(204); return res.end() }
      if (req.method === 'GET' && (req.url === '/' || req.url.startsWith('/?') || req.url === '/index.html')) {
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' })
        return res.end(bcPlayerHtml())
      }
      // Der Player holt hierher den vom Streamer eingestellten Jitter-Puffer (ms)
      // und wendet ihn live an – regelmaessig, damit Regler-Aenderungen auch bei
      // bereits verbundenen Zuschauern ohne Neuladen greifen.
      if (req.method === 'GET' && req.url === '/cfg') {
        const ice = [{ urls: 'stun:stun.l.google.com:19302' }, { urls: 'stun:stun.cloudflare.com:3478' }]
        const turn = bcTurnServer()
        if (turn) {
          // WebRTC verlangt bei turn:-URLs IMMER username+credential (sonst wirft
          // Chrome InvalidAccessError und die PeerConnection scheitert komplett) –
          // auch bei auth-losen Servern. Darum Platzhalter, die ein no-auth-Server
          // schlicht ignoriert.
          ice.push({ urls: turn.url, username: turn.username || 'lumora', credential: turn.password || 'lumora' })
        }
        res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' })
        return res.end(JSON.stringify({
          buffer: Math.max(0, appSettings.streamBufferMs || 120),
          iceServers: ice,
          forceRelay: !!(turn && appSettings.streamTurnForce),   // zum Testen: nur ueber TURN
        }))
      }
      // Identitaets-Endpunkt: verraet, WELCHE Lumora-Instanz unter dieser Adresse
      // antwortet. Noetig fuer den Fall "zwei PCs hinter demselben Router": bevor
      // ein zweiter PC das IPv4-Port-Mapping beansprucht, fragt er hier nach, ob
      // die oeffentliche IPv4:Port schon einem ANDEREN Lumora gehoert (sonst wuerde
      // er dessen Mapping je nach Router stehlen und den laufenden Stream abreissen).
      if (req.method === 'GET' && req.url === '/instanz') {
        res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' })
        // group: laufender Raumcode - der zweite eigene PC im selben Netz nutzt das
        // fuer den automatischen Gruppen-Beitritt beim Stream-Start.
        return res.end(JSON.stringify({ lumora: true, id: groupMemberId(), group: bcGroup ? bcGroup.code : null }))
      }
      // WHEP: POST = Offer->Answer, PATCH = ICE-Trickle, DELETE = Abmelden.
      // (Die Stream-Tab-Vorschau nutzt denselben /whep-Weg per localhost, ueber IPC.)
      if (req.url === '/whep' || req.url.startsWith('/whep')) return bcProxyWhep(req, res)
      // QoS-Bericht eines Players (adaptive Bitrate, s. bcQosReport).
      if (req.method === 'POST' && req.url === '/qos') {
        let body = ''
        req.on('data', (c) => { body += c; if (body.length > 4096) req.destroy() })
        req.on('end', () => {
          try { bcQosReport(req.socket.remoteAddress || '', JSON.parse(body)) } catch {}
          res.writeHead(204); res.end()
        })
        return
      }
      res.writeHead(404); res.end('not found')
    })
    srv.on('error', reject)
    srv.listen(port, () => resolve(srv))
  })
}
// --- Gruppen-Stream: Freunde sehen sich gegenseitig im Grid ------------------
// Vermittlung ueber ein kleines PHP-Skript (gruppe.php) auf einem Webspace: Es
// verwaltet Raumcodes + die aktuellen Adressen der Mitglieder, liefert den
// Grid-Player per HTTPS aus (dadurch iPhone/Safari-tauglich und der Zuschauer-
// Link bleibt die GANZE Sitzung stabil, egal wer kommt/geht) und reicht die
// WHEP-Handshakes durch. Der VIDEO-Traffic laeuft weiterhin direkt P2P
// Browser<->Streamer - kein Byte Video ueber den Webspace. Die URL ist
// austauschbar (Einstellung groupRelayUrl, Standard unten); das Skript liegt
// der Installation bei (resources/gruppe.php) - jeder kann es auf den eigenen
// Webspace legen und bleibt damit unabhaengig.
const GROUP_HEARTBEAT_MS = 8000     // Intervall fuer den Anwesenheits-Heartbeat
const GROUP_RELAY_DEFAULT = 'https://lumora.kara-webdesign.de/gruppe.php'
let bcGroup = null                  // null = nicht in einer Gruppe, sonst { code, members: [], relayFails }
let bcGroupTimer = null

function groupRelayUrl() {
  const u = String(appSettings.groupRelayUrl || '').trim()
  return u || GROUP_RELAY_DEFAULT
}
// Anfrage an die Vermittlung (https ODER http, je nach eingetragener URL).
// Mit bodyObj = POST, sonst GET. Kurzer Timeout, niemals werfen.
function groupRelay(action, params, bodyObj) {
  return new Promise((resolve) => {
    let u
    try {
      u = new (require('url').URL)(groupRelayUrl())
      u.searchParams.set('a', action)
      for (const k in (params || {})) u.searchParams.set(k, params[k])
    } catch { return resolve(null) }
    const mod = u.protocol === 'https:' ? https : http
    const body = bodyObj ? Buffer.from(JSON.stringify(bodyObj)) : null
    const req = mod.request(u, {
      method: body ? 'POST' : 'GET', timeout: 8000,
      headers: body ? { 'Content-Type': 'application/json', 'Content-Length': body.length } : {},
    }, (res) => {
      let d = ''
      res.on('data', (c) => { d += c; if (d.length > 500000) req.destroy() })
      res.on('end', () => { try { resolve(JSON.parse(d)) } catch { resolve(null) } })
    })
    req.on('error', () => resolve(null))
    req.on('timeout', () => { req.destroy(); resolve(null) })
    req.end(body)
  })
}

function groupMemberId() {
  if (!appSettings.groupMemberId) {
    appSettings.groupMemberId = require('crypto').randomBytes(6).toString('hex')
    saveAppSettings()
  }
  return appSettings.groupMemberId
}
function groupDisplayName() {
  try { return require('os').userInfo().username.slice(0, 24) || 'Spieler' } catch { return 'Spieler' }
}
// Roh-JSON an eine Basisadresse ("http://ip:port/" bzw. "http://[v6]:port/")
// senden. Kurzer Timeout - eine tote Adresse soll den Heartbeat nicht blockieren.
function groupHttpJson(base, urlPath, method, bodyObj, timeoutMs) {
  return new Promise((resolve) => {
    let u
    try { u = new (require('url').URL)(urlPath, base) } catch { return resolve(null) }
    const body = bodyObj ? Buffer.from(JSON.stringify(bodyObj)) : null
    const req = http.request({
      host: u.hostname, port: u.port || 80, path: u.pathname, method, timeout: timeoutMs || 4000,
      headers: body ? { 'Content-Type': 'application/json', 'Content-Length': body.length } : {},
    }, (res) => {
      let d = ''
      res.on('data', (c) => { d += c; if (d.length > 200000) req.destroy() })
      res.on('end', () => { try { resolve(JSON.parse(d)) } catch { resolve(null) } })
    })
    req.on('error', () => resolve(null))
    req.on('timeout', () => { req.destroy(); resolve(null) })
    req.end(body)
  })
}
function groupSelfEntry() {
  return {
    id: groupMemberId(), name: groupDisplayName(),
    linkV4: broadcastState.linkV4 || null, linkV6: broadcastState.linkV6 || null,
    // Mitgliedschaft != Streamen: Wer seinen Stream stoppt, bleibt in der Gruppe
    // (Server laeuft weiter) und erscheint im Grid als "pausiert" statt zu verschwinden.
    streaming: !!broadcastState.active,
  }
}
function groupPushState() { sendToUi('group-status', groupPublicState()) }
function groupPublicState() {
  // lastCode auch im Nicht-Gruppen-Zustand mitliefern: damit kann die Oberflaeche
  // das Beitreten-Feld vorbefuellen - Wiederbeitritt ist dann ein Klick.
  if (!bcGroup) return { active: false, lastCode: appSettings.groupLastCode || '' }
  return {
    active: true,
    code: bcGroup.code,
    link: groupRelayUrl() + '?code=' + bcGroup.code,
    members: (bcGroup.members || []).map((m) => ({ id: m.id, name: m.name, isSelf: m.id === groupMemberId(), streaming: m.streaming !== false })),
    relayUnreachable: (bcGroup.relayFails || 0) > 0,
  }
}
// Ein Heartbeat-Takt: eigenen Eintrag bei der Vermittlung auffrischen, die
// aktuelle Mitgliederliste zuruecknehmen. Abgelaufene Mitglieder raeumt die
// Vermittlung selbst (TTL) - keine Host-Rolle, kein Gossip, keine Nachfolge
// mehr noetig: der Raum lebt auf dem Webspace, nicht bei einer Person.
async function groupTick() {
  if (!bcGroup) return
  const r = await groupRelay('update', { code: bcGroup.code }, groupSelfEntry())
  if (r && r.ok) {
    bcGroup.relayFails = 0
    bcGroup.members = Array.isArray(r.members) ? r.members : []
    groupPushState()
    return
  }
  if (r && r.error === 'no-room') {
    // Raum existiert nicht mehr (alle waren zu lange offline -> TTL). Lokal sauber
    // beenden; der Code bleibt als lastCode vorbefuellt, ein Klick auf "Gruppe
    // starten" erzeugt einen frischen.
    osdDbg('[gruppe] groupTick: Raum ' + bcGroup.code + ' existiert nicht mehr -> Gruppe beendet')
    groupStopHeartbeat()
    bcGroup = null
    groupPushState()
    return
  }
  bcGroup.relayFails = (bcGroup.relayFails || 0) + 1
  if (bcGroup.relayFails === 1 || bcGroup.relayFails % 5 === 0) osdDbg('[gruppe] Vermittlung nicht erreichbar (' + bcGroup.relayFails + 'x): ' + groupRelayUrl())
  groupPushState()
}
function groupStartHeartbeat() {
  osdDbg('[gruppe] groupStartHeartbeat: Eintritt, bcGroupTimer=' + !!bcGroupTimer)
  if (!bcGroupTimer) bcGroupTimer = setInterval(() => { osdDbg('[gruppe] Heartbeat-Timer ausgeloest'); groupTick().catch((e) => osdDbg('[gruppe] groupTick warf: ' + (e && e.message))) }, GROUP_HEARTBEAT_MS)
  osdDbg('[gruppe] groupStartHeartbeat: fertig')
}
function groupStopHeartbeat() { if (bcGroupTimer) { clearInterval(bcGroupTimer); bcGroupTimer = null } }
// Aus Anwendersicht ist die Gruppe EIN Klick: "Gruppe starten" bzw. "Beitreten"
// zieht den eigenen Stream automatisch mit hoch, statt den Nutzer mit einem
// "erst Stream starten"-Toast in einen zweiten, unverstaendlichen Pflichtschritt
// zu schicken. Gewartet wird bis Phase 2 (Router/oeffentliche IPs) fertig ist,
// denn erst dann stehen linkV4/linkV6 - ohne die waere man im Roster adresslos
// und fuer die anderen unsichtbar.
async function groupEnsureBroadcast() {
  if (!broadcastState.active) {
    osdDbg('[gruppe] Auto-Start des Streams (fuer Gruppe)')
    await startBroadcast()
    if (!broadcastState.active) return broadcastState.noEncoder ? 'no-encoder' : 'stream-start-failed'
  }
  for (let i = 0; i < 40 && broadcastState.opening; i++) await new Promise((r) => setTimeout(r, 500))
  if (broadcastState.opening) osdDbg('[gruppe] Router-Pruefung dauert ungewoehnlich lange - fahre mit LAN-Kenntnisstand fort')
  return null
}
// Raumcode aus beliebiger Eingabe ziehen: roher 6er-Code ("TG7KP2") oder
// kompletter Einladungslink (...gruppe.php?code=TG7KP2), Gross/Klein egal.
function groupParseCode(raw) {
  const s = String(raw || '').trim().toUpperCase()
  const m = /[?&]CODE=([A-Z2-9]{6})\b/.exec(s) || /^([A-Z2-9]{6})$/.exec(s)
  return m ? m[1] : null
}
async function groupStart() {
  osdDbg('[gruppe] groupStart: Eintritt')
  if (bcGroup) return groupPublicState()
  const bcErr = await groupEnsureBroadcast()
  if (bcErr) { osdDbg('[gruppe] groupStart: Stream-Start fehlgeschlagen -> Abbruch'); return { active: false, error: bcErr } }
  const c = await groupRelay('create', {}, {})
  if (!c || !c.ok || !c.code) { osdDbg('[gruppe] groupStart: Vermittlung nicht erreichbar (' + groupRelayUrl() + ')'); return { active: false, error: 'relay-unreachable' } }
  bcGroup = { code: c.code, members: [], relayFails: 0 }
  const r = await groupRelay('update', { code: c.code }, groupSelfEntry())
  if (r && r.ok) bcGroup.members = Array.isArray(r.members) ? r.members : []
  appSettings.groupLastCode = c.code
  saveAppSettings()
  groupStartHeartbeat()
  groupPushState()
  osdDbg('[gruppe] groupStart: Raum ' + c.code + ' erstellt')
  return groupPublicState()
}
async function groupJoin(raw) {
  osdDbg('[gruppe] groupJoin: Eintritt, Eingabe=' + raw)
  if (bcGroup) return { active: false, error: 'already-in-group' }
  const code = groupParseCode(raw)
  if (!code) return { active: false, error: 'bad-code' }
  const bcErr = await groupEnsureBroadcast()
  if (bcErr) { osdDbg('[gruppe] groupJoin: Stream-Start fehlgeschlagen -> Abbruch'); return { active: false, error: bcErr } }
  const r = await groupRelay('update', { code }, groupSelfEntry())
  osdDbg('[gruppe] groupJoin: Antwort=' + JSON.stringify(r))
  if (!r) return { active: false, error: 'relay-unreachable' }
  if (!r.ok) return { active: false, error: r.error === 'no-room' ? 'no-room' : 'join-failed' }
  bcGroup = { code, members: Array.isArray(r.members) ? r.members : [], relayFails: 0 }
  appSettings.groupLastCode = code
  saveAppSettings()
  groupStartHeartbeat()
  groupPushState()
  osdDbg('[gruppe] groupJoin: Raum ' + code + ' beigetreten, Mitglieder=' + bcGroup.members.length)
  return groupPublicState()
}
async function groupLeave() {
  osdDbg('[gruppe] groupLeave: Eintritt, inGruppe=' + !!bcGroup)
  if (!bcGroup) return { active: false }
  const code = bcGroup.code
  groupStopHeartbeat()
  bcGroup = null
  // Abmeldung ABWARTEN, damit ein sofortiger Wiederbeitritt den Austritt nicht
  // ueberholen kann; scheitert sie, raeumt die TTL der Vermittlung den Eintrag.
  await groupRelay('leave', { code }, { id: groupMemberId() })
  groupPushState()
  // Lief der Server nur noch fuer die Gruppe (eigener Stream ist aus), jetzt
  // abbauen - ohne Gruppe und ohne Stream gibt es nichts mehr auszuliefern.
  if (!broadcastState.active && broadcastServer) bcTeardownServer()
  osdDbg('[gruppe] groupLeave: fertig')
  return { active: false }
}
// --- Zuschauer-Tracking: IP + Browser je WHEP-Session ----------------------
// mediamtx kennt die IP (webrtcsessions-API); den Browser (User-Agent) sieht
// nur unser Proxy. Beides zusammen ergibt die Zuschauer-Liste; Kick + IP-Sperre
// werfen ungebetene Gaeste raus.
const bcBlockedIps = new Set()   // gesperrte Zuschauer-IPs (Proxy weist POST /whep ab)
// mediamtx-ICE-Kandidat: "<typ>/<proto>/<ip>/<port>". Die IP ist der vorletzte
// Teil – so klappt es fuer IPv4 UND IPv6 (frueher nur IPv4-Regex -> IPv6-Zuschauer
// wurden dauerhaft als "verbindet…" angezeigt, obwohl laengst verbunden).
function bcExtractIp(s) {
  s = String(s || '')
  const parts = s.split('/')
  if (parts.length >= 4 && parts[parts.length - 2]) return parts[parts.length - 2]
  const m = /(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})/.exec(s)   // Fallback: nackte IPv4
  return m ? m[1] : ''
}
function bcClientIp(req) {
  let ip = (req.socket && req.socket.remoteAddress) || ''
  if (ip.startsWith('::ffff:')) ip = ip.slice(7)   // IPv4-mapped IPv6 -> nackte IPv4
  return ip
}
function bcUaShort(ua) {
  ua = String(ua || '')
  if (!ua) return ''
  const br = /Edg\//.test(ua) ? 'Edge' : /OPR\/|Opera/.test(ua) ? 'Opera' : /Firefox\//.test(ua) ? 'Firefox' : /Chrome\//.test(ua) ? 'Chrome' : /Safari\//.test(ua) ? 'Safari' : 'Browser'
  const os = /Android/.test(ua) ? 'Android' : /iPhone|iPad|iPod/.test(ua) ? 'iOS' : /Windows/.test(ua) ? 'Windows' : /Mac OS X/.test(ua) ? 'macOS' : /Linux/.test(ua) ? 'Linux' : ''
  return br + (os ? ' · ' + os : '')
}
// Die eigene lokale Stream-Vorschau (Renderer verbindet sich zu 127.0.0.1) darf
// NICHT als Zuschauer zaehlen – ihr ICE-Kandidat ist Loopback.
function bcIsLocalViewer(cand) { const c = String(cand || ''); return c.includes('127.0.0.1') || c.includes('::1') }
// Die eigene Live-Vorschau verbindet sich zwar lokal, aber ihre WebRTC-Medien
// laufen NICHT ueber Loopback (mediamtx bietet die konfigurierten Hosts/globalen
// IPs als ICE-Kandidaten an -> die Vorschau nimmt oft eine private IPv6-ULA);
// bcIsLocalViewer greift da nicht. Loesung: die Vorschau schickt einen eindeutigen
// User-Agent, den mediamtx je Session ausweist -> exakt identifizierbar (verifiziert:
// die WHEP-Location-ID ist NICHT die Session-ID der API, taugt also nicht dafuer).
// Explizit statt per IP-Heuristik, damit ein echter LAN-Zuschauer korrekt zaehlt.
const PREVIEW_UA = 'LumoraPreview'
function bcIsPreviewSession(s) { return s.userAgent === PREVIEW_UA || bcIsLocalViewer(s.remoteCandidate) }
function bcApiPost(apiPath) {
  return new Promise((resolve) => {
    const rq = http.request({ host: '127.0.0.1', port: MTX_API_PORT, path: apiPath, method: 'POST', timeout: 3000 }, (r) => { let d = ''; r.on('data', (c) => d += c); r.on('end', () => resolve({ status: r.statusCode, body: d })) })
    rq.on('error', () => resolve({ status: 0 })); rq.on('timeout', () => { rq.destroy(); resolve({ status: 0 }) })
    rq.end()
  })
}
// WHEP-Anfrage an mediamtx weiterreichen (dessen Endpoint ist /<path>/whep).
// Die Session-Sub-URL fuer PATCH/DELETE haengt mediamtx im Location-Header an;
// wir biegen sie auf unseren Proxy-Pfad um, damit der Zuschauer bei uns bleibt.
function bcProxyWhep(req, res) {
  const ip = bcClientIp(req)
  if (req.method === 'POST' && bcBlockedIps.has(ip)) { try { res.writeHead(403); res.end('blocked') } catch {}; return }   // gesperrt
  const chunks = []
  req.on('data', (c) => { chunks.push(c); if (Buffer.concat(chunks).length > 300000) req.destroy() })
  req.on('end', () => {
    const body = Buffer.concat(chunks)
    const rest = req.url.replace(/^\/whep/, '')          // '' oder '/<session>'
    const target = '/' + MTX_PATH + '/whep' + rest
    const headers = {}
    if (req.headers['content-type']) headers['Content-Type'] = req.headers['content-type']
    if (req.headers['user-agent']) headers['User-Agent'] = req.headers['user-agent']   // -> mediamtx-Session (Browser)
    headers['Content-Length'] = body.length
    const preq = http.request({ host: '127.0.0.1', port: MTX_WHEP_PORT, path: target, method: req.method, headers }, (pres) => {
      const h = {}
      let loc = null
      for (const k of Object.keys(pres.headers)) {
        if (k.toLowerCase() === 'location') {
          loc = String(pres.headers[k])
          h['location'] = loc.replace(/^https?:\/\/[^/]+/, '').replace('/' + MTX_PATH + '/whep', '/whep')
        } else h[k] = pres.headers[k]
      }
      res.writeHead(pres.statusCode, h)
      pres.pipe(res)
    })
    preq.on('error', () => { try { res.writeHead(502); res.end('mediamtx nicht erreichbar') } catch {} })
    preq.end(body)
  })
}
// Aktive Zuschauer: mediamtx-Sessions (IP, Dauer, Datenmenge) + unser User-Agent.
ipcMain.handle('list-viewers', async () => {
  try {
    const r = await upnpHttpGet('http://127.0.0.1:' + MTX_API_PORT + '/v3/webrtcsessions/list')
    const j = JSON.parse(r.body)
    const sessions = (Array.isArray(j.items) ? j.items : []).filter((s) => s.path === MTX_PATH && s.state === 'read' && !bcIsPreviewSession(s))
    return sessions.map((s) => {
      // IP + Browser direkt aus mediamtx: IP aus dem ICE-Kandidaten (remoteAddr
      // ist nur unser Proxy = 127.0.0.1), den User-Agent reichen wir durch. Solange
      // die WebRTC-Verbindung noch aushandelt, ist der Kandidat leer -> "verbindet…".
      const ip = bcExtractIp(s.remoteCandidate)
      return { id: s.id, ip: ip || '(verbindet…)', ua: bcUaShort(s.userAgent), since: Date.parse(s.created) || Date.now(), bytes: s.bytesSent || 0 }
    })
  } catch { return [] }
})
// Zuschauer trennen; block=true sperrt zusaetzlich die IP (kommt nicht sofort wieder).
ipcMain.handle('kick-viewer', async (e, id, ip, block) => {
  try {
    if (block && ip && ip !== '(verbindet…)') bcBlockedIps.add(ip)
    await bcApiPost('/v3/webrtcsessions/kick/' + id)
    return { ok: true }
  } catch { return { ok: false } }
})
function bcPushState() { sendToUi('broadcast-status', broadcastState) }
// mediamtx starten. Kurz warten und pruefen, dass der Prozess laeuft (mediamtx
// ist quasi sofort bereit); stirbt er sofort, ist meist ein Port belegt.
function bcStartMtx(configPath) {
  return new Promise((resolve, reject) => {
    let done = false, out = '', p
    try { p = spawn(streamBin('mediamtx.exe'), [configPath], { windowsHide: true }) }
    catch (e) { return reject(e) }
    const onData = (d) => { out = (out + d.toString()).slice(-2000); const t = d.toString().trim(); if (t) bcLogStream('mtx: ' + t) }   // bei logLevel error nur echte Fehler
    if (p.stdout) p.stdout.on('data', onData)
    if (p.stderr) p.stderr.on('data', onData)
    p.on('error', (e) => { if (!done) { done = true; reject(e) } })
    p.on('exit', (code) => {
      if (!done) { done = true; reject(new Error('mediamtx beendet (' + code + '): ' + out.slice(-200))); return }
      // mediamtx starb NACH erfolgreichem Start (Absturz/externer Kill): bei
      // aktivem Stream neu starten - FFmpeg verliert derweil nur kurz seine
      // RTSP-Verbindung und dockt nach seinem Auto-Neustart wieder an (Audit-
      // Befund: vorher drehte FFmpeg eine endlose Neustart-Schleife und die
      // UI zeigte den irrefuehrenden Vollbild-Hinweis).
      if (mtxProc === p) mtxProc = null
      if (broadcastState.active && !mtxProc) {
        bcLogStream('mtx: unerwartet beendet (' + code + ') -> Neustart')
        setTimeout(() => {
          if (!broadcastState.active || mtxProc) return
          bcStartMtx(configPath).then((np) => { mtxProc = np }).catch((e) => bcLogStream('mtx: Neustart fehlgeschlagen: ' + (e && e.message)))
        }, 800)
      }
    })
    // Aktiver Ready-Check statt blindem Warten: sobald der RTSP-Port annimmt, ist
    // mediamtx bereit (typisch ~150 ms). Das feste Timeout bleibt nur als Fallback –
    // jede gesparte Zehntelsekunde verkuerzt die Zeit bis zum ersten Vorschaubild.
    const probe = () => {
      if (done) return
      const s = require('net').connect({ host: '127.0.0.1', port: MTX_RTSP_PORT }, () => { s.destroy(); if (!done) { done = true; resolve(p) } })
      s.on('error', () => { s.destroy(); if (!done) setTimeout(probe, 60) })
    }
    setTimeout(probe, 60)
    setTimeout(() => { if (!done) { done = true; resolve(p) } }, 800)
  })
}
// Laufende Helfer (Bild + Ton) beenden.
function bcKillCap() {
  if (capProc) { const c = capProc; capProc = null; try { c.kill() } catch {} }
  if (audProc) { const a = audProc; audProc = null; try { a.kill() } catch {} }
}
// Streaming-Prozesse beim Scheduler bevorzugen. Log-Beleg (VSTAT im Helfer):
// maxwrite bis ~300 ms bei maxcopy 1-5 ms -> FFmpeg nimmt die Frame-Pipe
// periodisch nicht ab, waehrend Spiel/Browser Last machen. FFmpeg lief zwar
// schon auf HIGH, aber ohne Erfolgskontrolle (stiller catch) und die Helfer
// gar nicht erhoeht. Jetzt: ganze Kette angehoben und das Ergebnis geloggt -
// so weiss die naechste Stall-Analyse sicher, ob die Prioritaet greift.
function bcBoostPriority(proc, name, high) {
  if (!proc || !proc.pid) return
  try {
    os.setPriority(proc.pid, high ? os.constants.priority.PRIORITY_HIGH : os.constants.priority.PRIORITY_ABOVE_NORMAL)
    bcLogStream('prio ' + name + ' (' + proc.pid + '): ' + (high ? 'HIGH' : 'ABOVE_NORMAL'))
  } catch (e) { bcLogStream('prio ' + name + ': FEHLER ' + (e && e.message)) }
}
// Audio-Helfer starten: PCM (f32le 48k stereo) auf stdout, das FFmpeg als
// zweiten Eingang (fd 3) bekommt. Mit hwnd (Fenster-Modus): NUR der Ton des
// zugehoerigen Prozesses (WASAPI Process-Loopback) - sonst wuerde man z.B.
// Firefox-Ton im Stream hoeren, obwohl nur ein Spielfenster geteilt wurde.
// Ohne hwnd (Monitor-Modus): System-weites Loopback wie bisher, dort korrekt,
// weil ja der ganze Bildschirm geteilt wird.
function bcStartAudio(hwnd) {
  const args = ['--audio']
  if (hwnd) args.push('--hwnd', String(hwnd))
  let a
  try { a = spawn(streamBin('lumora-capture.exe'), args, { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] }) }
  catch (e) { bcLogStream('aud-start: ' + (e && e.message)); return null }
  audProc = a
  bcBoostPriority(a, 'audio')
  a.on('error', (e) => { bcLogStream('aud-error: ' + (e && e.message)) })
  a.on('exit', (code) => { if (audProc === a) audProc = null; bcLogStream('aud beendet (' + code + ')') })
  if (a.stderr) a.stderr.on('data', (d) => bcLogStream('aud: ' + d.toString().trim()))
  if (a.stdout) a.stdout.on('error', () => {})
  return a
}

// Aufnahme starten – je nach Quelle Monitor (ddagrab) oder Fenster (WGC-Helfer
// -> Pipe an FFmpeg). Bei Absturz automatischer Neustart, solange der Stream laeuft.
let ffFastFails = 0   // aufeinanderfolgende Sofort-Abstuerze
let ffGpuTimer = null // GPU-Telemetrie-Poll (Diagnose), laeuft nur waehrend FFmpeg lebt
let ffLagTimer = null // Event-Loop-Messung (Diagnose): haengt NODE, stockt die fd3-Audio-Pipe
// Zero-Copy-Selbstheilung (Audit-Befund Hybrid-Systeme): haengt der Monitor an
// der iGPU, kann NVENC die fremden D3D11-Frames nicht encodieren -> der Start
// scheitert deterministisch. Nach einem Sofort-Absturz im Zero-Copy-Modus
// (der NICHT nach exklusivem Vollbild aussieht) faellt die Session dauerhaft
// auf die adapter-tolerante CPU-Kette zurueck, statt endlos neu zu starten.
let bcZeroCopyBroken = false
let bcLastZeroCopy = false
function bcStartFfmpeg(cfg) {
  bcKillCap()   // evtl. verwaisten Helfer aufraeumen
  // Anzeige nachfuehren: die adaptive Bitrate aendert cfg.kbit bei Neustarts -
  // Qualitaetslabel + Hinweis-Badge in der UI sollen den echten Wert zeigen.
  broadcastState.quality = bcQualityLabel(cfg)
  broadcastState.adaptKbit = (bcAdaptLevel > 0 && appSettings.streamAdaptive !== false) ? cfg.kbit : 0
  bcPushState()
  if (cfg.mode === 'window') return bcStartWindowCapture(cfg)
  // Monitor-Weg (ddagrab): der Ton kommt vom parallelen Audio-Helfer ueber fd 3 -
  // hier bewusst OHNE hwnd, also System-weites Loopback (der ganze Bildschirm wird
  // geteilt, da gehoert auch der ganze System-Ton dazu). ddagrab liefert nur das
  // Bild, daher Audio separat.
  const aud = bcStartAudio()
  return bcSpawnEncoder(bcBuildFfmpegArgs(cfg, 0, 0, !!aud), null, null, aud)
}
// Fenster-Aufnahme: WGC-Helfer starten, dessen gemeldete Groesse (SIZE auf
// stderr) abwarten, dann FFmpeg mit rawvideo-Eingang starten und die Frames
// hineinpipen. Stirbt der Helfer (Fenster zu), bekommt FFmpeg EOF -> Neustart.
function bcStartWindowCapture(cfg) {
  // Video-Transport: Named Pipe mit grossem Puffer (Name pro Lauf eindeutig).
  // Die Standard-stdout-Pipe (64-KB-Puffer) war bei 4K-Rohframes der belegte
  // Engpass (13 statt 157 fps im Direktvergleich) - siehe Program.cs.
  cfg.vidPipe = 'lumora-vid-' + process.pid + '-' + Date.now()
  let cap
  try { cap = spawn(streamBin('lumora-capture.exe'), ['--hwnd', String(cfg.hwnd), '--fps', String(cfg.fps), '--max-height', String(cfg.scaleH || 0), '--pipe', cfg.vidPipe], { windowsHide: true }) }
  catch (e) { osdDbg('[stream] capture-Start fehlgeschlagen: ' + (e && e.message)); return null }
  capProc = cap
  bcBoostPriority(cap, 'capture')
  cap.on('error', (e) => { bcLogStream('cap-error: ' + (e && e.message)) })
  let sized = false, errbuf = '', capW = 0, capH = 0, rszTimer = null
  cap.on('exit', (code) => { if (rszTimer) { clearTimeout(rszTimer); rszTimer = null } if (capProc === cap) capProc = null; bcLogStream('cap beendet (' + code + ')') })
  cap.stderr.on('data', (d) => {
    const s = d.toString(); errbuf = (errbuf + s).slice(-1000); bcLogStream('cap: ' + s.trim())
    if (sized) {
      // Deutliche Fenster-Groessenaenderung (z.B. kleines Video -> Vollbild):
      // Der Helfer letterboxt in die START-Aufloesung - das Vollbild bliebe
      // dauerhaft unscharf (Audit-Befund). Bei >=50 % Flaechenaenderung, die
      // 3 s stabil bleibt, die Aufnahme einmal neu aufsetzen: der Neustart
      // misst die Groesse frisch und streamt wieder nativ.
      const rm = /RESIZE (\d+) (\d+) ->/.exec(s)
      if (rm && capW && capH) {
        const ratio = (parseInt(rm[1], 10) * parseInt(rm[2], 10)) / (capW * capH)
        if (ratio >= 1.5 || ratio <= 0.67) {
          if (rszTimer) clearTimeout(rszTimer)
          rszTimer = setTimeout(() => {
            rszTimer = null
            if (capProc !== cap || !broadcastState.active || ffStopping) return
            bcLogStream('resize: Fenster deutlich veraendert (Faktor ' + ratio.toFixed(2) + ') -> Aufnahme passt sich an')
            bcRestartFfmpeg()
          }, 3000)
        }
      }
      return
    }
    const m = /SIZE (\d+) (\d+)/.exec(errbuf)
    if (!m) return
    sized = true
    capW = parseInt(m[1], 10); capH = parseInt(m[2], 10)
    if (capProc !== cap || !broadcastState.active || ffStopping) return
    // Das Video laeuft ueber die Named Pipe des Helfers (siehe oben) - FFmpeg
    // oeffnet sie selbst per Dateiname. Historie der Transportwege: Node-Piping
    // ~8 fps, stdout-OS-Pipe (64-KB-Puffer) ~58 fps, Named Pipe mit 64-MB-
    // Puffer haelt 60 fps auch bei 4K-Rohframes.
    if (cap.stdout) cap.stdout.on('error', () => {})
    const aud = bcStartAudio(cfg.hwnd)                              // Ton NUR des aufgenommenen Fensters/Prozesses
    bcSpawnEncoder(bcBuildFfmpegArgs(cfg, capW, capH, !!aud), cap, null, aud)
  })
  return cap
}
// FFmpeg starten (gemeinsam fuer beide Quellen) inkl. Exit-/Neustart-Logik.
// capOfThis = zugehoeriger Helfer (Fenster-Modus); wird beim Beenden mitgekillt.
function bcSpawnEncoder(args, capOfThis, stdinStream, audOfThis) {
  if (!args) return null
  osdDbg('[stream] ffmpeg ' + args.join(' '))
  let p
  // stdinStream gesetzt (Fenster-Aufnahme) -> dessen fd wird FFmpegs stdin:
  // die Rohframes fliessen OS-direkt vom Helfer, ohne Node-Overhead.
  const stdio = stdinStream ? [stdinStream, 'pipe', 'pipe'] : ['pipe', 'pipe', 'pipe']
  if (audOfThis && audOfThis.stdout) stdio[3] = 'pipe'   // fd 3 = Audio (Node-Pipe, s. u.)
  try { p = spawn(streamBin('ffmpeg.exe'), args, { windowsHide: true, stdio }) }
  catch (e) { osdDbg('[stream] ffmpeg-Start fehlgeschlagen: ' + (e && e.message)); return null }
  ffProc = p
  // Audio-PCM in FFmpegs fd 3 leiten – als Node-Pipe. Audio ist klein (~384 KB/s),
  // daher unkritisch; die DIREKTE fd-Weitergabe zwischen zwei Kindprozessen
  // liefert dagegen kein Audio (verifiziert). Video bleibt OS-direkt (fd 0).
  if (audOfThis && audOfThis.stdout && p.stdio && p.stdio[3]) {
    try { p.stdio[3].on('error', () => {}); audOfThis.stdout.pipe(p.stdio[3]) } catch {}
  }
  const startedAt = Date.now()
  bcBoostPriority(p, 'ffmpeg', true)
  // GPU-Telemetrie (Diagnose der maxwrite-Stalls): Takt + Encoder-Last alle 10s
  // neben die VSTAT-Zeilen legen. These: bei ruhigem Bild/ohne Zuschauer senkt
  // die GPU ihre Takte (Idle-Clocks, gemessen: SM 780 statt ~2600 MHz) -> der
  // NVENC-Encode dauert x-fach laenger -> FFmpeg nimmt die Pipe 200-300ms nicht
  // ab. Faellt der Video-Takt exakt am Kipp-Punkt, ist die Ursache bewiesen.
  // NVIDIA-only und fehlertolerant: ohne nvidia-smi beendet sich der Timer.
  if (!ffGpuTimer) {
    ffGpuTimer = setInterval(() => {
      // Selbstheilung: beim Stop-Pfad wird ffProc genullt BEVOR der Prozess
      // stirbt - der exit-Handler stoppt den Timer dann nicht (ffProc !== p).
      if (!ffProc) { clearInterval(ffGpuTimer); ffGpuTimer = null; return }
      exec('nvidia-smi --query-gpu=clocks.sm,clocks.video,utilization.gpu,utilization.encoder,power.draw --format=csv,noheader,nounits', { windowsHide: true, timeout: 5000 }, (err, out) => {
        if (err) { if (ffGpuTimer) { clearInterval(ffGpuTimer); ffGpuTimer = null } bcLogStream('gpu: Abfrage nicht moeglich (' + String(err.message || '').slice(0, 80) + ')'); return }
        const v = String(out).trim().split(/,\s*/)
        if (v.length >= 5) bcLogStream('gpu: sm=' + v[0] + 'MHz video=' + v[1] + 'MHz util=' + v[2] + '% enc=' + v[3] + '% pwr=' + v[4] + 'W')
      })
    }, 10000)
  }
  // Event-Loop-Verzoegerung im Node-Hauptprozess messen (Diagnose): das Audio
  // laeuft als Node-Pipe durch diesen Prozess (fd 3) - haengt der Event-Loop
  // (IPC/HTTP/Settings), koennte das die WSTALL-Ereignisse im Helfer erklaeren.
  // Nur Ausreisser (>100 ms Drift eines 1s-Timers) landen im Log.
  if (!ffLagTimer) {
    let lagT = Date.now()
    ffLagTimer = setInterval(() => {
      if (!ffProc) { clearInterval(ffLagTimer); ffLagTimer = null; return }
      const d = Date.now() - lagT - 1000
      lagT = Date.now()
      if (d > 100) bcLogStream('node-lag: ' + d + 'ms')
    }, 1000)
  }
  // Adaptions-Tick (adaptive Bitrate): ueberlebt FFmpeg-Neustarts (active bleibt
  // true), raeumt sich nach Stream-Ende selbst weg.
  if (!bcAdaptTimer) {
    bcAdaptTimer = setInterval(() => {
      if (!ffProc && !broadcastState.active) { clearInterval(bcAdaptTimer); bcAdaptTimer = null; return }
      try { bcAdaptTick() } catch {}
    }, 5000)
  }
  // stderr zeilenweise verarbeiten: die key=value-Bloecke von -progress werden
  // eingesammelt und alle 5s als EINE ff-stat-Zeile geloggt (sonst Log-Flut);
  // alles andere (Warnungen/Fehler) laeuft wie bisher als 'ff:' ins Log und in
  // errbuf (NVENC-Treiber-Erkennung beim Exit).
  let errbuf = '', ffLineBuf = ''
  const ffProg = {}
  if (p.stderr) p.stderr.on('data', (d) => {
    ffLineBuf += d.toString()
    let nl
    while ((nl = ffLineBuf.indexOf('\n')) >= 0) {
      const line = ffLineBuf.slice(0, nl).trim(); ffLineBuf = ffLineBuf.slice(nl + 1)
      if (!line) continue
      const m = /^(frame|fps|stream_\d+_\d+_q|bitrate|total_size|out_time\w*|dup_frames|drop_frames|speed|progress)=(.*)$/.exec(line)
      if (m) {
        if (m[1] === 'progress') bcLogStream('ff-stat: frame=' + ffProg.frame + ' fps=' + ffProg.fps + ' speed=' + ffProg.speed + ' dup=' + ffProg.dup_frames + ' drop=' + ffProg.drop_frames)
        else ffProg[m[1]] = m[2]
        continue
      }
      errbuf = (errbuf + line + '\n').slice(-2000)
      bcLogStream('ff: ' + line)
    }
  })
  // WICHTIG: 'error' MUSS behandelt werden – sonst wird ein Prozessfehler zur
  // uncaught exception und beendet die ganze App (genau der beobachtete Absturz).
  p.on('error', (e) => { bcLogStream('ff-error: ' + (e && e.message)) })
  p.on('exit', (code) => {
    if (ffProc === p) {
      ffProc = null
      if (ffGpuTimer) { clearInterval(ffGpuTimer); ffGpuTimer = null }
      if (ffLagTimer) { clearInterval(ffLagTimer); ffLagTimer = null }
    }
    // Zugehoerige Helfer (Bild + Ton) mit beenden (sonst schreiben sie in tote Pipes).
    if (capOfThis) { if (capProc === capOfThis) capProc = null; try { capOfThis.kill() } catch {} }
    if (audOfThis) { if (audProc === audOfThis) audProc = null; try { audOfThis.kill() } catch {} }
    // _intentional: von bcRestartFfmpeg gewollt beendet -> KEIN eigener Neustart
    // (sonst konkurriert er mit dem geplanten neuen Prozess -> Doppel-Publisher).
    if (p._intentional || ffStopping || !broadcastState.active) return
    // Sofort-Absturz (typisch: Spiel im EXKLUSIVEN Vollbild -> ddagrab verliert
    // den Zugriff, DXGI_ERROR_ACCESS_LOST). Nach mehreren schnellen Abstuerzen
    // langsamer neu versuchen und die UI warnen, statt im Sekundentakt zu spammen.
    if (Date.now() - startedAt < 4000) ffFastFails++; else ffFastFails = 0
    // Zero-Copy-Selbstheilung (s. Deklaration bei bcZeroCopyBroken): Sofort-
    // Absturz im GPU-Direktmodus OHNE Vollbild-Signatur (ACCESS_LOST waere die
    // ddagrab-Meldung fuer exklusives Vollbild) -> ab jetzt CPU-Kette.
    if (Date.now() - startedAt < 4000 && bcLastZeroCopy && !bcZeroCopyBroken && !/ACCESS_LOST|887a0026/i.test(errbuf)) {
      bcZeroCopyBroken = true
      bcLogStream('video: GPU-direkt fehlgeschlagen -> Rueckfall auf CPU-Kette (Hybrid-GPU?): ' + errbuf.replace(/\s+/g, ' ').slice(-160))
    }
    const delay = ffFastFails >= 3 ? 5000 : 1200
    // NVENC-Treiber zu alt (deterministisch – jeder Neuversuch scheitert gleich):
    // sofort klaren Hinweis. Der aktuelle FFmpeg-Build braucht NVIDIA-Treiber >= 610.
    if (/required nvenc API version|minimum required Nvidia driver/i.test(errbuf) && !broadcastState.driverError) {
      broadcastState.driverError = true; bcPushState()
    }
    if (ffFastFails === 3 && !broadcastState.captureError) { broadcastState.captureError = true; bcPushState() }
    osdDbg('[stream] ffmpeg beendet (' + code + '), Neustart in ' + delay + 'ms (fails=' + ffFastFails + '): ' + errbuf.slice(-160))
    ffRestartTimer = setTimeout(() => { if (broadcastState.active && !ffStopping) bcStartFfmpeg(bcStreamCfg(bcEncoderCache || { encoder: 'h264_nvenc' })) }, delay)
  })
  // Laeuft der Prozess laenger stabil, Fehlerzaehler + Warnung zuruecksetzen.
  setTimeout(() => { if (ffProc === p) { ffFastFails = 0; if (broadcastState.captureError || broadcastState.driverError) { broadcastState.captureError = false; broadcastState.driverError = false; bcPushState() } } }, 6000)
  return p
}
// FFmpeg gezielt neu starten (nach Einstellungsaenderung). mediamtx + WHEP-
// Server laufen durch; Zuschauer verbinden nach dem kurzen Aussetzer weiter.
// Debounce: mehrere schnelle Aenderungen (Bitrate + Aufloesung kurz nacheinander)
// zu EINEM Neustart zusammenfassen.
function bcRestartFfmpeg() {
  if (ffRestartDebounce) clearTimeout(ffRestartDebounce)
  ffRestartDebounce = setTimeout(bcDoRestartFfmpeg, 350)
}
function bcDoRestartFfmpeg() {
  ffRestartDebounce = null
  if (!broadcastState.active || ffStopping) return
  if (ffRestartTimer) { clearTimeout(ffRestartTimer); ffRestartTimer = null }
  const cfg = bcStreamCfg(bcEncoderCache || { encoder: 'h264_nvenc' })
  broadcastState.quality = bcQualityLabel(cfg)
  const old = ffProc
  ffProc = null
  let started = false
  // Erst wenn das ALTE FFmpeg wirklich beendet ist, das neue starten – sonst
  // senden zwei Prozesse gleichzeitig auf denselben mediamtx-Pfad (der beobachtete
  // "End of file / Broken pipe"-Konflikt, der den Stream aufhaengt).
  const startNew = () => { if (started) return; started = true; if (broadcastState.active && !ffStopping) bcStartFfmpeg(cfg) }
  if (old) {
    old._intentional = true          // sein exit-Handler darf NICHT selbst neu starten
    old.once('exit', startNew)
    try { old.kill() } catch {}
    setTimeout(startNew, 2500)        // Sicherheitsnetz, falls kein exit-Event kommt
  } else {
    startNew()
  }
}
// Zuschauerzahl von der mediamtx-API pollen (Anzahl WHEP-Reader auf dem Pfad).
function bcStartViewerPoll() {
  if (bcViewerPoll) return
  bcViewerPoll = setInterval(() => {
    if (!broadcastState.active) return
    // webrtcsessions/list statt paths/get: liefert die ICE-Kandidaten, damit die
    // eigene lokale Vorschau (127.0.0.1) nicht als Zuschauer mitgezaehlt wird.
    upnpHttpGet('http://127.0.0.1:' + MTX_API_PORT + '/v3/webrtcsessions/list').then((r) => {
      let n = 0
      try { const j = JSON.parse(r.body); n = (Array.isArray(j.items) ? j.items : []).filter((s) => s.path === MTX_PATH && s.state === 'read' && !bcIsPreviewSession(s)).length } catch { return }
      if (n !== broadcastState.viewers) {
        const prev = broadcastState.viewers
        broadcastState.viewers = n
        // Immer-an-Log (lumora-stream.log): Bei einem harten PC-Absturz zeigt die
        // LETZTE Zeile, ob der Crash unmittelbar auf einen Zuschauer-Wechsel folgte
        // (Verdachtsfall AMD-System: Absturz, als der 2. Zuschauer den Player oeffnete).
        bcLogStream('viewer: ' + prev + ' -> ' + n)
        bcPushState()
        if (n > prev) sendToUi('viewer-joined', n)
      }
    }).catch(() => {})
  }, 2000)
}
async function startBroadcast() {
  if (broadcastState.active) return broadcastState
  ffStopping = false
  const enc = await bcDetectEncoder()
  if (!enc.encoder) {
    // Kein Hardware-Video-Encoder (NVENC/AMF/QSV) verfuegbar. Der LGPL-FFmpeg-Build
    // hat bewusst keinen Software-Encoder (libx264 = GPL) -> hier kein Streaming.
    // Klarer Hinweis statt einer FFmpeg-"Unknown encoder"-Fehlerschleife.
    broadcastState = { active: false, noEncoder: true }
    bcPushState()
    osdDbg('[stream] Abbruch: kein Hardware-Encoder (NVENC/AMF/QSV) gefunden.')
    return broadcastState
  }
  const lanIp = bcLanIp()
  const lanLink = 'http://' + lanIp + ':' + BROADCAST_PORT + '/'
  // Lief der Server durch (Gruppe aktiv, Stream war nur pausiert), sind die
  // oeffentlichen Adressen weiterhin gueltig - uebernehmen statt zu leeren, sonst
  // waere man im Gruppen-Roster kurzzeitig adresslos, bis Phase 2 durch ist.
  const prevV4 = broadcastServer ? (broadcastState.linkV4 || '') : ''
  const prevV6 = broadcastServer ? (broadcastState.linkV6 || '') : ''
  // Adaptive Bitrate: jeder Stream beginnt mit der vollen eingestellten Bitrate.
  bcAdaptLevel = 0; bcAdaptLastChange = 0; bcAdaptBadSince = 0; bcAdaptGoodSince = 0
  bcAdaptUpAt = 0; bcAdaptUpHold = 0
  bcQosMap.clear(); bcQosLogLast.clear()
  broadcastState = { active: true, port: BROADCAST_PORT, link: lanLink, linkV4: prevV4, linkV6: prevV6, lanLink, viewers: 0, quality: '', internet: false, opening: true, encoder: enc.encoder }
  bcPushState()   // LAN-Link sofort anzeigen
  bcPinholeIds = []
  // Test-Schalter: IPv4-Weg bewusst auslassen, damit sich der IPv6-Direktweg
  // isoliert pruefen laesst (Link + Medien laufen dann garantiert ueber IPv6).
  const forceV6 = !!appSettings.streamForceIPv6
  // PHASE 1 – LOKAL, SOFORT: mediamtx + WHEP-Server + FFmpeg hochziehen, damit das
  // erste Vorschaubild ohne Router-Wartezeit kommt. Als ICE-Hosts nimmt mediamtx
  // die GECACHTEN oeffentlichen IPs der letzten Session (aendern sich selten);
  // Phase 2 korrigiert sie bei Abweichung (mediamtx laedt seine Config selbst neu).
  const cachedHosts = Array.isArray(appSettings.streamLastHosts) ? appSettings.streamLastHosts.filter(Boolean) : []
  try {
    const cfgPath = bcWriteMtxConfig(cachedHosts)
    mtxProc = await bcStartMtx(cfgPath)
    // Server kann vom Gruppen-Modus noch laufen (Stream war nur pausiert) ->
    // NICHT neu binden, das gaebe EADDRINUSE und risse den Start ab.
    if (!broadcastServer) broadcastServer = await bcStartServer(BROADCAST_PORT)
  } catch (e) {
    osdDbg('[stream] Start fehlgeschlagen: ' + (e && e.message))
    bcLogStream('start: FEHLGESCHLAGEN: ' + (e && e.message))
    // Dem Nutzer den Grund ZEIGEN (Audit-Befund: Portkonflikt/mediamtx-Fehler
    // blieben komplett stumm - Knopf sprang einfach auf "aus" zurueck).
    const msg = /EADDRINUSE/i.test(String(e && e.message))
      ? 'Stream-Start fehlgeschlagen: Port wird von einem anderen Programm belegt. Läuft Lumora doppelt oder nutzt eine andere App Port 8787/8554?'
      : 'Stream-Start fehlgeschlagen: ' + ((e && e.message) || 'unbekannter Fehler').slice(0, 140)
    sendToUi('stream-error', msg)
    stopBroadcast()
    return broadcastState
  }
  const cfg = bcStreamCfg(enc)
  bcStartFfmpeg(cfg)
  // HDR-Farb-Hinweis (Audit): der Monitor-Weg (ddagrab) hat kein HDR-Tone-
  // mapping - laeuft der gewaehlte Bildschirm in HDR, wirken die Farben beim
  // Zuschauer blass. Einmal pro Stream-Start pruefen und in der UI erklaeren
  // (der Fenster-Weg wandelt HDR sauber, dorthin verweist der Hinweis).
  if (cfg.mode === 'monitor') {
    require('child_process').execFile(streamBin('lumora-capture.exe'), ['--hdr-check'], { windowsHide: true, timeout: 8000 }, (err, out) => {
      if (err || !broadcastState.active) return
      if (new RegExp('^HDR ' + cfg.outputIdx + ' 1', 'm').test(String(out))) {
        broadcastState.hdrMonitor = true
        bcLogStream('hdr-check: Monitor ' + cfg.outputIdx + ' laeuft in HDR -> Farb-Hinweis aktiv')
        bcPushState()
      }
    })
  }
  bcStartViewerPoll()
  broadcastState.quality = bcQualityLabel(cfg)
  bcPushState()   // Medien laufen – Vorschau kann verbinden (opening bleibt true bis der Router geprueft ist)
  if (bcGroup) { groupPushState(); groupTick().catch(() => {}) }   // "streamt wieder" sofort an die Gruppe verteilen
  // PHASE 2 – ROUTER, PARALLEL: Portfreigaben + oeffentliche IPs (dauert je nach
  // Router Sekunden). Erst danach steht der oeffentliche Link fest -> opening=false.
  ;(async () => {
    const routerT0 = Date.now()   // Dauer der Router-Phase belegen (Startwartezeit-Optimierung)
    let pubIp = null, tcpOk = false, udpOk = false
    let pubIp6 = null, v6Ok = false
    let v4OccupiedBy = null      // andere Lumora-Instanz haelt bereits IPv4:Port (zweiter PC im selben Netz)
    let v4OccupiedGroup = null   // deren laufender Raumcode, falls vorhanden (fuer den Auto-Join)
    try {
      // ERST die oeffentlichen IPs ermitteln, DANN mappen: vor dem Beanspruchen des
      // IPv4-Ports pruefen, ob dort schon eine ANDERE Lumora-Instanz antwortet
      // (zweiter PC hinter demselben Router). Ohne diese Pruefung wuerde je nach
      // Router das bestehende Mapping ueberschrieben - und der bereits laufende
      // Stream des anderen PCs risse mitten im Spiel ab.
      const ips = await Promise.all([upnpGetExternalIp(), bcPublicIPv6()])
      pubIp = ips[0]; pubIp6 = ips[1]
      // IPv6-Pinholes PARALLEL zum IPv4-Zweig oeffnen (beide sind voneinander
      // unabhaengig; frueher lief das sequentiell und addierte sich auf die
      // Startwartezeit). Ergebnis wird unten vor der Link-Wahl abgewartet.
      const v6Job = (async () => {
        if (!pubIp6) return
        const [t6, u6] = await Promise.all([
          upnpAddPinhole(pubIp6, BROADCAST_PORT, 'TCP'),
          upnpAddPinhole(pubIp6, MTX_ICE_UDP, 'UDP'),
        ])
        if (t6) bcPinholeIds.push(t6)
        if (u6) bcPinholeIds.push(u6)
        v6Ok = !!(t6 && u6)
        if (v6Ok) bcPinholeSetAt = Date.now()   // fuer die 12-h-Erneuerung im IP-Watcher
      })()
      if (!forceV6 && pubIp) {
        // Kurzer Timeout (statt der 4s-Voreinstellung): eine ANDERE Instanz im
        // selben Netz antwortet ueber Router-Hairpin in <500 ms. Gibt es keine
        // (Normalfall), versandet das Paket mangels Portfreigabe kommentarlos -
        // und dieser Timeout war der groesste Einzelposten der Startwartezeit.
        const inst = await groupHttpJson('http://' + pubIp + ':' + BROADCAST_PORT + '/', '/instanz', 'GET', null, 1500)
        if (inst && inst.lumora && inst.id && inst.id !== groupMemberId()) {
          v4OccupiedBy = inst.id
          v4OccupiedGroup = inst.group || null   // Raumcode der anderen Instanz (fuer Auto-Join)
        }
      }
      if (v4OccupiedBy) {
        osdDbg('[stream] IPv4:' + BROADCAST_PORT + ' gehoert bereits einer anderen Lumora-Instanz (' + v4OccupiedBy + ') im selben Netz -> IPv4-Weg wird NICHT beansprucht, weiche auf IPv6 aus')
      } else {
        const r = await Promise.all([
          forceV6 ? Promise.resolve(false) : upnpMapPort(BROADCAST_PORT, 'TCP', 'Lumora Stream'),
          forceV6 ? Promise.resolve(false) : upnpMapPort(MTX_ICE_UDP, 'UDP', 'Lumora Stream Medien'),
        ])
        tcpOk = r[0]; udpOk = r[1]
        if (!forceV6) bcV4Mapped = true   // nur dann darf der Teardown spaeter auch unmappen
      }
      // IPv6-Pinholes (oben parallel gestartet) fertig abwarten.
      await v6Job
    } catch {}
    if (!broadcastState.active) {
      // Waehrend der Router-Phase gestoppt: frisch gesetzte Pinholes wieder schliessen.
      const ids = bcPinholeIds; bcPinholeIds = []
      for (const id of ids) { try { upnpDeletePinhole(id) } catch {} }
      return
    }
    // Weichen die frischen oeffentlichen IPs vom Cache ab, Config aktualisieren –
    // mediamtx uebernimmt sie per Hot-Reload (nur der WebRTC-Teil startet kurz neu;
    // die Vorschau verbindet sich dank Auto-Reconnect von selbst wieder).
    const hosts = (forceV6 ? [pubIp6] : [pubIp, pubIp6]).filter(Boolean)
    if (JSON.stringify(hosts) !== JSON.stringify(cachedHosts)) {
      try { bcWriteMtxConfig(hosts) } catch {}
      appSettings.streamLastHosts = hosts
      saveAppSettings()
    }
    bcLogStream('router-phase: ' + (Date.now() - routerT0) + ' ms (v4=' + (tcpOk && udpOk ? 'ok' : 'nein') + ' v6=' + (v6Ok ? 'ok' : 'nein') + ')')
    broadcastState.opening = false
    // Link-Prioritaet: klappt die IPv4-Freigabe -> IPv4-Link (breiteste Reichweite,
    // jeder Zuschauer erreicht ihn). Sonst, wenn der IPv6-Pinhole sitzt -> IPv6-Link
    // ([v6]:port) fuer DS-Lite/CGNAT (Zuschauer braucht IPv6). Sonst IPv4 anzeigen.
    const v4Reachable = tcpOk && udpOk && pubIp
    if (v4Reachable) broadcastState.link = 'http://' + pubIp + ':' + BROADCAST_PORT + '/'
    else if (v6Ok && pubIp6) broadcastState.link = 'http://[' + pubIp6 + ']:' + BROADCAST_PORT + '/'
    else if (pubIp) broadcastState.link = 'http://' + pubIp + ':' + BROADCAST_PORT + '/'
    // Beide Adressfamilien getrennt merken (fuer den Gruppen-Modus): jedes Mitglied
    // kennt so von jedem anderen sowohl IPv4- als auch IPv6-Weg, falls vorhanden -
    // Zuschauer/Beitretende koennen dann selbst waehlen, was bei ihnen klappt.
    broadcastState.linkV4 = v4Reachable ? ('http://' + pubIp + ':' + BROADCAST_PORT + '/') : ''
    broadcastState.linkV6 = (v6Ok && pubIp6) ? ('http://[' + pubIp6 + ']:' + BROADCAST_PORT + '/') : ''
    broadcastState.internet = !!(v4Reachable || (v6Ok && pubIp6))
    broadcastState.ipv6Only = !!(!v4Reachable && v6Ok && pubIp6)   // Link laeuft ueber IPv6 (Zuschauer braucht IPv6)
    broadcastState.needsForward = !!(pubIp && !v4Reachable && !(v6Ok && pubIp6))
    bcPushState()
    osdDbg('[stream] aktiv: ' + broadcastState.link + ' enc=' + enc.encoder + ' tcp=' + tcpOk + ' udp=' + udpOk + ' ip=' + pubIp + ' v6=' + v6Ok + ' ip6=' + pubIp6 + (v4OccupiedBy ? ' (IPv4 belegt von ' + v4OccupiedBy + ')' : ''))
    // IP-Wechsel-Watcher (s. bcIpWatchTick): raeumt sich nach Stream-Ende selbst weg.
    if (!bcIpWatchTimer) {
      bcIpWatchTimer = setInterval(() => {
        if (!broadcastState.active) { clearInterval(bcIpWatchTimer); bcIpWatchTimer = null; return }
        bcIpWatchTick().catch(() => {})
      }, 300000)
    }
    // Zweiter PC im selben Netz: laeuft auf der anderen Lumora-Instanz bereits eine
    // Gruppe, automatisch per Raumcode beitreten - der Nutzer startet auf Rechner 2
    // nur den Stream und ist dabei, ohne Code-Kopieren zwischen den eigenen Geraeten.
    if (v4OccupiedGroup && !bcGroup) {
      try {
        osdDbg('[gruppe] Auto-Join: Instanz von ' + v4OccupiedBy + ' hat Raum ' + v4OccupiedGroup)
        const st = await groupJoin(v4OccupiedGroup)
        if (st && st.active) sendToUi('group-autojoin', {})
        else osdDbg('[gruppe] Auto-Join fehlgeschlagen: ' + JSON.stringify(st))
      } catch (e) { osdDbg('[gruppe] Auto-Join warf: ' + (e && e.message)) }
    }
  })()
  return broadcastState
}
// --- IP-Wechsel-Watcher (Audit-Befund: DSL-Zwangstrennung mitten im Stream) ----
// Alle 5 min die oeffentlichen Adressen pruefen (UPnP-Control ist gecacht ->
// billig, kein Discovery-Lauf). Aendern sie sich, werden mediamtx-Hosts (per
// Hot-Reload), Portfreigabe, IPv6-Pinholes, Anzeige-Links und das Gruppen-
// Roster nachgezogen - vorher blieb der Stream nach einer Zwangstrennung still
// unerreichbar, waehrend die UI weiter gruen zeigte. Nebenbei werden die
// IPv6-Pinholes (24-h-Lease) nach 12 h erneuert (Marathon-Streams/Dauergruppe).
let bcIpWatchTimer = null
let bcPinholeSetAt = 0
async function bcIpWatchTick() {
  if (!broadcastState.active || broadcastState.opening) return
  let ip = null, ip6 = null
  try { ip = await upnpGetExternalIp() } catch {}
  if (!ip) upnpCtrl = null   // Router neu gestartet? Control-Cache verwerfen -> naechster Tick discovert frisch
  try { ip6 = await bcPublicIPv6() } catch {}
  const forceV6 = !!appSettings.streamForceIPv6
  const hosts = (forceV6 ? [ip6] : [ip, ip6]).filter(Boolean)
  if (!hosts.length) return   // gerade gar kein Netz: nichts anfassen, naechster Tick prueft wieder
  const cur = Array.isArray(appSettings.streamLastHosts) ? appSettings.streamLastHosts.filter(Boolean) : []
  const changed = JSON.stringify(hosts) !== JSON.stringify(cur)
  const renewPinholes = !!ip6 && bcPinholeSetAt > 0 && Date.now() - bcPinholeSetAt > 12 * 3600 * 1000
  if (!changed && !renewPinholes) return
  if (changed) {
    bcLogStream('ipwatch: oeffentliche Adresse geaendert ' + JSON.stringify(cur) + ' -> ' + JSON.stringify(hosts))
    try { bcWriteMtxConfig(hosts) } catch {}   // mediamtx uebernimmt per Hot-Reload
    appSettings.streamLastHosts = hosts
    saveAppSettings()
    if (!forceV6 && ip) { try { await Promise.all([upnpMapPort(BROADCAST_PORT, 'TCP', 'Lumora Stream'), upnpMapPort(MTX_ICE_UDP, 'UDP', 'Lumora Stream Medien')]) } catch {} }
    if (ip) broadcastState.linkV4 = 'http://' + ip + ':' + BROADCAST_PORT + '/'
    if (ip6) broadcastState.linkV6 = 'http://[' + ip6 + ']:' + BROADCAST_PORT + '/'
    broadcastState.link = broadcastState.linkV4 || broadcastState.linkV6 || broadcastState.link
    bcPushState()
    if (bcGroup) { groupPushState(); groupTick().catch(() => {}) }   // neue Adressen ins Gruppen-Roster
  }
  if (ip6 && (changed || renewPinholes)) {
    const ids = bcPinholeIds; bcPinholeIds = []
    for (const id of ids) { try { upnpDeletePinhole(id) } catch {} }
    try {
      const [t6, u6] = await Promise.all([upnpAddPinhole(ip6, BROADCAST_PORT, 'TCP'), upnpAddPinhole(ip6, MTX_ICE_UDP, 'UDP')])
      if (t6) bcPinholeIds.push(t6)
      if (u6) bcPinholeIds.push(u6)
      if (t6 && u6) bcPinholeSetAt = Date.now()
      bcLogStream('ipwatch: IPv6-Pinholes ' + (changed ? 'neu gesetzt' : 'erneuert') + ' (' + bcPinholeIds.length + ')')
    } catch {}
  }
}
// Server + Freigaben komplett abbauen - der Teil des Stopps, der bei aktiver
// Gruppe NICHT laufen darf (siehe stopBroadcast): der HTTP-Server ist die
// Gruppen-Infrastruktur (Roster, Gossip, Grid) und muss die Mitgliedschaft
// ueberleben, sonst haengt die ganze Gruppe am Streaming-Zustand einer Person.
function bcTeardownServer() {
  if (broadcastServer) { try { broadcastServer.close() } catch {}; broadcastServer = null }
  if (bcViewerPoll) { clearInterval(bcViewerPoll); bcViewerPoll = null }
  bcBlockedIps.clear()
  // IPv4-Freigaben NUR schliessen, wenn wir sie selbst gesetzt haben: gehoert das
  // Mapping einer anderen Lumora-Instanz (zweiter PC im selben Netz), wuerde ein
  // blindes Unmap deren laufenden Stream von aussen kappen.
  if (bcV4Mapped) { upnpUnmapPort(BROADCAST_PORT, 'TCP'); upnpUnmapPort(MTX_ICE_UDP, 'UDP'); bcV4Mapped = false }
  bcPinholeIds.forEach((id) => upnpDeletePinhole(id)); bcPinholeIds = []       // IPv6-Firewall-Pinholes schliessen
  broadcastState = { active: false, port: 0, link: '', linkV4: '', linkV6: '', lanLink: '', viewers: 0, quality: '', internet: false, opening: false }
  bcPushState()
}
// fullTeardown=true nur beim App-Ende: dann wird auch eine aktive Gruppe verlassen
// und der Server abgebaut. Der normale "Stream stoppen"-Weg (Knopf/Hotkey) beendet
// dagegen NUR die Videopipeline: Wer in einer Gruppe ist, BLEIBT Mitglied (und
// Host bleibt Host!) - er gilt fuer die anderen lediglich als "pausiert". Frueher
// bedeutete "Stream stoppen" automatisch "Gruppe verlassen"; beim Host riss das
// den Einstiegspunkt der ganzen Gruppe mit um (geteilter Link sofort tot).
async function stopBroadcast(fullTeardown) {
  const wasActive = broadcastState.active
  ffStopping = true
  const keepGroup = !!bcGroup && !fullTeardown
  // Beim echten Abbau die Gruppe sauber verlassen - ABWARTEN, damit ein sofortiger
  // Wiederbeitritt die Austrittsmeldung nicht ueberholen kann (Race, siehe groupLeave).
  if (bcGroup && fullTeardown) await groupLeave()
  if (ffRestartTimer) { clearTimeout(ffRestartTimer); ffRestartTimer = null }
  if (ffRestartDebounce) { clearTimeout(ffRestartDebounce); ffRestartDebounce = null }
  bcKillCap()
  if (ffProc) { try { ffProc.kill() } catch {}; ffProc = null }
  if (mtxProc) { try { mtxProc.kill() } catch {}; mtxProc = null }
  if (keepGroup) {
    // Nur die Pipeline ist aus; Server, Portfreigaben und oeffentliche Adressen
    // bleiben gueltig (der Server laeuft ja weiter). Zuschauerzaehler stoppen.
    if (bcViewerPoll) { clearInterval(bcViewerPoll); bcViewerPoll = null }
    broadcastState = { active: false, port: BROADCAST_PORT, link: '', linkV4: broadcastState.linkV4 || '', linkV6: broadcastState.linkV6 || '', lanLink: '', viewers: 0, quality: '', internet: broadcastState.internet, opening: false }
    bcPushState()
    groupPushState()                        // UI: Gruppe lebt weiter, eigener Stream pausiert
    groupTick().catch(() => {})             // streaming=false sofort an alle verteilen
    osdDbg('[stream] gestoppt (Gruppe bleibt aktiv, Server laeuft weiter)')
    return
  }
  bcTeardownServer()
  osdDbg('[stream] gestoppt')
}
// Globaler Hotkey: Freigabe schnell an/aus – auch mitten im Spiel (der Poll mit
// GetAsyncKeyState greift dort, wo globalShortcut abgefangen wird). Beim Start
// wird das AKTUELLE Vordergrundfenster (i.d.R. das laufende Spiel) freigegeben –
// wie man es erwartet, wenn man den Hotkey im Spiel drueckt. Ist Lumora/Overlay
// selbst vorn, das zuletzt aktive Spiel; findet sich keins, bleibt die in den
// Einstellungen gewaehlte Quelle.
function toggleBroadcastHotkey() {
  const wasActive = broadcastState.active
  if (wasActive) {
    stopBroadcast()
  } else {
    let hwnd = 0
    try { hwnd = fgWin ? Number(fgWin.get()) : 0 } catch {}
    const own = nativeHwnd(mainWindow), ovl = nativeHwnd(overlayWindow)
    if (!hwnd || hwnd === own || hwnd === ovl) hwnd = prevGameHwnd || 0
    if (hwnd) {
      appSettings.streamSource = 'window:' + hwnd
      saveAppSettings()
      sendToUi('stream-source-changed', {})   // Renderer aktualisiert die Quellen-Anzeige
    }
    startBroadcast()
  }
  // Akustische Rueckmeldung – man drueckt den Hotkey im Spiel und sieht die UI nicht.
  sendToUi('stream-toggle-sound', { on: !wasActive })
}
// Live-Vorschau im Stream-Tab: der Renderer haengt sich per WHEP an den EIGENEN
// Broadcast-Server (localhost) und dekodiert den Stream per Hardware im Browser –
// exakt dasselbe Bild wie der Zuschauer, in voller Qualitaet, aber OHNE zweiten
// Encoder und OHNE Upload (alles lokal im RAM). Der SDP-Austausch laeuft ueber IPC,
// damit der Renderer keinen Cross-Origin-fetch auf den Server braucht. Solche
// localhost-Sessions zaehlen ueber bcIsLocalViewer NICHT als Zuschauer.
ipcMain.handle('preview-whep', (e, offerSdp) => new Promise((resolve) => {
  try {
    const port = (broadcastState && broadcastState.port) || BROADCAST_PORT
    // User-Agent 'LumoraPreview' markiert die Session -> aus der Zuschauerzaehlung gefiltert.
    const r = require('http').request({ host: '127.0.0.1', port, path: '/whep', method: 'POST', headers: { 'Content-Type': 'application/sdp', 'User-Agent': PREVIEW_UA } }, (resp) => {
      let body = ''
      resp.on('data', (d) => { body += d })
      resp.on('end', () => resolve({ ok: resp.statusCode >= 200 && resp.statusCode < 300, answer: body, session: resp.headers.location || null }))
    })
    r.on('error', () => resolve({ ok: false }))
    r.end(offerSdp)
  } catch { resolve({ ok: false }) }
}))
ipcMain.handle('preview-whep-stop', (e, session) => {
  if (!session) return
  try {
    const port = (broadcastState && broadcastState.port) || BROADCAST_PORT
    const r = require('http').request({ host: '127.0.0.1', port, path: session, method: 'DELETE' })
    r.on('error', () => {}); r.end()
  } catch {}
})
ipcMain.handle('start-broadcast', () => startBroadcast())
ipcMain.handle('stop-broadcast', async () => { await stopBroadcast(); return broadcastState })
ipcMain.handle('broadcast-status', () => broadcastState)
ipcMain.handle('group-start', () => { osdDbg('[gruppe] IPC group-start empfangen'); const r = groupStart(); osdDbg('[gruppe] IPC group-start liefert zurueck'); return r })
ipcMain.handle('group-join', (e, link) => groupJoin(link))
ipcMain.handle('group-leave', () => groupLeave())
// Beim expliziten Status-Abruf (Tab-Oeffnen) zusaetzlich pruefen, ob der zuletzt
// genutzte Raum NOCH EXISTIERT: nur dann wird das Beitreten-Feld vorbefuellt.
// Ein toter Code von gestern im Feld stiftet nur Verwirrung; ein lebender Raum
// (z. B. App-Absturz, waehrend die Freunde weiterzocken) macht den Wiederbeitritt
// zum Ein-Klick. 30-s-Cache, damit Tab-Wechsel nicht dauernd die Vermittlung anfragen.
let _lastCodeCheck = { code: '', alive: false, ts: 0 }
ipcMain.handle('group-status', async () => {
  const st = groupPublicState()
  if (!st.active && st.lastCode) {
    if (_lastCodeCheck.code !== st.lastCode || Date.now() - _lastCodeCheck.ts > 30000) {
      const r = await groupRelay('list', { code: st.lastCode })
      _lastCodeCheck = { code: st.lastCode, alive: !!(r && r.ok), ts: Date.now() }
    }
    st.lastCodeAlive = _lastCodeCheck.alive
  }
  return st
})
// TEMPORAER (Absturz-Diagnose Gruppen-Feature): Renderer-Debug ins selbe Log wie osdDbg.
ipcMain.on('r-log', (e, m) => osdDbg(String(m)))

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
  // Reste eines frueheren Datei-Updates aufraeumen (angewandt oder abgebrochen -
  // beim Start ist nie ein Staging in Benutzung).
  try { require('fs').rmSync(fuStagingDir(), { recursive: true, force: true }) } catch {}
  if (!autoUpdater) return
  autoUpdater.autoDownload = false           // nie ungefragt laden
  autoUpdater.autoInstallOnAppQuit = false   // Installation steuern wir selbst

  autoUpdater.on('update-available', (info) => {
    // releaseNotes kann String ODER Liste ({version, note}) sein – beides zu Text.
    let notes = ''
    if (typeof info.releaseNotes === 'string') notes = info.releaseNotes
    else if (Array.isArray(info.releaseNotes)) notes = info.releaseNotes.map(n => (n && n.note) || '').join('\n\n')
    // Zweisprachig: release.ps1 packt bei vorhandener EN-Notes-Datei DE + Marker
    // + EN in EIN releaseNotes-Feld (latest.yml kann nur eines). Hier trennen.
    let notesEn = ''
    const sp = notes.split(/^===EN===$/m)
    if (sp.length > 1) { notes = sp[0].trim(); notesEn = sp[1].trim() }
    sendToUi('update-available', { version: info.version, notes, notesEn })
  })
  autoUpdater.on('update-not-available', () => {
    // Kein neuer Basis-Installer -> Datei-Update (Manifest) pruefen. Erst wenn
    // AUCH das nichts hat, bekommt ein manueller Check sein "aktuell".
    const manual = updateManualCheck
    updateManualCheck = false
    fuCheck().then((found) => { if (!found && manual) sendToUi('update-none', {}) })
      .catch(() => { if (manual) sendToUi('update-none', {}) })
  })
  autoUpdater.on('download-progress', (p) => {
    sendToUi('update-progress', { percent: Math.round(p.percent || 0) })
  })
  autoUpdater.on('update-downloaded', (info) => {
    sendToUi('update-ready', { version: info.version })
  })
  autoUpdater.on('error', (err) => {
    // Feed-Fehler (z.B. latest.yml nicht erreichbar): der Manifest-Weg kann
    // trotzdem funktionieren - erst versuchen, dann ggf. den Fehler melden.
    const manual = updateManualCheck
    updateManualCheck = false
    fuCheck().then((found) => { if (!found && manual) sendToUi('update-error', { message: String(err && err.message || err) }) })
      .catch(() => { if (manual) sendToUi('update-error', { message: String(err && err.message || err) }) })
  })
}

function checkForUpdates(manual) {
  if (!autoUpdater || !app.isPackaged) {
    // Ohne electron-updater-Modul, aber gepackt: wenigstens den Datei-Update-
    // Weg (Manifest) versuchen. Im Dev-Modus gibt es nichts zu aktualisieren.
    if (app.isPackaged) {
      fuCheck().then((found) => { if (!found && manual) sendToUi('update-none', {}) }).catch(() => { if (manual) sendToUi('update-none', {}) })
      return
    }
    if (manual) sendToUi('update-none', {})
    return
  }
  updateManualCheck = !!manual
  autoUpdater.checkForUpdates().catch((err) => {
    if (manual) sendToUi('update-error', { message: String(err && err.message || err) })
    updateManualCheck = false
  })
}

// --- Datei-Updater (Basis-Installer-Strategie, seit 2.2.12) --------------------
// Problem: Jedes klassische Update laedt einen kompletten neuen unsignierten
// Installer (~170 MB), der bei SmartScreen wieder bei null anfaengt. Strategie
// jetzt: Der einmal verteilte Basis-Installer bleibt eingefroren (sammelt
// Reputation); Updates tauschen nur noch die App-Dateien (app.asar ~5 MB plus
// geaenderte Binaries) anhand von updates/app/manifest.json - Version, Notes,
// minElectron, Dateiliste mit sha512+Groesse (erzeugt release.ps1).
// Arbeitsteilung: latest.yml/electron-updater wird ZUERST gefragt und bleibt
// fuer seltene Basis-Wechsel (neue Electron-Version -> neue exe). Meldet er
// nichts, prueft fuCheck das Manifest. Die bestehende Update-UI laeuft
// unveraendert mit (gleiche IPC-Kanaele update-available/-progress/-ready).
// "Laden" holt nur Dateien mit abweichendem sha512 in ein Staging und
// verifiziert jeden Download; "Neu starten" schreibt apply.ps1 + Dateiliste
// und beendet die App - das Skript wartet auf den Prozess-Exit, tauscht mit
// .bak-Rollback und startet Lumora neu. Installation liegt unter
// %LOCALAPPDATA%\Programs -> kein Admin noetig. Log: %TEMP%\lumora-update.log.
const FU_MANIFEST_URL = 'https://lumora.kara-webdesign.de/updates/app/manifest.json'
let fuPending = null   // Manifest eines ausstehenden Updates (nach fuCheck)
let fuReady = false    // Staging vollstaendig verifiziert -> Neustart moeglich
let fuDownloading = false  // Re-Entrancy-Schutz: laeuft gerade ein Datei-Download?
function fuStagingDir() { return path.join(app.getPath('userData'), 'update-staging') }
function fuVersionNewer(a, b) {   // ist a neuer als b?
  const pa = String(a).split('.').map((x) => parseInt(x, 10) || 0)
  const pb = String(b).split('.').map((x) => parseInt(x, 10) || 0)
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) > (pb[i] || 0)
  }
  return false
}
// Manifest-Pfade absichern: nur schlichte relative Pfade INNERHALB von
// resources (Tiefenverteidigung, obwohl das Manifest von der eigenen
// HTTPS-Domain kommt). Das Zeichen-Whitelist MUSS scoped npm-Pakete zulassen
// (@scope/name, z.B. @koromix/koffi) sowie '+'/'~' - sonst scheitert der
// gesamte Download an einem einzigen gueltigen Modulpfad. Die eigentliche
// Ausbruch-Sicherheit leisten der '..'-Ausschluss + der startsWith-Check.
function fuSafeTarget(rel) {
  if (typeof rel !== 'string' || !/^[A-Za-z0-9._@+~/-]+$/.test(rel) || rel.includes('..')) return null
  const abs = path.resolve(process.resourcesPath, rel)
  return abs.startsWith(path.resolve(process.resourcesPath) + path.sep) ? abs : null
}
function fuHttpsGet(url, destPath, onData, redirects) {
  return new Promise((resolve, reject) => {
    if ((redirects || 0) > 3) return reject(new Error('zu viele Weiterleitungen'))
    const req = require('https').get(url, { headers: { 'User-Agent': 'Lumora-Updater' } }, (res) => {
      if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
        res.resume()
        return resolve(fuHttpsGet(new URL(res.headers.location, url).toString(), destPath, onData, (redirects || 0) + 1))
      }
      if (res.statusCode !== 200) { res.resume(); return reject(new Error('HTTP ' + res.statusCode + ': ' + url)) }
      if (!destPath) {
        let buf = ''
        res.on('data', (c) => { buf += c; if (buf.length > 1048576) req.destroy(new Error('Manifest zu gross')) })
        res.on('end', () => resolve(buf))
      } else {
        // Synchrone Fehler (mkdirSync: MAX_PATH, Platte voll, ENOTDIR) laufen im
        // Response-Callback AUSSERHALB des Promise-Executors - ohne dieses
        // try/catch bliebe das Promise ewig offen und fuDownload haenge still.
        try {
          const fs2 = require('fs')
          fs2.mkdirSync(path.dirname(destPath), { recursive: true })
          const out = fs2.createWriteStream(destPath)
          res.on('data', (c) => { if (onData) onData(c.length) })
          res.pipe(out)
          out.on('finish', () => out.close(() => resolve(destPath)))
          out.on('error', reject)
        } catch (e) { res.resume(); reject(e) }
      }
      res.on('error', reject)
    })
    req.on('error', reject)
    req.setTimeout(30000, () => req.destroy(new Error('Timeout: ' + url)))
  })
}
function fuSha512(file) {
  return new Promise((resolve, reject) => {
    const h = require('crypto').createHash('sha512')
    const s = require('fs').createReadStream(file)
    s.on('data', (c) => h.update(c))
    s.on('end', () => resolve(h.digest('hex')))
    s.on('error', reject)
  })
}
async function fuCheck() {
  if (!app.isPackaged) return false
  let mf
  try { mf = JSON.parse(await fuHttpsGet(FU_MANIFEST_URL)) } catch (e) { osdDbg('[update] Manifest nicht ladbar: ' + (e && e.message)); return false }
  if (!mf || !mf.version || !Array.isArray(mf.files)) return false
  // Braucht das Update eine neuere Electron-Basis, kommt die (selten) als
  // klassisches Setup ueber latest.yml - dieses Manifest ist dann nicht fuer uns.
  if (mf.minElectron && parseInt(process.versions.electron, 10) < mf.minElectron) {
    osdDbg('[update] Manifest ' + mf.version + ' braucht Electron ' + mf.minElectron + ' - Basis zu alt')
    return false
  }
  if (!fuVersionNewer(mf.version, app.getVersion())) return false
  fuPending = mf
  fuReady = false
  osdDbg('[update] Datei-Update verfuegbar: ' + app.getVersion() + ' -> ' + mf.version)
  sendToUi('update-available', { version: mf.version, notes: mf.notes || '', notesEn: mf.notesEn || '' })
  return true
}
async function fuDownload() {
  const mf = fuPending
  if (!mf) return
  const base = FU_MANIFEST_URL.replace(/manifest\.json$/, '')
  const staging = fuStagingDir()
  try { require('fs').rmSync(staging, { recursive: true, force: true }) } catch {}
  // Nur laden, was lokal wirklich abweicht (erst Groesse - billig, dann Hash).
  const todo = []
  for (const f of mf.files) {
    const target = fuSafeTarget(f.path)
    if (!target || !f.sha512) throw new Error('Manifest-Eintrag ungueltig: ' + f.path)
    let same = false
    try {
      const st = require('fs').statSync(target)
      if (st.size === f.size) same = (await fuSha512(target)) === f.sha512
    } catch {}
    if (!same) todo.push(f)
  }
  if (!todo.length) { fuReady = true; sendToUi('update-ready', { version: mf.version }); return }
  const total = todo.reduce((s, f) => s + (f.size || 0), 0)
  let got = 0, lastPct = -1
  for (const f of todo) {
    const dest = path.join(staging, f.path)
    await fuHttpsGet(base + f.path.split('/').map(encodeURIComponent).join('/'), dest, (n) => {
      got += n
      const pct = total ? Math.min(99, Math.round(got * 100 / total)) : 0
      if (pct !== lastPct) { lastPct = pct; sendToUi('update-progress', { percent: pct }) }
    })
    const hash = await fuSha512(dest)
    if (hash !== f.sha512) throw new Error('Pruefsumme falsch: ' + f.path)
  }
  osdDbg('[update] ' + todo.length + ' Datei(en) geladen und verifiziert (' + Math.round(total / 1024) + ' KB)')
  fuReady = true
  sendToUi('update-progress', { percent: 100 })
  sendToUi('update-ready', { version: mf.version })
}
function fuInstall() {
  const mf = fuPending
  if (!mf || !fuReady) return
  const fs2 = require('fs')
  const staging = fuStagingDir()
  const lines = []
  for (const f of mf.files) {
    const src = path.join(staging, f.path)
    const dst = fuSafeTarget(f.path)
    if (dst && fs2.existsSync(src)) lines.push(src + '|' + dst)
  }
  if (!lines.length) return
  fs2.writeFileSync(path.join(staging, 'apply-list.txt'), lines.join('\r\n'), 'utf8')
  // Apply-Skript (ASCII-only, PowerShell 5.1): wartet auf den Prozess-Exit,
  // tauscht jede Datei mit .bak-Sicherung (Retry gegen kurze Sperren), rollt
  // bei Fehlern komplett zurueck und startet Lumora neu.
  const ps1 = path.join(staging, 'apply.ps1')
  fs2.writeFileSync(ps1, [
    'param([int]$LumoraPid, [string]$ExePath)',
    "$log = Join-Path $env:TEMP 'lumora-update.log'",
    'function L($m) { try { Add-Content -Path $log -Value ((Get-Date -Format o) + \" \" + $m) } catch {} }',
    'L (\"apply start pid=\" + $LumoraPid)',
    'try { Wait-Process -Id $LumoraPid -Timeout 30 -ErrorAction SilentlyContinue } catch {}',
    "if ($ExePath) { try { Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $ExePath } | Stop-Process -Force -ErrorAction SilentlyContinue } catch {} }",
    'Start-Sleep -Milliseconds 500',
    "$list = Get-Content -Encoding UTF8 (Join-Path $PSScriptRoot 'apply-list.txt')",
    '$done = @()',
    '$ok = $true',
    'foreach ($line in $list) {',
    "  $p = $line -split '\\|'",
    '  $src = $p[0]; $dst = $p[1]; $bak = $dst + \".bak\"',
    '  try {',
    '    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null',
    '    if (Test-Path $bak) { Remove-Item -Force $bak }',
    '    $moved = $false',
    '    $backedUp = $false',
    '    for ($i = 0; $i -lt 20; $i++) {',
    '      try {',
    '        if (Test-Path $dst) { Move-Item -Force $dst $bak; $backedUp = $true }',
    '        Move-Item -Force $src $dst; $moved = $true; break',
    '      } catch { Start-Sleep -Milliseconds 500 }',
    '    }',
    '    if (-not $moved) {',
    '      if ($backedUp -and (Test-Path $bak) -and -not (Test-Path $dst)) { try { Move-Item -Force $bak $dst } catch {} }',
    '      throw (\"gesperrt: \" + $dst)',
    '    }',
    '    $done += ,@($dst, $bak)',
    '    L (\"ok \" + $dst)',
    '  } catch { L (\"FEHLER \" + $_.Exception.Message); $ok = $false; break }',
    '}',
    'if (-not $ok) {',
    '  foreach ($d in $done) { try { if (Test-Path $d[1]) { Move-Item -Force $d[1] $d[0] } } catch {} }',
    '  L \"rollback\"',
    '} else {',
    '  foreach ($d in $done) { try { if (Test-Path $d[1]) { Remove-Item -Force $d[1] } } catch {} }',
    '  L \"fertig\"',
    '}',
    'if ($ExePath) { try { Start-Process $ExePath } catch { L (\"start-fehler \" + $_.Exception.Message) } }',
  ].join('\r\n'), 'utf8')
  osdDbg('[update] Apply-Skript geschrieben (' + lines.length + ' Datei(en)) - App beendet sich fuer den Tausch')
  try {
    spawn('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ps1, '-LumoraPid', String(process.pid), '-ExePath', process.execPath], { detached: true, stdio: 'ignore', windowsHide: true }).unref()
  } catch (e) { osdDbg('[update] Apply-Start fehlgeschlagen: ' + (e && e.message)); return }
  app.isQuitting = true
  app.quit()
}

// Toggle-Hotkey aus den Einstellungen setzen. Leerer String = deaktiviert.
// Liefert { ok } zurueck – false, wenn die Kombination nicht registriert werden
// konnte (z.B. schon vom System/anderer App belegt).
ipcMain.handle('set-hotkey', (event, accelerator, which) => {
  const key = which === 'osd' ? 'osdHotkey' : which === 'osdEdit' ? 'osdEditHotkey' : which === 'osdAb' ? 'osdAbHotkey' : which === 'stream' ? 'streamHotkey' : 'toggleHotkey'
  appSettings[key] = accelerator || ''
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
// Live-Edit: Design/Deckkraft/Werte direkt am Overlay – schreibt dieselben
// Einstellungen wie der Dialog (beide bleiben synchron) und zieht das OSD sofort nach.
ipcMain.handle('osd-edit-theme', (e, theme) => {
  if (['compact', 'min', 'bar', 'neon', 'strip', 'tiles'].includes(theme)) { appSettings.osdTheme = theme; saveAppSettings(); applyOsdConfig() }
})
ipcMain.handle('osd-edit-opacity', (e, op) => {
  appSettings.osdOpacity = Math.max(0.2, Math.min(0.95, Number(op) || 0.55)); saveAppSettings(); applyOsdConfig()
})
ipcMain.handle('osd-edit-fields', (e, fields) => {
  if (fields && typeof fields === 'object') { appSettings.osdFields = fields; saveAppSettings(); applyOsdConfig() }
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
ipcMain.handle('download-update', () => {
  // Ausstehendes Datei-Update hat Vorrang (fuCheck lief nach dem Feed-Check).
  if (fuPending) {
    if (fuDownloading) return   // Re-Entrancy: laeuft schon -> zweiten Klick ignorieren
    fuDownloading = true
    return fuDownload().catch((e) => {
      osdDbg('[update] Download fehlgeschlagen: ' + (e && e.message))
      sendToUi('update-error', { message: String(e && e.message || e) })
    }).finally(() => { fuDownloading = false })
  }
  // electron-updater-Zweig: Fehler NICHT verschlucken - sonst haengt der
  // sichtbare "Update wird geladen"-Toast ewig (dieselbe Klasse wie BUG B).
  if (autoUpdater) autoUpdater.downloadUpdate().catch((err) => sendToUi('update-error', { message: String(err && err.message || err) }))
})
// isSilent=true -> Installer mit /S (kein Wizard, In-Place-Update am gespeicherten
// Pfad); isForceRunAfter=true -> Lumora danach automatisch neu starten. Ohne das
// erste true liefe beim Auto-Update der komplette Ersteinrichtungs-Assistent auf.
ipcMain.handle('install-update', () => {
  if (fuPending && fuReady) return fuInstall()   // Datei-Update: Tausch beim Beenden + Neustart
  if (autoUpdater) { app.isQuitting = true; autoUpdater.quitAndInstall(true, true) }
})

// Nur eine Instanz zulassen: ein zweiter Start holt das vorhandene (ggf. im Tray
// versteckte) Fenster nach vorne, statt eine neue Instanz zu öffnen.
if (process.argv.includes('--fps-broker')) {
  // Broker-Instanz: kein Fenster, keine Single-Instance-Sperre, nur FPS messen.
  try { app.disableHardwareAcceleration() } catch {}   // schlanker (keine GPU-Prozesse)
  app.whenReady().then(runFpsBroker)
} else if (process.argv.includes('--sensor-broker')) {
  // Sensor-Broker: elevated via geplanter Aufgabe, liest CPU-Temp/-Power via PawnIO.
  try { app.disableHardwareAcceleration() } catch {}
  app.whenReady().then(runSensorBroker)
} else if (!app.requestSingleInstanceLock()) {
  app.quit()
} else {
  // Windows Graphics Capture (WGC) fuer getDisplayMedia/Broadcast erzwingen.
  // Der alte Desktop-Duplication-Pfad bricht bei Vollbild-/Flip-Model-Spielen
  // auf ~40 fps ein (Desktop fluessig, Spiel ruckelt); WGC greift am DWM-
  // Compositor ab und liefert bei Spielen volle 60 fps. Steuerung ueber
  // WebRTC-Field-Trials (allow_wgc_capturer) – braucht Win10 1809+ (erfuellt).
  try {
    // ENTSCHEIDEND fuer fluessige Aufnahme bei 4K: Chromium deckelt die Bildschirm-
    // aufnahme per Default auf 50% CPU-Zeit eines Kerns (kDefaultMaximumCpuConsumption
    // Percentage=50). Bei teuren 4K-Frames verdoppelt das die Aufnahme-Periode ->
    // ~30 fps (genau die gemessenen 32). Auf 100 heben = volle Bildrate.
    app.commandLine.appendSwitch('webrtc-max-cpu-consumption-percentage', '100')
    // WGC-Capturer + Frame-Lieferung per GPU-TEXTUR (statt langsamer CPU-Kopie) ->
    // weniger CPU-Zeit pro Frame, hoehere Aufnahme-Bildrate.
    app.commandLine.appendSwitch('force-fieldtrials', 'WebRTC-AllowWgcScreenCapturer/Enabled/WebRTC-AllowWgcWindowCapturer/Enabled/')
    app.commandLine.appendSwitch('enable-features', 'AllowWgcScreenCapturer,AllowWgcWindowCapturer,AllowWgcUsingTexture')
    // Unsichtbares Broadcast-Fenster nicht drosseln.
    app.commandLine.appendSwitch('disable-renderer-backgrounding')
    app.commandLine.appendSwitch('disable-backgrounding-occluded-windows')
    app.commandLine.appendSwitch('disable-background-timer-throttling')
    // ZERO-COPY GPU-Aufnahme: haelt die Frames als GPU-Textur (statt teurer CPU-
    // Kopie) und reicht sie direkt an den Hardware-Encoder -> volle Bildrate auch
    // bei 4K. Wirkt zusammen mit HAGS (Hardware-GPU-Scheduling) am staerksten.
    app.commandLine.appendSwitch('enable-zero-copy')
    app.commandLine.appendSwitch('enable-native-gpu-memory-buffers')
    app.commandLine.appendSwitch('enable-gpu-memory-buffer-video-frames')
  } catch {}
  app.on('second-instance', () => showMainWindow())

  app.whenReady().then(() => { setupAutoUpdate(); createWindow(); setupGamepadHotkey(); registerToggleHotkey(); setupNvml(); setupAdl(); setupRtss(); setupMahm(); setupCpuClock(); setupVramCounter(); setupGpuName(); startExternalWatcher(); syncOsdVisibility() })

  // Encoder-Erkennung vorwaermen (PowerShell-GPU-Abfrage + ffmpeg -encoders kosten
  // beim allerersten Streamstart sonst 1-2 s auf dem kritischen Weg zum ersten Bild).
  app.whenReady().then(() => setTimeout(() => { bcDetectEncoder().catch(() => {}) }, 2500))

  // HDR beim Beenden nur abschalten, wenn KEIN getracktes Spiel mehr laeuft.
  // Sonst wuerde das Schliessen von Lumora ein laufendes Spiel mitten im Betrieb
  // von HDR auf SDR kicken. Laeuft noch eine Session, bleibt HDR bewusst an.
  app.on('before-quit', () => {
    if (hdrEnabledByLauncher) {
      if (externalSessions.size > 0 || activeLaunchExes.size > 0) {
        playLog('QUIT: Spiel laeuft noch -> HDR bleibt an')
        return
      }
      try { execSync(`"${getHdrCmdPath()}" off`) } catch {}
      hdrEnabledByLauncher = false
    }
  })

  app.on('will-quit', () => { globalShortcut.unregisterAll(); if (xinputPoll) clearInterval(xinputPoll); if (timeEndPeriodFn) try { timeEndPeriodFn(1) } catch {}; stopFps(); stopSensorBroker(); try { rtssOsdClear() } catch {}; try { rtssReleaseVisible() } catch {}; try { stopBroadcast(true).catch(() => {}) } catch {} })

  app.on('window-all-closed', () => {
    if (hdrPollInterval) clearInterval(hdrPollInterval)
    app.quit()
  })
}
