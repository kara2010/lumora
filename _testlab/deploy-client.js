// deploy-client.js  -  laedt Dateien gechunkt zum deploy.php-Endpunkt hoch.
//
// Aufruf:  node _testlab/deploy-client.js <plan.json>
//   plan.json = { "files": [ { "local": "<abs/rel Pfad>", "remote": "<Zielpfad auf Server>" }, ... ] }
//
// Zugangsdaten kommen aus _testlab/deploy.local.json (GITIGNORED, nie im Repo):
//   { "baseUrl": "https://lumora.kara-webdesign.de/deploy.php", "token": "<geheim>" }
//
// Sicherheit: Token nur im Header, HTTPS erzwungen, sha256 wird gegen den vom
// Server zurueckgemeldeten Hash geprueft (Uebertragungs-Integritaet).
const fs = require('fs')
const path = require('path')
const https = require('https')
const crypto = require('crypto')

const CHUNK = 4 * 1024 * 1024   // 4 MB je Request (sicher unter post_max_size)

function loadCfg() {
  const p = path.join(__dirname, 'deploy.local.json')
  if (!fs.existsSync(p)) { console.error('FEHLER: _testlab/deploy.local.json fehlt (baseUrl + token).'); process.exit(2) }
  const c = JSON.parse(fs.readFileSync(p, 'utf8'))
  if (!/^https:\/\//i.test(c.baseUrl || '')) { console.error('FEHLER: baseUrl muss https sein.'); process.exit(2) }
  if (!c.token || String(c.token).length < 32) { console.error('FEHLER: token fehlt/zu kurz.'); process.exit(2) }
  return c
}

function putChunk(cfg, remote, buf, offset, final) {
  return new Promise((resolve, reject) => {
    const u = new URL(cfg.baseUrl)
    const req = https.request({
      hostname: u.hostname, port: u.port || 443, path: u.pathname, method: 'POST',
      headers: {
        'Content-Type': 'application/octet-stream',
        'Content-Length': buf.length,
        'X-Deploy-Auth': cfg.token,
        'X-Deploy-Path': remote,
        'X-Deploy-Offset': String(offset),
        'X-Deploy-Final': final ? '1' : '0',
      },
    }, (res) => {
      let body = ''
      res.on('data', (d) => (body += d))
      res.on('end', () => {
        let j
        try { j = JSON.parse(body) } catch { return reject(new Error('HTTP ' + res.statusCode + ': ' + body.slice(0, 200))) }
        if (res.statusCode !== 200 || !j.ok) return reject(new Error('deploy ' + res.statusCode + ': ' + (j.error || body)))
        resolve(j)
      })
    })
    req.setTimeout(60000, () => req.destroy(new Error('timeout')))
    req.on('error', reject)
    req.end(buf)
  })
}

async function uploadFile(cfg, local, remote) {
  const data = fs.readFileSync(local)
  const localSha = crypto.createHash('sha256').update(data).digest('hex')
  let offset = 0, last = null
  if (data.length === 0) { last = await putChunk(cfg, remote, Buffer.alloc(0), 0, true) }
  while (offset < data.length) {
    const end = Math.min(offset + CHUNK, data.length)
    const final = end >= data.length
    last = await putChunk(cfg, remote, data.subarray(offset, end), offset, final)
    offset = end
    process.stdout.write('  ' + remote + '  ' + Math.round(offset * 100 / data.length) + '%\r')
  }
  if (!last || last.sha256 !== localSha) throw new Error('sha256-Mismatch bei ' + remote + ' (Server: ' + (last && last.sha256) + ', lokal: ' + localSha + ')')
  console.log('  ✓ ' + remote + '  (' + (data.length / 1048576).toFixed(2) + ' MB, sha256 ok)        ')
}

async function main() {
  const planPath = process.argv[2]
  if (!planPath) { console.error('Aufruf: node deploy-client.js <plan.json>'); process.exit(2) }
  const cfg = loadCfg()
  const plan = JSON.parse(fs.readFileSync(planPath, 'utf8'))
  if (!Array.isArray(plan.files) || !plan.files.length) { console.error('plan.files leer.'); process.exit(2) }
  console.log('Deploy -> ' + cfg.baseUrl + '  (' + plan.files.length + ' Datei(en))')
  for (const f of plan.files) {
    if (!fs.existsSync(f.local)) throw new Error('Lokale Datei fehlt: ' + f.local)
    await uploadFile(cfg, f.local, f.remote)
  }
  console.log('Fertig: alle ' + plan.files.length + ' Datei(en) hochgeladen + verifiziert.')
}
main().catch((e) => { console.error('\nDEPLOY FEHLGESCHLAGEN: ' + e.message); process.exit(1) })
