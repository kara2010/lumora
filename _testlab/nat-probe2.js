// Korrigierte NAT-Diagnose: EIN Socket (gleicher lokaler Port) an mehrere
// STUN-Server. Nur so ist der externe Port vergleichbar -> echter Cone-vs-
// symmetrisch-Test. Zusaetzlich UPnP-IGD- und NAT-PMP-Discovery (Router-
// Portfreigabe automatisch moeglich?). Alles reines Node, keine Abhaengigkeit.
'use strict'
const dgram = require('dgram')
const crypto = require('crypto')
const MAGIC = 0x2112a442

const SERVERS = [
  ['stun.l.google.com', 19302],
  ['stun1.l.google.com', 19302],
  ['stun.cloudflare.com', 3478],
]

function parseMapped(msg) {
  let off = 20
  while (off + 4 <= msg.length) {
    const type = msg.readUInt16BE(off)
    const len = msg.readUInt16BE(off + 2)
    const val = msg.slice(off + 4, off + 4 + len)
    if (type === 0x0020 || type === 0x0001) {
      const xor = type === 0x0020
      const port = val.readUInt16BE(2) ^ (xor ? (MAGIC >>> 16) : 0)
      const b = Buffer.from(val.slice(4, 8))
      if (xor) { b[0] ^= 0x21; b[1] ^= 0x12; b[2] ^= 0xa4; b[3] ^= 0x42 }
      return { ip: `${b[0]}.${b[1]}.${b[2]}.${b[3]}`, port }
    }
    off += 4 + len + ((4 - (len % 4)) % 4)
  }
  return null
}

// Ein einziger Socket, nacheinander an alle Server – gleicher Quellport!
function symmetryTest() {
  return new Promise((resolve) => {
    const sock = dgram.createSocket('udp4')
    const results = []
    let idx = 0
    sock.bind(() => { console.log('Lokaler Quellport (fest):', sock.address().port); next() })
    const tid = () => crypto.randomBytes(12)
    function send() {
      const req = Buffer.alloc(20)
      req.writeUInt16BE(0x0001, 0); req.writeUInt16BE(0, 2); req.writeUInt32BE(MAGIC, 4); tid().copy(req, 8)
      sock.send(req, SERVERS[idx][1], SERVERS[idx][0])
    }
    let timer
    function next() {
      if (idx >= SERVERS.length) { try { sock.close() } catch {}; return resolve(results) }
      send()
      timer = setTimeout(() => { results.push({ host: SERVERS[idx][0], error: 'timeout' }); idx++; next() }, 4000)
    }
    sock.on('message', (msg) => {
      clearTimeout(timer)
      const m = parseMapped(msg)
      results.push({ host: SERVERS[idx][0], ...(m || { error: 'no-addr' }) })
      console.log(`  ${SERVERS[idx][0]} -> ${m ? m.ip + ':' + m.port : 'FEHLER'}`)
      idx++; next()
    })
    sock.on('error', () => { clearTimeout(timer); results.push({ host: SERVERS[idx][0], error: 'sock' }); idx++; next() })
  })
}

// SSDP (UPnP-IGD) Discovery per Multicast – findet ein Internet-Gateway-Device.
function upnpDiscover() {
  return new Promise((resolve) => {
    const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true })
    const msg = Buffer.from(
      'M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n' +
      'MAN: "ssdp:discover"\r\nMX: 2\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n')
    const found = []
    sock.on('message', (buf, rinfo) => {
      const s = buf.toString('latin1')
      const loc = /LOCATION:\s*(\S+)/i.exec(s)
      found.push({ from: rinfo.address, location: loc ? loc[1] : '(keine)' })
    })
    sock.on('error', () => {})
    sock.bind(() => { try { sock.setBroadcast(true) } catch {}; sock.send(msg, 1900, '239.255.255.250') })
    setTimeout(() => { try { sock.close() } catch {}; resolve(found) }, 3000)
  })
}

// NAT-PMP (Apple/Fritz teils): Public-Address-Request an das Default-Gateway.
// Wir kennen das Gateway nicht direkt; Standard-Gateway .1 im lokalen /24 testen.
function natpmp(gateway) {
  return new Promise((resolve) => {
    const sock = dgram.createSocket('udp4')
    const req = Buffer.from([0, 0]) // version 0, op 0 = public address request
    let done = false
    const fin = (r) => { if (done) return; done = true; try { sock.close() } catch {}; resolve(r) }
    const t = setTimeout(() => fin(null), 2000)
    sock.on('message', (buf) => {
      clearTimeout(t)
      if (buf.length >= 12 && buf[0] === 0 && buf[1] === 128) {
        fin(`${buf[8]}.${buf[9]}.${buf[10]}.${buf[11]}`)
      } else fin('antwort-unklar')
    })
    sock.on('error', () => { clearTimeout(t); fin(null) })
    sock.send(req, 5351, gateway, (e) => { if (e) { clearTimeout(t); fin(null) } })
  })
}

;(async () => {
  console.log('=== NAT-Symmetrie (gleicher Socket an mehrere STUN-Server) ===')
  const r = await symmetryTest()
  const ok = r.filter(x => x.port)
  const ports = [...new Set(ok.map(x => x.port))]
  console.log('')
  if (ports.length === 1) {
    console.log('ERGEBNIS: Cone-NAT – externer Port konstant (' + ports[0] + ').')
    console.log('-> Direktes WebRTC-P2P via STUN funktioniert. KEIN Relay noetig.')
  } else if (ports.length) {
    console.log('ERGEBNIS: symmetrisches NAT – externer Port variiert (' + ports.join(', ') + ').')
    console.log('-> STUN-P2P unzuverlaessig; Portfreigabe oder Relay noetig.')
  } else {
    console.log('ERGEBNIS: keine STUN-Antwort.')
  }

  console.log('\n=== UPnP-IGD (automatische Portfreigabe?) ===')
  const igd = await upnpDiscover()
  if (igd.length) igd.forEach(d => console.log('  Gateway:', d.from, '->', d.location))
  else console.log('  Kein UPnP-IGD gefunden (evtl. im Router deaktiviert).')

  console.log('\n=== NAT-PMP (Gateway .1) ===')
  const gw = (require('os').networkInterfaces()['Ethernet'] || []).find?.(a => a.family === 'IPv4')
  const guessGw = '192.168.0.1'
  const pmp = await natpmp(guessGw)
  console.log('  ' + guessGw + ':', pmp || 'keine Antwort')
})()
