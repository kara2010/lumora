// NAT-Typ-Diagnose fuer WebRTC-Streaming – reines Node dgram, KEINE Abhaengigkeit.
// Sendet STUN-Binding-Requests (RFC 5389) an mehrere oeffentliche STUN-Server und
// liest die XOR-MAPPED-ADDRESS (unsere oeffentliche IP:Port aus Sicht des Internets).
// Daraus leiten wir ab, ob direktes P2P moeglich ist:
//   - reflexive IP == lokale IP           -> oeffentliche IP direkt am PC (ideal)
//   - reflexive IP in 100.64/10           -> CGN/DS-Lite (kein direktes IPv4-P2P)
//   - Port ueber versch. Server GLEICH    -> Cone NAT (STUN-P2P klappt)
//   - Port ueber versch. Server VERSCHIED -> symmetrisches NAT (TURN noetig)
'use strict'
const dgram = require('dgram')
const os = require('os')
const crypto = require('crypto')

const SERVERS = [
  ['stun.l.google.com', 19302],
  ['stun1.l.google.com', 19302],
  ['stun.cloudflare.com', 3478],
]
const MAGIC = 0x2112a442

function localIPv4s() {
  const out = []
  const ifs = os.networkInterfaces()
  for (const name of Object.keys(ifs)) {
    for (const a of ifs[name]) {
      if (a.family === 'IPv4' && !a.internal) out.push(a.address)
    }
  }
  return out
}
function isCGN(ip) { // 100.64.0.0/10
  const p = ip.split('.').map(Number)
  return p[0] === 100 && p[1] >= 64 && p[1] <= 127
}
function isPrivate(ip) {
  const p = ip.split('.').map(Number)
  return p[0] === 10 || (p[0] === 172 && p[1] >= 16 && p[1] <= 31) || (p[0] === 192 && p[1] === 168)
}

function stunQuery(host, port) {
  return new Promise((resolve) => {
    const sock = dgram.createSocket('udp4')
    const tid = crypto.randomBytes(12)
    const req = Buffer.alloc(20)
    req.writeUInt16BE(0x0001, 0)   // Binding Request
    req.writeUInt16BE(0x0000, 2)   // Length 0
    req.writeUInt32BE(MAGIC, 4)    // Magic Cookie
    tid.copy(req, 8)
    let done = false
    const finish = (r) => { if (done) return; done = true; try { sock.close() } catch {}; resolve(r) }
    const timer = setTimeout(() => finish({ error: 'timeout' }), 4000)
    sock.on('message', (msg) => {
      clearTimeout(timer)
      try {
        // Attribute ab Offset 20 durchgehen
        let off = 20
        while (off + 4 <= msg.length) {
          const type = msg.readUInt16BE(off)
          const len = msg.readUInt16BE(off + 2)
          const val = msg.slice(off + 4, off + 4 + len)
          if (type === 0x0020 || type === 0x0001) { // XOR-MAPPED-ADDRESS / MAPPED-ADDRESS
            const xor = type === 0x0020
            const port = val.readUInt16BE(2) ^ (xor ? (MAGIC >>> 16) : 0)
            const b = Buffer.from(val.slice(4, 8))
            if (xor) { b[0] ^= 0x21; b[1] ^= 0x12; b[2] ^= 0xa4; b[3] ^= 0x42 }
            return finish({ ip: `${b[0]}.${b[1]}.${b[2]}.${b[3]}`, port })
          }
          off += 4 + len + ((4 - (len % 4)) % 4)
        }
        finish({ error: 'no-mapped-address' })
      } catch (e) { finish({ error: e.message }) }
    })
    sock.on('error', (e) => { clearTimeout(timer); finish({ error: e.message }) })
    sock.send(req, port, host, (e) => { if (e) { clearTimeout(timer); finish({ error: e.message }) } })
  })
}

;(async () => {
  const locals = localIPv4s()
  console.log('Lokale IPv4:', locals.join(', ') || '(keine)')
  console.log('Frage STUN-Server ab...\n')
  const results = []
  for (const [host, port] of SERVERS) {
    const r = await stunQuery(host, port)
    results.push({ host, ...r })
    console.log(`  ${host}:${port} -> ${r.error ? 'FEHLER: ' + r.error : r.ip + ':' + r.port}`)
  }
  const ok = results.filter(r => r.ip)
  console.log('')
  if (!ok.length) { console.log('KEINE STUN-Antwort – UDP evtl. blockiert.'); return }

  const pubIP = ok[0].ip
  const ports = [...new Set(ok.map(r => r.port))]
  const ipsSeen = [...new Set(ok.map(r => r.ip))]

  console.log('=== BEFUND ===')
  console.log('Oeffentliche IP (reflexiv):', ipsSeen.join(' / '))
  if (isCGN(pubIP)) {
    console.log('NAT-Typ: CGN / DS-Lite (Carrier-Grade-NAT, 100.64/10)')
    console.log('-> Direktes IPv4-P2P NICHT moeglich. Zuschauer erreichen den PC nur ueber IPv6 oder ein Relay.')
  } else if (isPrivate(pubIP)) {
    console.log('NAT-Typ: doppeltes NAT (reflexive Adresse ist privat)')
    console.log('-> Direktes P2P unwahrscheinlich; Relay noetig.')
  } else if (locals.includes(pubIP)) {
    console.log('NAT-Typ: KEIN NAT – oeffentliche IP direkt am PC.')
    console.log('-> Ideal: Zuschauer erreichen den PC direkt (nur Firewall-Freigabe fuer den Port).')
  } else if (ports.length === 1) {
    console.log('NAT-Typ: Cone-NAT (gleicher Port ueber alle STUN-Server:', ports[0] + ')')
    console.log('-> Direktes P2P via STUN funktioniert zuverlaessig. Kein Relay noetig.')
  } else {
    console.log('NAT-Typ: symmetrisch (Ports:', ports.join(', ') + ')')
    console.log('-> Direktes P2P schwierig; fuer manche Gegenstellen ist ein TURN-Relay noetig.')
  }

  // IPv6-Check (DS-Lite hat oft globales IPv6 -> direkter Weg, wenn Zuschauer IPv6 hat)
  const v6 = []
  const ifs = os.networkInterfaces()
  for (const n of Object.keys(ifs)) for (const a of ifs[n]) {
    if (a.family === 'IPv6' && !a.internal && !a.address.startsWith('fe80') && !a.address.startsWith('fc') && !a.address.startsWith('fd')) v6.push(a.address)
  }
  console.log('Globale IPv6:', v6.length ? v6.join(', ') : '(keine)')
})()
