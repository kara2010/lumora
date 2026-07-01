const { app, BrowserWindow, ipcMain, dialog, shell, screen, Tray, Menu, nativeImage, globalShortcut } = require('electron')
const path = require('path')
const fs = require('fs')
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
const appSettings = { autostart: false, startMinimized: false, minimizeToTray: false, steamGridDbKey: '', toggleHotkey: 'Alt+L' }
app.isQuitting = false

function loadAppSettings() {
  try { Object.assign(appSettings, JSON.parse(fs.readFileSync(appSettingsFile, 'utf8'))) } catch {}
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
  if (!mainWindow) return
  if (mainWindow.isMinimized()) mainWindow.restore()
  mainWindow.show()
  mainWindow.focus()
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

// (Neu-)Registriert den konfigurierten Toggle-Hotkey. Gibt true/false zurueck,
// je nachdem ob die Kombination angenommen wurde (kann vom OS belegt sein).
function registerToggleHotkey() {
  globalShortcut.unregisterAll()
  const acc = appSettings.toggleHotkey
  if (!acc) return true
  try { return globalShortcut.register(acc, toggleMainWindow) }
  catch { return false }
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
  const wantMin = appSettings.startMinimized || process.argv.includes('--minimized')

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
      contextIsolation: false
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
    }
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
    let started = false, startTs = null, absent = 0
    const monitor = setInterval(async () => {
      const running = await probeRunning()
      if (!started) {
        if (running) {
          started = true; startTs = Date.now(); absent = 0
          mainWindow.webContents.send('launch-status', 'running')
          playLog(`STARTED nach +${Math.round((Date.now() - launchTs) / 1000)}s`)
        } else if (Date.now() - launchTs > 300000) {    // 5 Min – großzügig für schwere Spiele
          clearInterval(monitor)
          playLog(`TIMEOUT – nie erkannt nach ${Math.round((Date.now() - launchTs) / 1000)}s.  ` +
            `reg=${(steamInfo && steamInfo.appId) ? await steamAppRunning(steamInfo.appId) : 'n/a'}  ` +
            `name=${await processByName(exeName)}  folder=${isLnk ? 'n/a' : await anyProcessInFolder(gameDir)}`)
          endSession(null)
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
  try { event.returnValue = fs.readFileSync(gamesFile, 'utf8') }
  catch { event.returnValue = null }
})

ipcMain.handle('save-games', (event, json) => {
  try { fs.writeFileSync(gamesFile, json) } catch {}
})

ipcMain.on('load-prefs-sync', (event) => {
  try { event.returnValue = fs.readFileSync(prefsFile, 'utf8') }
  catch { event.returnValue = null }
})

ipcMain.handle('save-prefs', (event, json) => {
  try { fs.writeFileSync(prefsFile, json) } catch {}
})

ipcMain.handle('get-app-settings', () => appSettings)

ipcMain.handle('set-app-settings', (event, partial) => {
  Object.assign(appSettings, partial)
  saveAppSettings()
  applyAutostart()
  if (appSettings.minimizeToTray) createTray()
  else destroyTray()
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
    sendToUi('update-available', { version: info.version, notes: typeof info.releaseNotes === 'string' ? info.releaseNotes : '' })
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

ipcMain.handle('check-for-updates', () => { checkForUpdates(true) })
ipcMain.handle('download-update', () => { if (autoUpdater) autoUpdater.downloadUpdate().catch(() => {}) })
ipcMain.handle('install-update', () => { if (autoUpdater) { app.isQuitting = true; autoUpdater.quitAndInstall() } })

// Nur eine Instanz zulassen: ein zweiter Start holt das vorhandene (ggf. im Tray
// versteckte) Fenster nach vorne, statt eine neue Instanz zu öffnen.
if (!app.requestSingleInstanceLock()) {
  app.quit()
} else {
  app.on('second-instance', () => showMainWindow())

  app.whenReady().then(() => { setupAutoUpdate(); createWindow(); registerToggleHotkey() })

  // HDR abschalten, falls der Launcher es für ein noch laufendes Spiel aktiviert hatte
  app.on('before-quit', () => {
    if (hdrEnabledByLauncher) {
      try { execSync(`"${getHdrCmdPath()}" off`) } catch {}
      hdrEnabledByLauncher = false
    }
  })

  app.on('will-quit', () => globalShortcut.unregisterAll())

  app.on('window-all-closed', () => {
    if (hdrPollInterval) clearInterval(hdrPollInterval)
    app.quit()
  })
}
