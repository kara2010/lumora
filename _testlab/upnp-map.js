// UPnP-Freigabe-Diagnose: Discovery -> externe IP -> AddPortMapping (Port 8787 TCP).
// Zeigt den exakten Router-Fehlercode, wenn die automatische Freigabe scheitert.
//   node _testlab/upnp-map.js
const http = require('http'), dgram = require('dgram'), { URL } = require('url')

let ip = '127.0.0.1'
const ifs = require('os').networkInterfaces()
for (const n of Object.keys(ifs)) for (const a of ifs[n]) if (a.family === 'IPv4' && !a.internal) { ip = a.address; break }

const httpGet = (u) => new Promise((res, rej) => {
  http.get(u, { timeout: 4000 }, (r) => { let d = ''; r.on('data', (c) => d += c); r.on('end', () => res({ status: r.statusCode, body: d })) }).on('error', rej)
})

const soap = (ctrl, type, action, inner) => new Promise((res, rej) => {
  const u = new URL(ctrl)
  const xml = '<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:' + action + ' xmlns:u="' + type + '">' + (inner || '') + '</u:' + action + '></s:Body></s:Envelope>'
  const rq = http.request({ host: u.hostname, port: u.port, path: u.pathname, method: 'POST', headers: { 'Content-Type': 'text/xml; charset="utf-8"', 'SOAPAction': '"' + type + '#' + action + '"', 'Content-Length': Buffer.byteLength(xml) }, timeout: 4000 }, (r) => { let d = ''; r.on('data', (c) => d += c); r.on('end', () => res({ status: r.statusCode, body: d })) })
  rq.on('error', rej); rq.write(xml); rq.end()
})

const discover = () => new Promise((resolve) => {
  const s = dgram.createSocket({ type: 'udp4', reuseAddr: true })
  let loc = null
  s.on('message', (b) => { const t = b.toString(); if (/InternetGatewayDevice/i.test(t) && !loc) { const l = /LOCATION:\s*(\S+)/i.exec(t); if (l) loc = l[1] } })
  s.bind(0, ip, () => {
    s.setMulticastTTL(4); try { s.setMulticastInterface(ip) } catch {}
    s.send(Buffer.from('M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "ssdp:discover"\r\nMX: 2\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n'), 1900, '239.255.255.250')
  })
  setTimeout(() => { s.close(); resolve(loc) }, 3000)
})

;(async () => {
  console.log('lokale IP:', ip)
  const loc = await discover()
  if (!loc) { console.log('keine Discovery-Antwort'); return }
  console.log('Gateway:', loc)
  const desc = await httpGet(loc)
  let m, ctrl, type
  const re = /<service>([\s\S]*?)<\/service>/g
  while ((m = re.exec(desc.body))) {
    if (/WAN(IP|PPP)Connection/i.test(m[1])) {
      type = (/<serviceType>([^<]+)</i.exec(m[1]) || [])[1]
      const c = (/<controlURL>([^<]+)</i.exec(m[1]) || [])[1]
      const b = new URL(loc)
      ctrl = c.startsWith('http') ? c : b.protocol + '//' + b.host + c
      break
    }
  }
  if (!ctrl) { console.log('keine WAN-ControlURL gefunden'); return }
  try { const e = await soap(ctrl, type, 'GetExternalIPAddress'); console.log('externe IP:', (/<NewExternalIPAddress>([^<]*)</i.exec(e.body) || [])[1] || '(leer)') } catch (e) { console.log('GetExternalIP-Fehler:', e.message) }
  const inner = '<NewRemoteHost></NewRemoteHost><NewExternalPort>8787</NewExternalPort><NewProtocol>TCP</NewProtocol><NewInternalPort>8787</NewInternalPort><NewInternalClient>' + ip + '</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>Lumora Test</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>'
  try {
    const r = await soap(ctrl, type, 'AddPortMapping', inner)
    if (r.status === 200) {
      console.log('AddPortMapping: 200 OK -> Freigabe FUNKTIONIERT')
      await soap(ctrl, type, 'DeletePortMapping', '<NewRemoteHost></NewRemoteHost><NewExternalPort>8787</NewExternalPort><NewProtocol>TCP</NewProtocol>')
    } else {
      const ec = /<errorCode>([^<]+)</i.exec(r.body), ed = /<errorDescription>([^<]+)</i.exec(r.body)
      console.log('AddPortMapping: HTTP', r.status, 'FEHLER', ec ? ec[1] : '?', ed ? ed[1] : r.body.slice(0, 160))
    }
  } catch (e) { console.log('AddPortMapping-Fehler:', e.message) }
})()
