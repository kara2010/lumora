// Robuster UPnP-IGD-Client (reines Node: dgram + http, keine Abhaengigkeit).
// 1) SSDP-Discovery – korrekt an das lokale Interface gebunden, mehrere Suchtypen.
// 2) Device-Description (LOCATION) laden, WAN*Connection-Service + controlURL finden.
// 3) GetExternalIPAddress aufrufen = Beweis, dass wir Portfreigaben steuern koennen.
// Damit ist geklaert, ob Lumora den Port spaeter automatisch oeffnen kann.
'use strict'
const dgram = require('dgram')
const http = require('http')
const os = require('os')
const { URL } = require('url')

function localIPv4() {
  const ifs = os.networkInterfaces()
  for (const n of Object.keys(ifs)) for (const a of ifs[n]) {
    if (a.family === 'IPv4' && !a.internal) return a.address
  }
  return '0.0.0.0'
}
const LOCAL = localIPv4()
console.log('Binde an lokales Interface:', LOCAL)

const STs = [
  'urn:schemas-upnp-org:device:InternetGatewayDevice:1',
  'urn:schemas-upnp-org:service:WANIPConnection:1',
  'urn:schemas-upnp-org:service:WANPPPConnection:1',
  'ssdp:all',
]

function discover() {
  return new Promise((resolve) => {
    const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true })
    const locations = new Map()   // location -> from
    sock.on('message', (buf, rinfo) => {
      const s = buf.toString('latin1')
      const loc = /LOCATION:\s*(\S+)/i.exec(s)
      const st = /\bST:\s*(\S+)/i.exec(s)
      if (loc && !locations.has(loc[1])) {
        locations.set(loc[1], rinfo.address)
        console.log('  SSDP-Antwort von', rinfo.address, '->', loc[1], st ? '(' + st[1] + ')' : '')
      }
    })
    sock.on('error', (e) => console.log('  SSDP-Socketfehler:', e.message))
    sock.bind(0, LOCAL, () => {
      try { sock.setMulticastTTL(4) } catch {}
      try { sock.setMulticastInterface(LOCAL) } catch {}
      for (const st of STs) {
        const m = Buffer.from(
          'M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n' +
          'MAN: "ssdp:discover"\r\nMX: 2\r\nST: ' + st + '\r\n\r\n')
        sock.send(m, 1900, '239.255.255.250')
      }
    })
    setTimeout(() => { try { sock.close() } catch {}; resolve([...locations.keys()]) }, 5000)
  })
}

function httpGet(u) {
  return new Promise((resolve, reject) => {
    const req = http.get(u, { timeout: 4000 }, (res) => {
      let d = ''
      res.on('data', c => d += c); res.on('end', () => resolve({ status: res.statusCode, body: d }))
    })
    req.on('error', reject); req.on('timeout', () => { req.destroy(); reject(new Error('timeout')) })
  })
}

function soap(controlURL, serviceType, action, bodyInner) {
  return new Promise((resolve, reject) => {
    const u = new URL(controlURL)
    const xml = `<?xml version="1.0"?>` +
      `<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">` +
      `<s:Body><u:${action} xmlns:u="${serviceType}">${bodyInner || ''}</u:${action}></s:Body></s:Envelope>`
    const req = http.request({
      host: u.hostname, port: u.port, path: u.pathname, method: 'POST',
      headers: {
        'Content-Type': 'text/xml; charset="utf-8"',
        'SOAPAction': `"${serviceType}#${action}"`,
        'Content-Length': Buffer.byteLength(xml),
      }, timeout: 4000,
    }, (res) => { let d = ''; res.on('data', c => d += c); res.on('end', () => resolve({ status: res.statusCode, body: d })) })
    req.on('error', reject); req.on('timeout', () => { req.destroy(); reject(new Error('timeout')) })
    req.write(xml); req.end()
  })
}

;(async () => {
  console.log('=== SSDP-Discovery (5 s) ===')
  const locs = await discover()
  if (!locs.length) {
    console.log('\nKein UPnP-Geraet gefunden. UPnP im Router vermutlich AUS')
    console.log('(oder Windows-Firewall blockt die Multicast-Antwort).')
    return
  }
  console.log('\n=== Device-Descriptions pruefen ===')
  for (const loc of locs) {
    let desc
    try { desc = await httpGet(loc) } catch (e) { console.log('  ', loc, 'nicht ladbar:', e.message); continue }
    if (!/InternetGatewayDevice/i.test(desc.body)) continue
    console.log('  IGD gefunden:', loc)
    // WAN*Connection-Service + controlURL herausziehen
    const svcRe = /<service>([\s\S]*?)<\/service>/g
    let m, svc = null
    while ((m = svcRe.exec(desc.body))) {
      const block = m[1]
      if (/WAN(IP|PPP)Connection/i.test(block)) {
        const type = /<serviceType>([^<]+)<\/serviceType>/i.exec(block)?.[1]
        const ctrl = /<controlURL>([^<]+)<\/controlURL>/i.exec(block)?.[1]
        if (type && ctrl) { svc = { type, ctrl }; break }
      }
    }
    if (!svc) { console.log('  (kein WAN-Connection-Service in der Beschreibung)'); continue }
    const base = new URL(loc)
    const controlURL = svc.ctrl.startsWith('http') ? svc.ctrl : `${base.protocol}//${base.host}${svc.ctrl.startsWith('/') ? '' : '/'}${svc.ctrl}`
    console.log('  Service:', svc.type)
    console.log('  Control-URL:', controlURL)
    try {
      const r = await soap(controlURL, svc.type, 'GetExternalIPAddress')
      const ip = /<NewExternalIPAddress>([^<]*)<\/NewExternalIPAddress>/i.exec(r.body)?.[1]
      console.log('  GetExternalIPAddress -> HTTP', r.status, ip ? '= ' + ip : '(keine IP in Antwort)')
      if (ip) {
        console.log('\n*** UPnP STEUERBAR *** Lumora kann den Port automatisch oeffnen.')
        return
      }
    } catch (e) { console.log('  SOAP-Aufruf fehlgeschlagen:', e.message) }
  }
  console.log('\nUPnP-Geraet(e) da, aber keine steuerbare WAN-Verbindung gefunden.')
})()
