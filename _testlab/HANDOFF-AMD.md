# Übergabe: AMD-Streaming-Diagnose

## Stand
Lumora **2.2.3**. Streaming (FFmpeg + mediamtx → WHEP) ist auf **NVIDIA (NVENC)** voll verifiziert:
Monitor-Weg (ddagrab) UND Fenster-Weg (WGC-Helfer), beide mit Ton, „2 tracks (H264, Opus)", stabil.
FFmpeg ist bewusst LGPL (kein libx264). Encoder-Wahl GPU-agnostisch: NVENC/AMF/QSV.

## Das AMD-Problem (zu diagnostizieren)
- **Ganzer Bildschirm (Monitor/ddagrab) → Stream bleibt SCHWARZ.**
- **Fenster → Stream „kommt nicht auf die Beine".**

## Zwei Verdachtsrichtungen
1. **Schwarz (ddagrab):** vermutlich **HDR-Desktop**. Der Monitor-Weg hat als Einziger KEIN
   HDR→SDR-Tonemapping (nur der Fenster-Weg via ACES-Shader in `capture/Program.cs`). Bei aktivem
   Windows-HDR liefert ddagrab HDR-Frames → als SDR schwarz/ausgewaschen. **Gegenprobe: Windows-HDR
   aus → wird das Bild hell?** Alternativ ddagrab-Adapter bei Multi-GPU (output_idx ↔ GPU-Zuordnung).
2. **Fenster kommt nicht hoch (h264_amf):** die AMF-Parameter in `main.js` → `bcEncoderArgs`
   (`-usage lowlatency -rc cbr -b:v … -bufsize … -g 120 -bf 0`) isoliert gegen die echte AMD testen.

## Erste Schritte in der AMD-Session (Fakten sammeln, nicht raten)
1. AMD-Encoder da? `bin/ffmpeg.exe -hide_banner -encoders | grep amf` (soll `h264_amf` zeigen)
2. Stream-Log: `%TEMP%\lumora-stream.log` — Zeilen `ff:`/`cap:`/`aud:`/`mtx:` (logLevel=error).
3. Direkter AMF-Encode-Test:
   `bin/ffmpeg.exe -f lavfi -i testsrc=size=1280x720:rate=60 -t 3 -c:v h264_amf -usage lowlatency -rc cbr -b:v 8M -f null -`
4. ddagrab-Test inkl. HDR-Frage:
   `bin/ffmpeg.exe -filter_complex "ddagrab=output_idx=0:framerate=60,hwdownload,format=bgra,format=yuv420p" -c:v h264_amf -usage lowlatency -b:v 8M -t 5 out.mp4` → `out.mp4` ansehen (schwarz?).
   Dann Windows-HDR AUS und wiederholen.
5. Volle Kette: `scratchpad/mon.js` (Monitor) / `brk.js` (Fenster) aus früheren Sessions — Encoder-Args
   auf `h264_amf` umstellen. Beweiszeile: mediamtx „stream is available and online, 2 tracks".

## Setup auf dem AMD-PC
- `git pull` (Branch **feature/osd**) holt Code + `capture/`-Source + Lizenztexte.
- **Binaries sind NICHT im Repo** (.gitignore, teils >100 MB) — separat besorgen:
  - `ffmpeg.exe` (LGPL): github.com/BtbN/FFmpeg-Builds → `ffmpeg-master-latest-win64-lgpl.zip`
  - `mediamtx.exe`: github.com/bluenviron/mediamtx/releases (v1.19.2)
  - `lumora-capture.exe`: `dotnet publish capture -c Release -r win-x64 --self-contained -p:PublishSingleFile=true -o bin`
  - Einfachste Alternative: die 3 Dateien aus einer installierten Lumora `…\resources\bin\` kopieren.

## Relevante Code-Stellen
- `main.js`: `bcDetectEncoder` (Encoder-Wahl), `bcEncoderArgs` (amf/nvenc/qsv), `bcBuildFfmpegArgs`
  (ddagrab-Filter = Monitor / rawvideo = Fenster), `bcStartFfmpeg`/`bcStartWindowCapture`.
- `capture/Program.cs`: WGC-Helfer, `DetectHdr()`, ACES-Shader-Tonemapping — **nur Fenster-Weg**.
- Kernpunkt HDR: Fenster-Weg tonemappt HDR→SDR, Monitor-Weg (ddagrab) NICHT → heißester Verdacht fürs schwarze Bild.
