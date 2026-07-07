# Robusterer Xref-Finder: statt Instruktionen linear zu disassemblieren (desyncanfaellig),
# durchsucht dies den GESAMTEN Adressraum nach rohen 4-Byte-Little-Endian-Werten, die
# entweder (a) der ABSOLUTEN VA des Strings entsprechen (bei "mov reg, imm32/imm64" oder
# Datentabellen mit Pointern) oder (b) als RIP-relatives Displacement zu einer nahegelegenen
# LEA-Instruktion passen wuerden. Findet Tabellen-Referenzen, die reine Instruktions-
# Disassemblierung uebersieht.
import pefile, sys, struct

path = sys.argv[1]
target = sys.argv[2] if len(sys.argv) > 2 else 'EnableFill'

pe = pefile.PE(path, fast_load=True)
image_base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()

idx = data.find(target.encode('latin1'))
if idx < 0:
    print(f'"{target}" nicht gefunden'); sys.exit(1)
str_rva = idx
str_va = image_base + str_rva
print(f'"{target}" @ RVA=0x{str_rva:x}  VA=0x{str_va:x}')

# 1) Absolute VA als 4-Byte (32-bit Pointer-Tabelle, untypisch fuer x64 aber pruefen)
needle32 = struct.pack('<I', str_va & 0xffffffff)
# 2) Absolute VA als 8-Byte (typisch fuer x64 Pointer-Arrays: "const char* names[] = {...}")
needle64 = struct.pack('<Q', str_va)

def find_all(buf, needle):
    out = []
    start = 0
    while True:
        i = buf.find(needle, start)
        if i < 0: break
        out.append(i)
        start = i + 1
    return out

hits32 = find_all(data, needle32)
hits64 = find_all(data, needle64)
print(f'Absolute-VA-Pointer (32-bit) gefunden bei RVA: {[hex(x) for x in hits32]}')
print(f'Absolute-VA-Pointer (64-bit) gefunden bei RVA: {[hex(x) for x in hits64]}')

# Fuer jeden 64-bit-Pointer-Treffer: welche Section, und was steht direkt davor/danach
# (haeufig ein Array von { const char* name; DWORD offset/id; } Eintraegen)
for h in hits64:
    sec = None
    for s in pe.sections:
        if s.VirtualAddress <= h < s.VirtualAddress + s.Misc_VirtualSize:
            sec = s.Name.decode('latin1').rstrip('\x00')
            break
    print(f'\n-- Pointer-Tabellen-Eintrag @ RVA 0x{h:x} in Section "{sec}" --')
    # 32 Bytes Kontext davor + danach als Hex + versuchte Interpretation als weitere Pointer
    ctx = data[max(0, h - 32): h + 40]
    print('  Hex:', ctx.hex())
    # Nachfolgende 8 Bytes (oft ein Begleitwert: Flag/ID/Offset/weiterer Pointer)
    for off in range(-16, 24, 8):
        chunk = data[h + off: h + off + 8]
        if len(chunk) == 8:
            val = struct.unpack('<Q', chunk)[0]
            tag = '  <== STRING-PTR' if off == 0 else ''
            print(f'  RVA+{off:+3d}: 0x{val:016x}{tag}')
