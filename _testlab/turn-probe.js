// TURN-Server faktisch testen: macht einen echten Allocate (RFC 5766, UDP) und
// zeigt, ob der Server eine Relay-Adresse herausgibt. So laesst sich VOR dem
// Lumora-Test trennen: liegt es am Relay-Server oder an uns?
//
//   node _testlab/turn-probe.js host:port [user] [pass]
//   node _testlab/turn-probe.js turn.example.com:3478 meinuser meinpass
//   node _testlab/turn-probe.js turn.example.com:3478          # ohne Auth
//
// Ergebnis:
//   "RELAY OK -> a.b.c.d:port"  = Server relayt -> in Lumora eintragbar
//   "Fehler 401"               = Server lebt, aber Zugangsdaten falsch
//   "Timeout"                  = Server tot / Port zu / UDP geblockt
const dgram = require('dgram'), crypto = require('crypto')
const target = process.argv[2] || ''
const user = process.argv[3] || '', pass = process.argv[4] || ''
const [host, portStr] = target.split(':')
const port = Number(portStr) || 3478
if (!host) { console.log('Aufruf: node _testlab/turn-probe.js host:port [user] [pass]'); process.exit(1) }

function bld(type, txid, attrs, key) {
  let b = Buffer.concat(attrs.map(function (a) { const v = a[1], p = (4 - v.length % 4) % 4, h = Buffer.alloc(4); h.writeUInt16BE(a[0], 0); h.writeUInt16BE(v.length, 2); return Buffer.concat([h, v, Buffer.alloc(p)]) }))
  if (key) { const hd = Buffer.alloc(20); hd.writeUInt16BE(type, 0); hd.writeUInt16BE(b.length + 24, 2); hd.writeUInt32BE(0x2112A442, 4); txid.copy(hd, 8); const m = crypto.createHmac('sha1', key).update(Buffer.concat([hd, b])).digest(); const mh = Buffer.alloc(4); mh.writeUInt16BE(8, 0); mh.writeUInt16BE(20, 2); b = Buffer.concat([b, mh, m]) }
  const hd = Buffer.alloc(20); hd.writeUInt16BE(type, 0); hd.writeUInt16BE(b.length, 2); hd.writeUInt32BE(0x2112A442, 4); txid.copy(hd, 8); return Buffer.concat([hd, b])
}
function parse(m) { const ty = m.readUInt16BE(0), ln = m.readUInt16BE(2), a = {}; let o = 20; while (o + 4 <= 20 + ln) { const t = m.readUInt16BE(o), l = m.readUInt16BE(o + 2); a[t] = m.slice(o + 4, o + 4 + l); o += 4 + l + ((4 - l % 4) % 4) } return { type: ty, attrs: a } }
function xa(b) { if (!b) return '?'; return [b[4]^0x21, b[5]^0x12, b[6]^0xa4, b[7]^0x42].join('.') + ':' + (b.readUInt16BE(2) ^ 0x2112) }
const s = dgram.createSocket('udp4'); let st = 1, done = false
const fin = function (r) { if (done) return; done = true; clearTimeout(t); try { s.close() } catch (e) {}; console.log(host + ':' + port + '  ' + r) }
const t = setTimeout(function () { fin('⌛ Timeout (Server tot / Port zu / UDP geblockt)') }, 5000)
s.on('error', function (e) { fin('DNS/Socket-Fehler: ' + e.code) })
s.on('message', function (m) {
  const p = parse(m)
  if (st === 1) {
    if (p.type === 0x0103) return fin('✅ RELAY OK (ohne Auth) -> ' + xa(p.attrs[0x0016]))
    const rl = p.attrs[0x0014], nc = p.attrs[0x0015]
    if (!rl || !nc) { const e = p.attrs[9]; return fin('unerwartete Antwort (Code ' + (e ? e[2]*100+e[3] : '?') + ')') }
    if (!user) return fin('Server verlangt Auth (realm ' + rl.toString() + ') – user/pass angeben')
    const k = crypto.createHash('md5').update(user + ':' + rl.toString() + ':' + pass).digest()
    st = 2
    s.send(bld(3, crypto.randomBytes(12), [[0x19, Buffer.from([17,0,0,0])], [6, Buffer.from(user)], [0x14, rl], [0x15, nc]], k), port, host)
  } else {
    if (p.type === 0x0103) return fin('✅ RELAY OK -> ' + xa(p.attrs[0x0016]))
    const e = p.attrs[9]; return fin('❌ Fehler ' + (e ? e[2]*100+e[3] : '?') + ' (401=Zugangsdaten falsch)')
  }
})
s.send(bld(3, crypto.randomBytes(12), [[0x19, Buffer.from([17,0,0,0])]]), port, host)
