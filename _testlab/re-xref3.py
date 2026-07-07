# Disassembliert gezielt einen Bereich um bekannte RVAs herum (statt linear ab
# Sektionsanfang - das umgeht das Alignment-Desync-Problem, weil wir hier bewusst
# etwas VOR dem Treffer beginnen und der Bytestrom sich innerhalb weniger Bytes
# nach einem Funktionsanfang/Basisblock realignt).
import pefile, capstone, sys, re

path = sys.argv[1]
rvas = [int(x, 16) for x in sys.argv[2:]]

pe = pefile.PE(path, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
image_base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()

thunk_name = {}
for entry in pe.DIRECTORY_ENTRY_IMPORT:
    dll = entry.dll.decode('latin1')
    for imp in entry.imports:
        if imp.name:
            thunk_name[imp.address - image_base] = dll + '!' + imp.name.decode('latin1')

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.skipdata = True

for rva in rvas:
    print(f'\n########## Kontext um RVA 0x{rva:x} ##########')
    start = max(0, rva - 64)
    window = data[start: rva + 200]
    for insn in md.disasm(window, start):
        extra = ''
        if insn.mnemonic == 'call':
            m = re.match(r'^(?:qword ptr )?\[rip\s*([+-])\s*0x([0-9a-fA-F]+)\]$', insn.op_str)
            if m:
                sign = 1 if m.group(1) == '+' else -1
                iat_rva = insn.address + insn.size + int(m.group(2), 16) * sign
                if iat_rva in thunk_name:
                    extra = '   ; ' + thunk_name[iat_rva]
                else:
                    extra = f'   ; IAT-Slot RVA 0x{iat_rva:x} (nicht im Import-Table)'
            else:
                try:
                    tgt = int(insn.op_str, 16)
                    extra = f'   ; interner Call -> RVA 0x{tgt:x}'
                except ValueError:
                    pass
        marker = '  <=== HIER' if insn.address <= rva < insn.address + insn.size else ''
        print(f'  0x{insn.address:06x}: {insn.mnemonic} {insn.op_str}{extra}{marker}')
