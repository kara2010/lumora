// Zeigt verfuegbare Artwork-Bilder (Steam + Microsoft Store) fuer ein Spiel,
// inkl. Masse – damit man sieht, was die Cover-/Hero-Suche anbieten kann.
//   node artwork.js "Forza Horizon 6"
const https = require('https')
const term = process.argv.slice(2).join(' ').trim()
if (!term) { console.log('Aufruf: node artwork.js "<Spielname>"'); process.exit(1) }

function get(u) {
  return new Promise(res => {
    https.get(u, { headers: { 'User-Agent': 'Mozilla/5.0' } }, r => {
      const d = []; r.on('data', c => d.push(c)); r.on('end', () => res({ status: r.statusCode, b: Buffer.concat(d) }))
    }).on('error', () => res({ status: 0, b: Buffer.alloc(0) }))
  })
}
function jpegSize(buf) {
  let i = 2
  while (i < buf.length) {
    if (buf[i] !== 0xFF) { i++; continue }
    const m = buf[i + 1]
    if (m >= 0xC0 && m <= 0xCF && m !== 0xC4 && m !== 0xC8 && m !== 0xCC) return [buf.readUInt16BE(i + 7), buf.readUInt16BE(i + 5)]
    i += 2 + buf.readUInt16BE(i + 2)
  }
  return [0, 0]
}
const BASE = 'https://shared.akamai.steamstatic.com/store_item_assets/'
async function steamGetItems(ids) {
  const input = JSON.stringify({ ids: ids.map(a => ({ appid: Number(a) })), context: { language: 'english', country_code: 'US' }, data_request: { include_assets: true, include_basic_info: true } })
  const r = await get(`https://api.steampowered.com/IStoreBrowseService/GetItems/v1/?input_json=${encodeURIComponent(input)}`)
  const m = {}
  for (const it of (JSON.parse(r.b.toString())?.response?.store_items || [])) {
    const a = it.assets || {}
    const mk = f => (a.asset_url_format && f) ? BASE + a.asset_url_format.replace('${FILENAME}', f) : null
    m[String(it.id)] = {
      name: it.name,
      isDlc: it.type !== 0 || !!(it.related_items && it.related_items.parent_appid),
      cover: mk(a.library_capsule), hero: mk(a.library_hero), bg: mk(a.page_background),
    }
  }
  return m
}
async function dim(u) {
  if (!u) return '–'
  const c = await get(u)
  if (c.status !== 200 || c.b.length < 5000) return '– (fehlt/Platzhalter)'
  const [w, h] = jpegSize(c.b)
  return `${w}x${h}  ${Math.round(c.b.length / 1024)}KB`
}
;(async () => {
  console.log(`\n=== STEAM: "${term}" ===`)
  const s = await get(`https://store.steampowered.com/api/storesearch/?term=${encodeURIComponent(term)}&l=english&cc=US`)
  const items = (JSON.parse(s.b.toString()).items || []).slice(0, 10)
  const assets = await steamGetItems(items.map(i => i.id))
  for (const it of items) {
    const a = assets[String(it.id)] || {}
    if (a.isDlc) { console.log(`  [DLC, uebersprungen] ${it.name}`); continue }
    console.log(`  [${it.id}] ${it.name}`)
    console.log(`     cover (library_capsule): ${await dim(a.cover)}`)
    console.log(`     hero  (library_hero):    ${await dim(a.hero)}`)
    console.log(`     bg    (page_background): ${await dim(a.bg)}`)
  }
  console.log(`\n=== MICROSOFT STORE: "${term}" ===`)
  const m = await get(`https://storeedgefd.dsx.mp.microsoft.com/v9.0/search?query=${encodeURIComponent(term)}&market=US&locale=en-us&deviceFamily=Windows.Desktop`)
  const sr = JSON.parse(m.b.toString())?.Payload?.SearchResults || []
  for (const r of sr.slice(0, 6)) {
    const types = (r.Images || []).map(i => `${i.ImageType} ${i.Width}x${i.Height}`).join(', ')
    console.log(`  ${r.Title}: ${types || '–'}`)
  }
})()
