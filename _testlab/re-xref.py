# Finde Code-Referenzen auf die "EnableFill"-Stringadresse (x64 RIP-relative LEA),
# disassembliere die umgebende Funktion, und suche nach Event/Message-IPC-Aufrufen
# in der Naehe (Hinweis auf einen Live-Reload-Mechanismus).
import pefile, capstone, sys

path = sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files (x86)\RivaTuner Statistics Server\RTSS.exe"
target_strings = sys.argv[2:] if len(sys.argv) > 2 else ['EnableFill']

pe = pefile.PE(path, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
image_base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()  # RVA-indiziert

# Ziel-String-RVAs finden (ASCII, nullterminiert)
target_rvas = {}
for s in target_strings:
    b = s.encode('latin1')
    start = 0
    found = []
    while True:
        idx = data.find(b, start)
        if idx < 0: break
        found.append(idx)
        start = idx + 1
    target_rvas[s] = found
    print(f'"{s}" gefunden bei RVA: {[hex(x) for x in found]}')

# Import-Thunk-RVAs (fuer Erkennung von "call [rip+disp] -> ImportName")
thunk_name = {}
for entry in pe.DIRECTORY_ENTRY_IMPORT:
    dll = entry.dll.decode('latin1')
    for imp in entry.imports:
        if imp.name:
            thunk_name[imp.address - image_base] = dll + '!' + imp.name.decode('latin1')

# .text-Sektion disassemblieren (x64)
text = None
for s in pe.sections:
    if b'.text' in s.Name:
        text = s
        break
if not text:
    print('Keine .text-Sektion gefunden'); sys.exit(1)

code = data[text.VirtualAddress: text.VirtualAddress + text.Misc_VirtualSize]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = False
md.skipdata = True   # lineares Sweep desynct sonst an Thunk-Tabellen/Padding -> ueber ungueltige Bytes hinwegspringen

print(f'\nDisassembliere .text ({len(code)} Bytes) und suche RIP-relative Referenzen auf Ziel-Strings...')
all_targets = set(r for lst in target_rvas.values() for r in lst)
hits = []
base_rva = text.VirtualAddress
count = 0
for insn in md.disasm(code, base_rva):
    count += 1
    # RIP-relative Operanden: capstone gibt bei op_str sowas wie "rcx, [rip + 0x12345]" NICHT direkt
    # die absolute Adresse - wir berechnen sie selbst falls Instruktion ein [rip+disp] Memory-Operand hat.
    if '[rip' in insn.op_str:
        # naive Extraktion des rip-relativen Displacements aus dem Bytecode ist fehleranfaellig;
        # nutze stattdessen: Ziel = insn.address + insn.size + disp, disp aus op_str parsen
        import re
        m = re.search(r'\[rip\s*([+-])\s*0x([0-9a-fA-F]+)\]', insn.op_str)
        if m:
            sign = 1 if m.group(1) == '+' else -1
            disp = int(m.group(2), 16) * sign
            abs_target = insn.address + insn.size + disp
            if abs_target in all_targets:
                hits.append(insn)

print(f'{count} Instruktionen verarbeitet, {len(hits)} String-Referenzen gefunden.\n')
for h in hits:
    print(f'=== Referenz bei RVA 0x{h.address:06x}: {h.mnemonic} {h.op_str} ===')
    # Kontext: 40 Instruktionen davor und danach dumpen (Funktionsgrenzen sind ohne
    # vollen Funktions-Erkenner schwer zu finden - grosszuegiges Fenster reicht meist)
    window_start_rva = max(base_rva, h.address - 0x300)
    window_code = data[window_start_rva: h.address + 0x400]
    ctx = list(md.disasm(window_code, window_start_rva))
    for insn2 in ctx:
        marker = '  <== STRING-REF' if insn2.address == h.address else ''
        extra = ''
        if insn2.mnemonic == 'call':
            try:
                tgt = int(insn2.op_str, 16)
                if tgt in thunk_name:
                    extra = '   ; ' + thunk_name[tgt]
                elif '[rip' in insn2.op_str:
                    import re
                    m2 = re.search(r'\[rip\s*([+-])\s*0x([0-9a-fA-F]+)\]', insn2.op_str)
                    if m2:
                        sign2 = 1 if m2.group(1) == '+' else -1
                        iat_rva = insn2.address + insn2.size + int(m2.group(2), 16) * sign2
                        if iat_rva in thunk_name:
                            extra = '   ; ' + thunk_name[iat_rva]
            except ValueError:
                pass
        print(f'  0x{insn2.address:06x}: {insn2.mnemonic} {insn2.op_str}{extra}{marker}')
    print()
