# Strings + Import-Tabelle von RTSS.exe untersuchen: wo taucht "Fill" auf, und
# welche IPC-relevanten WinAPI-Funktionen (Event/Pipe/Message) importiert die exe?
import pefile, re, sys

path = sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files (x86)\RivaTuner Statistics Server\RTSS.exe"
pe = pefile.PE(path, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])

data = pe.get_memory_mapped_image()
image_base_rva = 0  # get_memory_mapped_image() ist RVA-indiziert (ab 0 = ImageBase)

def find_strings(pattern_bytes_re, min_len, label):
    print(f"\n=== {label} ===")
    hits = []
    for m in re.finditer(pattern_bytes_re, data):
        s = m.group(0)
        try:
            txt = s.decode('latin1') if label == 'ASCII' else s.decode('utf-16le')
        except Exception:
            continue
        if len(txt) >= min_len:
            hits.append((m.start(), txt))
    return hits

# ASCII-Strings (>=4 druckbare Zeichen)
ascii_hits = find_strings(rb'[\x20-\x7e]{4,}', 4, 'ASCII')
# UTF-16LE-Strings (>=4 Zeichen, jedes Zeichen 1 druckbares Byte + 0x00)
u16_hits = find_strings(rb'(?:[\x20-\x7e]\x00){4,}', 4, 'UTF16')

print(f"ASCII-Strings gesamt: {len(ascii_hits)}  |  UTF16-Strings gesamt: {len(u16_hits)}")

pat = re.compile(r'fill', re.IGNORECASE)
print("\n--- Treffer 'Fill' (ASCII) ---")
for rva, txt in ascii_hits:
    if pat.search(txt):
        print(f"  RVA=0x{rva:06x}  {txt!r}")

print("\n--- Treffer 'Fill' (UTF16) ---")
for rva, txt in u16_hits:
    if pat.search(txt):
        print(f"  RVA=0x{rva:06x}  {txt!r}")

print("\n=== Imports (nur Event/Pipe/Message/File-relevante) ===")
interesting = re.compile(r'Event|Pipe|Message|WriteFile|ReadFile|Mutex|Semaphore|FileMapping|Broadcast', re.IGNORECASE)
for entry in pe.DIRECTORY_ENTRY_IMPORT:
    dll = entry.dll.decode('latin1')
    for imp in entry.imports:
        if imp.name and interesting.search(imp.name.decode('latin1')):
            print(f"  {dll}!{imp.name.decode('latin1')}  thunkRVA=0x{imp.address - pe.OPTIONAL_HEADER.ImageBase:06x}" if hasattr(imp, 'address') else f"  {dll}!{imp.name.decode('latin1')}")
