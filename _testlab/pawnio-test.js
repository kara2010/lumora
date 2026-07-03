// PawnIO-Sensortest fuer AMD Zen (Ryzen 9 7950X3D, Family 19h)
// Liest CPU-Temperatur (SMN THM_TCON_CUR_TMP), Package-Power (RAPL-MSRs) und
// effektiven Takt (MPERF/APERF) ueber das offizielle PawnIO-Modul AMDFamily17.bin.
//
// Voraussetzungen:
//   1. PawnIO installiert (Official Edition, signiert) -> %ProgramFiles%\PawnIO
//   2. Modul-Blobs entpackt in _testlab\pawnio-modules\ (release_0_2_9.zip)
//   3. Vermutlich Adminrechte noetig -> bei "Zugriff verweigert" aus einer
//      Admin-Eingabeaufforderung starten:  node pawnio-test.js
//
// Ausgabe: Rohwerte + berechnete Werte. KEINE Schreibzugriffe (nur read_msr/read_smn).

'use strict'
const fs = require('fs')
const path = require('path')
const os = require('os')
const koffi = require(path.join(__dirname, '..', 'node_modules', 'koffi'))

const hex = (v) => '0x' + (BigInt.asUintN(64, BigInt(v))).toString(16)
const hr = (v) => '0x' + ((v >>> 0).toString(16).padStart(8, '0'))

// ---------- PawnIOLib laden ----------
const libPath = path.join(process.env.ProgramFiles || 'C:/Program Files', 'PawnIO', 'PawnIOLib.dll')
if (!fs.existsSync(libPath)) {
  console.error('PawnIOLib.dll nicht gefunden:', libPath)
  console.error('-> PawnIO ist nicht installiert. Setup: _testlab\\PawnIO_setup.exe -install -silent')
  process.exit(1)
}
const lib = koffi.load(libPath)
const pawnioVersion = lib.func('long __stdcall pawnio_version(_Out_ uint32* version)')
const pawnioOpen = lib.func('long __stdcall pawnio_open(_Out_ void** handle)')
const pawnioLoad = lib.func('long __stdcall pawnio_load(void* h, void* blob, size_t size)')
const pawnioExecute = lib.func('long __stdcall pawnio_execute(void* h, str name, void* inBuf, size_t inCount, void* outBuf, size_t outCount, _Out_ size_t* retCount)')
const pawnioClose = lib.func('long __stdcall pawnio_close(void* h)')

// ---------- Hilfsfunktionen ----------
let H = null
function exec1(name, inVal) {
  // 1 Eingabe-Qword -> 1 Ausgabe-Qword (ioctl_read_msr / ioctl_read_smn)
  const inBuf = Buffer.alloc(8); inBuf.writeBigUInt64LE(BigInt.asUintN(64, BigInt(inVal)))
  const outBuf = Buffer.alloc(8)
  const ret = [0]
  const r = pawnioExecute(H, name, inBuf, 1, outBuf, 1, ret)
  if (r !== 0) return { err: r }
  return { val: outBuf.readBigUInt64LE(0) }
}
const readMsr = (msr) => exec1('ioctl_read_msr', msr)
const readSmn = (addr) => exec1('ioctl_read_smn', addr)

function main() {
  // Version + Handle
  const ver = [0]
  console.log('pawnio_version:', hr(pawnioVersion(ver)), '-> v' + (ver[0] >>> 16) + '.' + ((ver[0] >>> 8) & 0xff) + '.' + (ver[0] & 0xff))
  const h = [null]
  const rOpen = pawnioOpen(h)
  if (rOpen !== 0) {
    console.error('pawnio_open FEHLER:', hr(rOpen), rOpen === -2147024891 ? '(E_ACCESSDENIED -> bitte als Administrator ausfuehren)' : '')
    process.exit(1)
  }
  H = h[0]
  console.log('pawnio_open: OK')

  // Modul laden
  const blobPath = path.join(__dirname, 'pawnio-modules', 'AMDFamily17.bin')
  const blob = fs.readFileSync(blobPath)
  const rLoad = pawnioLoad(H, blob, blob.length)
  if (rLoad !== 0) {
    console.error('pawnio_load FEHLER:', hr(rLoad), '(Modul abgelehnt? Familie nicht 17h-1Ah? Signatur?)')
    pawnioClose(H); process.exit(1)
  }
  console.log('pawnio_load AMDFamily17.bin: OK  (' + blob.length + ' Bytes)')
  console.log('CPU:', os.cpus()[0].model, '| Basistakt lt. OS:', os.cpus()[0].speed, 'MHz')
  console.log('')

  // ---------- 1) Temperatur: SMN THM_TCON_CUR_TMP @ 0x00059800 ----------
  const smn = readSmn(0x00059800)
  if (smn.err != null) {
    console.log('SMN 0x59800 FEHLER:', hr(smn.err))
  } else {
    const raw = Number(smn.val & 0xffffffffn)
    const curTemp = (raw >>> 21) * 0.125            // Bits 31:21, 0.125 K Schritte
    const rangeSel = (raw & 0x80000) !== 0          // Bit 19: -49 C Range
    const tctl = rangeSel ? curTemp - 49 : curTemp
    console.log('THM_TCON_CUR_TMP roh:', hex(raw), '| CUR_TEMP=' + curTemp.toFixed(1), '| RANGE_SEL=' + rangeSel)
    console.log('  => Tctl:', tctl.toFixed(1), '°C')
  }
  // CCD-Temperaturen: Zen3 @ 0x00059954+i*4, Zen4/Raphael @ 0x00059b08+i*4
  // (roh: bits 0-11, Temp = x/8 - 49)
  for (const [label, base] of [['Zen3-Bereich 0x59954', 0x00059954], ['Zen4-Bereich 0x59b08', 0x00059b08]]) {
    const ccds = []
    for (let i = 0; i < 8; i++) {
      const r = readSmn(base + i * 4)
      if (r.err != null) continue
      const raw = Number(r.val & 0xffffffffn)
      if ((raw & 0xfff) !== 0) ccds.push('CCD' + i + '=' + (((raw & 0xfff) / 8) - 49).toFixed(1))
    }
    if (ccds.length) console.log('  CCD-Temps (' + label + '):', ccds.join('  '))
  }
  console.log('')

  // ---------- 2) Package-Power: RAPL (PWR_UNIT 0xC0010299, PKG_ENERGY 0xC001029B) ----------
  const unit = readMsr(0xC0010299)
  if (unit.err != null) {
    console.log('MSR PWR_UNIT FEHLER:', hr(unit.err))
    finish(); return
  }
  const esu = Number((unit.val >> 8n) & 0x1fn)
  const jouleUnit = 1 / Math.pow(2, esu)
  console.log('RAPL_PWR_UNIT roh:', hex(unit.val), '| ESU=' + esu, '->', jouleUnit.toExponential(3), 'J/Tick')
  const e1 = readMsr(0xC001029B)
  // Modul-Allowlist erlaubt nur AMDs Read-Only-Aliase MPERF_RO/APERF_RO
  // (0xC00000E7/E8), NICHT die architektonischen 0xE7/0xE8.
  const c1 = readMsr(0xC00000E7), a1 = readMsr(0xC00000E8)
  const t1 = process.hrtime.bigint()
  setTimeout(() => {
    const e2 = readMsr(0xC001029B)
    const c2 = readMsr(0xC00000E7), a2 = readMsr(0xC00000E8)
    const t2 = process.hrtime.bigint()
    const dt = Number(t2 - t1) / 1e9
    if (e1.err == null && e2.err == null) {
      let dE = Number(BigInt.asUintN(32, e2.val) - BigInt.asUintN(32, e1.val))
      if (dE < 0) dE += 0x100000000                  // 32-bit Wrap
      const watts = dE * jouleUnit / dt
      console.log('PKG_ENERGY delta:', dE, 'Ticks in', dt.toFixed(3), 's')
      console.log('  => CPU Package Power:', watts.toFixed(1), 'W')
    }
    // ---------- 3) Effektiver Takt: APERF/MPERF ----------
    if (c1.err == null && c2.err == null && a1.err == null && a2.err == null) {
      const dM = Number(BigInt.asUintN(64, c2.val - c1.val))
      const dA = Number(BigInt.asUintN(64, a2.val - a1.val))
      const base = os.cpus()[0].speed
      if (dM > 0) console.log('APERF/MPERF:', (dA / dM).toFixed(3), '=> eff. Takt ~', Math.round(base * dA / dM), 'MHz (Kern des ioctl-Aufrufs)')
    }
    finish()
  }, 1000)
}
function finish() {
  pawnioClose(H)
  console.log('\npawnio_close: OK — Test fertig.')
}
main()
