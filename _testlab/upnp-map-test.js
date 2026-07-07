// Testet den vollen UPnP-Portfreigabe-Zyklus (reines Node): Router finden,
// AddPortMapping(8787/TCP), per GetSpecificPortMappingEntry verifizieren,
// wieder DeletePortMapping. Beweist, ob Lumora den Port automatisch oeffnen darf.
'use strict'
const dgram = require('dgram')
const http = require('http')
const os = require('os')
const { URL } = require('url')
const PORT = 8787

function localIPv4() {
  const ifs = os.networkInterfaces()
  for (const n of Object.keys(ifs)) for (const a of ifs[n]) if (a.family === 'IPv4' && !a.internal) return a.address
  return '0.0.0.0'
}
const LOCAL = localIPv4()

function discover(t) {
  return new Promise((resolve) => {
    const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true })
    const locs = []
    sock.on('message', (b) => { const s = b.toString('latin1'); if (/InternetGatewayDevice|WAN(IP|PPP)Connection/i.test(s)) { const m = /LOCATION:\s*(\S+)/i.exec(s); if (m && !locs.includes(m[1])) locs.push(m[1]) } })
    sock.on('error', () => {})
    sock.bind(0, LOCAL, () => { try { sock.setMulticastTTL(4); sock.setMulticastInterface(LOCAL) } catch {}; for (const st of ['urn:schemas-upnp-org:device:InternetGatewayDevice:1', 'urn:schemas-upnp-org:service:WANIPConnection:1']) { const m = Buffer.from('M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "ssdp:discover"\r\nMX: 2\r\nST: ' + st + '\r\n\r\n'); sock.send(m, 1900, '239.255.255.250') } })
    setTimeout(() => { try { sock.close() } catch {}; resolve(locs) }, t || 3000)
  })
}
function httpGet(u) { return new Promise((res, rej) => { const r = http.get(u, { timeout: 4000 }, (x) => { let d = ''; x.on('data', c => d += c); x.on('end', () => res({ status: x.statusCode, body: d })) }); r.on('error', rej); r.on('timeout', () => { r.destroy(); rej(new Error('timeout')) }) }) }
function soap(controlURL, st, action, inner) {
  return new Promise((resolve, reject) => {
    const u = new URL(controlURL)
    const xml = '<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:' + action + ' xmlns:u="' + st + '">' + (inner || '') + '</u:' + action + '></s:Body></s:Envelope>'
    const req = http.request({ host: u.hostname, port: u.port, path: u.pathname, method: 'POST', headers: { 'Content-Type': 'text/xml; charset="utf-8"', 'SOAPAction': '"' + st + '#' + action + '"', 'Content-Length': Buffer.byteLength(xml) }, timeout: 4000 }, (res) => { let d = ''; res.on('data', c => d += c); res.on('end', () => resolve({ status: res.statusCode, body: d })) })
    req.on('error', reject); req.on('timeout', () => { req.destroy(); reject(new Error('timeout')) })
    req.write(xml); req.end()
  })
}
async function resolveCtrl() {
  const locs = await discover(3000)
  for (const loc of locs) {
    let desc; try { desc = await httpGet(loc) } catch { continue }
    if (!/InternetGatewayDevice/i.test(desc.body)) continue
    const re = /<service>([\s\S]*?)<\/service>/g; let m
    while ((m = re.exec(desc.body))) {
      if (!/WAN(IP|PPP)Connection/i.test(m[1])) continue
      const type = (/<serviceType>([^<]+)<\/serviceType>/i.exec(m[1]) || [])[1]
      const ctrl = (/<controlURL>([^<]+)<\/controlURL>/i.exec(m[1]) || [])[1]
      if (!type || !ctrl) continue
      const base = new URL(loc)
      return { controlURL: ctrl.startsWith('http') ? ctrl : base.protocol + '//' + base.host + (ctrl.startsWith('/') ? '' : '/') + ctrl, serviceType: type }
    }
  }
  return null
}
;(async () => {
  console.log('Lokale IP:', LOCAL)
  const c = await resolveCtrl()
  if (!c) { console.log('Kein IGD gefunden.'); return }
  console.log('Control-URL:', c.controlURL, '\n')

  const inner = '<NewRemoteHost></NewRemoteHost><NewExternalPort>' + PORT + '</NewExternalPort><NewProtocol>TCP</NewProtocol><NewInternalPort>' + PORT + '</NewInternalPort><NewInternalClient>' + LOCAL + '</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>Lumora Stream Test</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>'
  console.log('1) AddPortMapping 8787/TCP ...')
  let r = await soap(c.controlURL, c.serviceType, 'AddPortMapping', inner)
  console.log('   -> HTTP', r.status)
  if (r.status !== 200) { const err = (/<errorDescription>([^<]*)<\/errorDescription>/i.exec(r.body) || [])[1] || (/<errorCode>([^<]*)<\/errorCode>/i.exec(r.body) || [])[1]; console.log('   FEHLER:', err || r.body.slice(0, 200)) }

  console.log('2) GetSpecificPortMappingEntry 8787/TCP ...')
  r = await soap(c.controlURL, c.serviceType, 'GetSpecificPortMappingEntry', '<NewRemoteHost></NewRemoteHost><NewExternalPort>' + PORT + '</NewExternalPort><NewProtocol>TCP</NewProtocol>')
  if (r.status === 200) {
    const client = (/<NewInternalClient>([^<]*)<\/NewInternalClient>/i.exec(r.body) || [])[1]
    const enabled = (/<NewEnabled>([^<]*)<\/NewEnabled>/i.exec(r.body) || [])[1]
    console.log('   -> Eintrag AKTIV: Client=' + client + ' Enabled=' + enabled)
    console.log('\n*** UPnP-PORTFREIGABE FUNKTIONIERT *** Lumora kann 8787 automatisch oeffnen.')
  } else { console.log('   -> HTTP', r.status, '(kein Eintrag gefunden)') }

  console.log('\n3) DeletePortMapping (aufraeumen) ...')
  r = await soap(c.controlURL, c.serviceType, 'DeletePortMapping', '<NewRemoteHost></NewRemoteHost><NewExternalPort>' + PORT + '</NewExternalPort><NewProtocol>TCP</NewProtocol>')
  console.log('   -> HTTP', r.status)
})()
