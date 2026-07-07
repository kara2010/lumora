// Prueft nach Aktivierung der FritzBox-Freigabe alle drei Wege: UPnP, NAT-PMP, PCP.
'use strict'
const http = require('http'), dgram = require('dgram'), { URL } = require('url'), os = require('os')
const LOCAL = (() => { const i = os.networkInterfaces(); for (const n of Object.keys(i)) for (const a of i[n]) if (a.family === 'IPv4' && !a.internal) return a.address; return '0.0.0.0' })()
const GW = '192.168.0.1'
function soap(cu, st, a, inner) {
  return new Promise((resolve, reject) => {
    const u = new URL(cu)
    const xml = `<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:${a} xmlns:u="${st}">${inner || ''}</u:${a}></s:Body></s:Envelope>`
    const req = http.request({ host: u.hostname, port: u.port, path: u.pathname, method: 'POST', headers: { 'Content-Type': 'text/xml; charset="utf-8"', 'SOAPAction': `"${st}#${a}"`, 'Content-Length': Buffer.byteLength(xml) }, timeout: 4000 }, res => { let d = ''; res.on('data', c => d += c); res.on('end', () => resolve({ status: res.statusCode, body: d })) })
    req.on('error', reject); req.on('timeout', () => { req.destroy(); reject(new Error('timeout')) }); req.write(xml); req.end()
  })
}
function udp(port, buf) {
  return new Promise(res => { const s = dgram.createSocket('udp4'); let done = false; const fin = r => { if (done) return; done = true; try { s.close() } catch {}; res(r) }; const t = setTimeout(() => fin(null), 2500); s.on('message', m => { clearTimeout(t); fin(m) }); s.on('error', () => { clearTimeout(t); fin(null) }); s.send(buf, port, GW, e => { if (e) { clearTimeout(t); fin(null) } }) })
}
const PMP = { 0: 'Success', 1: 'Unsupported Version', 2: 'Not Authorized/Refused', 3: 'Network Failure', 4: 'Out of resources', 5: 'Unsupported opcode' }
const PCP = { 0: 'SUCCESS', 1: 'UNSUPP_VERSION', 2: 'NOT_AUTHORIZED', 3: 'MALFORMED_REQUEST', 8: 'NO_RESOURCES' }
async function main() {
  console.log('1) UPnP AddPortMapping (IGDv1) ...')
  const st1 = 'urn:schemas-upnp-org:service:WANIPConnection:1', cu1 = 'http://192.168.0.1:49000/igdupnp/control/WANIPConn1'
  const inner = `<NewRemoteHost></NewRemoteHost><NewExternalPort>8787</NewExternalPort><NewProtocol>TCP</NewProtocol><NewInternalPort>8787</NewInternalPort><NewInternalClient>${LOCAL}</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>Lumora</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>`
  try {
    const r = await soap(cu1, st1, 'AddPortMapping', inner)
    const err = (/<errorDescription>([^<]*)<\/errorDescription>/i.exec(r.body) || [])[1]
    console.log('   -> HTTP ' + r.status + (err ? ' (' + err + ')' : ' *** ERFOLG ***'))
    if (r.status === 200) {
      const g = await soap(cu1, st1, 'GetSpecificPortMappingEntry', '<NewRemoteHost></NewRemoteHost><NewExternalPort>8787</NewExternalPort><NewProtocol>TCP</NewProtocol>')
      console.log('   verifiziert: Client=' + ((/<NewInternalClient>([^<]*)<\/NewInternalClient>/i.exec(g.body) || [])[1] || '?'))
      await soap(cu1, st1, 'DeletePortMapping', '<NewRemoteHost></NewRemoteHost><NewExternalPort>8787</NewExternalPort><NewProtocol>TCP</NewProtocol>')
      console.log('   (Test-Mapping wieder entfernt)')
    }
  } catch (e) { console.log('   -> ' + e.message) }

  console.log('2) NAT-PMP TCP-Map ...')
  const pmp = Buffer.alloc(12); pmp.writeUInt8(0, 0); pmp.writeUInt8(2, 1); pmp.writeUInt16BE(8787, 4); pmp.writeUInt16BE(8787, 6); pmp.writeUInt32BE(7200, 8)
  const pr = await udp(5351, pmp)
  if (pr) { const rc = pr.readUInt16BE(2); const ext = pr.length >= 12 ? pr.readUInt16BE(10) : 0; console.log('   -> ' + (PMP[rc] || rc) + (rc === 0 ? ' externer Port=' + ext : '')) } else console.log('   -> keine Antwort')

  console.log('3) PCP MAP ...')
  const pcp = Buffer.alloc(60); pcp.writeUInt8(2, 0); pcp.writeUInt8(1, 1); pcp.writeUInt32BE(7200, 4)
  const cr = await udp(5351, pcp)
  if (cr) { const rc = cr.readUInt8(3); console.log('   -> ' + (PCP[rc] || rc)) } else console.log('   -> keine Antwort')
}
main()
