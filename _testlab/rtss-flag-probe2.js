// Sicherer, reversibler Test: Bit 8 (0x100) ist neben Bit 0 (OSD_VISIBLE) im
// Flags-Wert gesetzt (0x101) – unbekannte Bedeutung. Kandidat fuer "Fill".
// Schaltet es 15 s aus, damit man den Bildschirm live pruefen kann, stellt
// danach GARANTIERT den Ausgangszustand wieder her.
'use strict'
const path = require('path')
const koffi = require(path.join(__dirname, '..', 'node_modules', 'koffi'))
const lib = koffi.load('C:/Program Files (x86)/RivaTuner Statistics Server/RTSSHooks64.dll')
const getFlags = lib.func('uint32 __cdecl GetFlags()')
const setFlags = lib.func('void __cdecl SetFlags(uint32 dwAND, uint32 dwXOR)')

const before = getFlags()
console.log('Flags vorher: 0x' + before.toString(16).padStart(8, '0'))
const BIT8 = 0x100
if (!(before & BIT8)) { console.log('Bit 8 ist schon aus – nichts zu testen.'); process.exit(0) }

setFlags((~BIT8) >>> 0, 0)   // Bit 8 loeschen, Rest unangetastet
console.log('Bit 8 AUS -> Flags jetzt: 0x' + getFlags().toString(16).padStart(8, '0'))
console.log('\n>>> JETZT 15 SEKUNDEN AUF DEN BILDSCHIRM SCHAUEN – verschwindet der Hintergrund-Balken? <<<\n')

setTimeout(() => {
  // Bit 8 war vor dem Test gesetzt und ist jetzt 0 -> XOR mit BIT8 kippt es
  // exakt zurueck auf 1, alle anderen Bits bleiben unangetastet.
  setFlags(0xffffffff >>> 0, BIT8)
  console.log('Wiederhergestellt -> Flags: 0x' + getFlags().toString(16).padStart(8, '0') + ' (Soll: 0x' + before.toString(16).padStart(8, '0') + ')')
}, 15000)
