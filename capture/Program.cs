// lumora-capture: nimmt ein Fenster (oder einen Monitor) per Windows Graphics
// Capture auf und schreibt BGRA-Frames mit KONSTANTER Framerate nach stdout.
// Die Aufnahmegroesse geht als "SIZE <w> <h>" nach stderr, damit Lumora FFmpeg
// mit der passenden -s/-framerate startet und stdout direkt an FFmpeg pipet.
//
//   lumora-capture --hwnd <dezimal>  --fps 60
//   lumora-capture --title "Forza"   --fps 60
//
// WGC nimmt gezielt den FENSTERINHALT auf (auch im Vollbild, ohne Hooking) –
// kein Desktop, keine Benachrichtigungen. Genau das fuer die Privatsphaere.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Windows.Graphics;
using Windows.Graphics.Capture;
using Windows.Graphics.DirectX;
using Windows.Graphics.DirectX.Direct3D11;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.D3DCompiler;
using WinRT;

static class Program
{
    // --- Win32 -------------------------------------------------------------
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int size);
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr wp, IntPtr lp, uint flags, uint timeout, out IntPtr res);
    [DllImport("user32.dll")] static extern IntPtr GetClassLongPtr(IntPtr h, int idx);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    // Physische Pixel von den Fenster-APIs bekommen (sonst liefert Windows einer
    // DPI-unbewussten Konsolen-App skalierte LOGISCHE Werte, waehrend WGC
    // PHYSISCHE Texturen liefert -> der Client-Ausschnitt saesse daneben).
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    static readonly IntPtr DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = new IntPtr(-4);
    delegate bool EnumWindowsProc(IntPtr h, IntPtr p);
    // Hochaufloesenden System-Timer (1 ms) fuer praezises Frame-Timing.
    [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint p);

    // --- WinRT-Interop -----------------------------------------------------
    [DllImport("d3d11.dll", EntryPoint = "CreateDirect3D11DeviceFromDXGIDevice", SetLastError = true)]
    static extern uint CreateDirect3D11DeviceFromDXGIDevice(IntPtr dxgiDevice, out IntPtr graphicsDevice);

    [ComImport, Guid("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IGraphicsCaptureItemInterop
    {
        IntPtr CreateForWindow([In] IntPtr window, [In] ref Guid iid);
        IntPtr CreateForMonitor([In] IntPtr monitor, [In] ref Guid iid);
    }

    [ComImport, Guid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IDirect3DDxgiInterfaceAccess { IntPtr GetInterface([In] ref Guid iid); }

    static readonly Guid ID3D11Texture2D_IID = new Guid("6f15aaf2-d208-4e89-9ab4-489535d34f9c");

    // --- Zustand -----------------------------------------------------------
    static ID3D11Device _device;
    static ID3D11DeviceContext _context;
    static ID3D11Texture2D _staging;
    static ID3D11Texture2D _mipTex;
    static ID3D11ShaderResourceView _mipSrv;
    // Video-Processor (flexibler GPU-Scaler fuer krumme Faktoren, z.B. 4K->1440p)
    static ID3D11VideoDevice _vdev;
    static ID3D11VideoContext1 _vctx;
    static ID3D11VideoProcessor _vproc;
    static ID3D11VideoProcessorEnumerator _venum;
    static ID3D11Texture2D _scaleTex;
    static ID3D11VideoProcessorOutputView _voutView;
    static bool _useVp;
    static bool _vpLogged;
    // HDR-Tonemapping per eigenem Pixel-Shader (Video-Processor kann kein FP16).
    static bool _useShader;
    static ID3D11VertexShader _vs;
    static ID3D11PixelShader _ps;
    static ID3D11SamplerState _sampler;
    static ID3D11RenderTargetView _rtv;
    static ID3D11Texture2D _srcCopy;   // FP16-Kopie der WGC-Textur (mit ShaderResource-Flag)
    // HDR-Tonemap LIVE justierbar (mehrere Kurven + Helligkeit, per Steuerdatei ohne Neustart):
    static ID3D11Buffer _psParams;     // Constant Buffer {mode, exposure} fuer den Tonemap-Shader
    static int _tmMode = 0;            // 0=ACES 1=Hable 2=Reinhard 3=Linear-Clip
    static float _tmExposure = 0.3937f; // Helligkeit/Weisspunkt (0.3937 = 1/2.54 = bisheriges Verhalten)
    static string _tmCtlPath;          // %TEMP%\lumora-hdr.txt (Format: "<mode> <exposure>")
    static long _tmCtlMtime = -1;      // letzte gelesene mtime (nur bei Aenderung neu einlesen)
    static int _tmCtlCtr = 0;          // Frame-Drossel fuers Datei-Polling
    static int _width, _height;       // CLIENT-Groesse (sichtbarer Fensterinhalt)
    static int _outW, _outH;          // Ausgabe-Groesse (nach GPU-Downscale)
    // WGC liefert die FENSTER-Textur inkl. unsichtbarer Rahmenzonen (ein maximiertes
    // Fenster ragt mit dem Rahmen ueber den Monitor hinaus; z.B. 3876x2196 Textur um
    // 3840x2160 Inhalt) - und die Texturgroesse kann je Compositing-Zustand sogar
    // zwischen mit/ohne Rahmen WECHSELN. Wir schneiden deshalb IMMER den echten
    // Client-Bereich aus der Textur (Crop), bevor die Pipeline arbeitet: keine
    // Rahmenpixel im Stream, konstante Ausgabegroesse, kein Umbau bei Textur-Pingpong.
    static int _texW, _texH;          // aktuelle WGC-Texturgroesse
    static int _cropX, _cropY;        // Client-Offset innerhalb der Textur
    static bool _cropOn;              // Crop noetig? (Offset != 0 oder Textur > Client)
    static ID3D11Texture2D _cropTex;  // Zwischen-Textur in Client-Groesse
    static int _numMips;              // >0 = Mipmap-Downscale (glatte Halbierung)
    static volatile bool _running = true;

    // --- Dynamische Fenstergroesse (Letterbox) -----------------------------
    // FFmpeg braucht eine KONSTANTE Framegroesse (_outW x _outH). Aendert das
    // Fenster waehrend des Streams seine Groesse (z.B. YouTube-Video -> Vollbild),
    // meldet WGC eine neue ContentSize. Statt zu croppen skalieren wir den neuen
    // Inhalt seitenverhaeltnis-treu in ein zentriertes Rechteck des festen
    // Ausgabepuffers und lassen den Rest schwarz (Letterbox/Pillarbox). Der Stream
    // laeuft dabei ohne Unterbrechung weiter.
    static int _curW, _curH;                 // aktuelle Content-Groesse (WGC)
    static bool _resized;                    // nach 1. Groessenaenderung: Letterbox aktiv
    static int _dx, _dy, _dw, _dh;           // Ziel-Rechteck im Ausgabepuffer
    static ID3D11VideoProcessor _lbVproc;
    static ID3D11VideoProcessorEnumerator _lbVenum;
    static ID3D11Texture2D _lbScaleTex, _lbStaging, _lbSrcCopy;
    static ID3D11VideoProcessorOutputView _lbVout;
    static ID3D11RenderTargetView _lbRtv;

    // --- NV12-Ausgabe (GPU-Farbkonvertierung, -62 % Pipe-Daten) --------------
    // Alle Verarbeitungspfade enden in einer BGRA-Textur (_outW x _outH); ein
    // finaler VideoProcessorBlt wandelt sie GPU-seitig nach NV12 (BT.709
    // limited range, explizit gesetzt). FFmpeg liest dann -pixel_format nv12
    // und spart die komplette CPU-Farbkonvertierung (format=yuv420p) - auf
    // iGPUs mit geteilter Speicherbandbreite der groesste Einzelgewinn der
    // Kette. Scheitert die Initialisierung (Treiber/GPU ohne NV12-VP-Ausgang),
    // laeuft automatisch der bewaehrte BGRA-Weg (FMT bgra an Lumora).
    static bool _nv12On;
    static ID3D11Texture2D _finalTex;         // BGRA-Sammelziel aller Pfade
    static ID3D11RenderTargetView _finalRtv;  // fuers Schwarz-Clearen (Letterbox-Raender)
    static ID3D11Texture2D _nv12Tex;          // NV12-Ziel des Konvertier-Blts
    static ID3D11Texture2D _nv12Staging;      // CPU-lesbare NV12-Kopie
    static bool _nv12Primed;                  // Doppelpuffer-Readback: Staging haelt ein noch nicht abgeholtes Frame
    static ID3D11VideoProcessorEnumerator _nvVenum;
    static ID3D11VideoProcessor _nvVproc;
    static ID3D11VideoProcessorOutputView _nvVout;

    static int Main(string[] args)
    {
        try
        {
            try { timeBeginPeriod(1); } catch { }   // praezises Thread.Sleep fuer sauberes CFR
            try { SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); } catch { }   // Fenster-APIs in physischen Pixeln
            if (Array.IndexOf(args, "--audio") >= 0)
            {
                // Im Fenster-Modus wird der HWND mitgegeben, damit RunAudio nur den Ton
                // DIESES Fensters/Prozesses aufnimmt (siehe RunAudio-Kommentar unten).
                IntPtr audHwnd = IntPtr.Zero;
                for (int ai = 0; ai < args.Length; ai++) if (args[ai] == "--hwnd" && ai + 1 < args.Length) audHwnd = (IntPtr)long.Parse(args[++ai]);
                return RunAudio(audHwnd);
            }
            if (Array.IndexOf(args, "--list") >= 0) return ListWindows(); // Fenster-Liste (zuverlaessiger als Electrons desktopCapturer)
            if (Array.IndexOf(args, "--hdr-check") >= 0) return HdrCheck(); // HDR-Status je Monitor (fuer den Farb-Hinweis im Monitor-Weg)
            IntPtr hwnd = IntPtr.Zero;
            int fps = 60, maxHeight = 0, reconnectMs = 0;
            string pipeName = null;
            bool wantNv12 = false;
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--hwnd" && i + 1 < args.Length) hwnd = (IntPtr)long.Parse(args[++i]);
                else if (args[i] == "--title" && i + 1 < args.Length) hwnd = FindByTitle(args[++i]);
                else if (args[i] == "--fps" && i + 1 < args.Length) fps = int.Parse(args[++i]);
                else if (args[i] == "--max-height" && i + 1 < args.Length) maxHeight = int.Parse(args[++i]);
                else if (args[i] == "--pipe" && i + 1 < args.Length) pipeName = args[++i];
                else if (args[i] == "--nv12") wantNv12 = true;
                // --reconnect <ms>: nach FFmpeg-Ende die Pipe offen halten und bis
                // zu <ms> auf einen NEUEN FFmpeg warten, statt sich zu beenden.
                // Fuer den nahtlosen Bitrate-Neustart (Weg 1): der Helfer laeuft
                // durch, nur FFmpeg wird neu gestartet -> spart den Helfer-Kaltstart.
                // Ohne dieses Flag: Verhalten wie bisher (Pipe-Bruch = Ende).
                else if (args[i] == "--reconnect" && i + 1 < args.Length) reconnectMs = int.Parse(args[++i]);
            }
            if (hwnd == IntPtr.Zero) { Console.Error.WriteLine("ERR kein Fenster gefunden"); return 2; }
            // Video-Ausgang als Named Pipe mit GROSSEM Puffer statt Prozess-stdout.
            // Die kleine Standard-Pipe (64-KB-Puffer) ist bei 4K-Rohframes (33 MB,
            // ~2 GB/s) der belegte Engpass der ganzen Kette: prozessintern schafft
            // die Encoder-Kette 157 fps, durch die kleine Pipe 13 fps - der echte
            // Stream lief dadurch bei konstant ~55 statt 60 fps (speed=0.91x).
            // WICHTIG: Die Pipe muss VOR der SIZE-Meldung existieren - Lumora
            // startet FFmpeg direkt nach "SIZE", und dessen CreateFile schluege
            // auf einer noch nicht existenten Pipe fehl.
            System.IO.Pipes.NamedPipeServerStream vidPipe = null;
            if (pipeName != null)
            {
                try
                {
                    vidPipe = new System.IO.Pipes.NamedPipeServerStream(pipeName, System.IO.Pipes.PipeDirection.Out, 1,
                        System.IO.Pipes.PipeTransmissionMode.Byte, System.IO.Pipes.PipeOptions.None, 0, 64 * 1024 * 1024);
                }
                catch (Exception ex) { Console.Error.WriteLine("ERR Named Pipe: " + ex.Message); return 5; }
            }
            if (!GraphicsCaptureSession.IsSupported()) { Console.Error.WriteLine("ERR WGC nicht unterstuetzt"); return 3; }

            // D3D11-Device + WinRT-Device
            D3D11.D3D11CreateDevice(null, DriverType.Hardware, DeviceCreationFlags.BgraSupport, null, out _device).CheckError();
            _context = _device.ImmediateContext;
            IDirect3DDevice winrtDevice;
            using (var dxgi = _device.QueryInterface<IDXGIDevice>())
            {
                CreateDirect3D11DeviceFromDXGIDevice(dxgi.NativePointer, out IntPtr inspectable);
                winrtDevice = MarshalInterface<IDirect3DDevice>.FromAbi(inspectable);
                Marshal.Release(inspectable);
            }

            // Minimierte Fenster rendert Windows nicht -> WGC bekommt nur winzige/leere
            // Frames (typisch ~100x56). Vor dem Capture wiederherstellen: erst ohne
            // Fokusklau (SHOWNOACTIVATE), notfalls RESTORE. Danach kurz warten, damit
            // DWM das Fenster tatsaechlich neu zeichnet, bevor der erste Frame kommt.
            if (IsIconic(hwnd))
            {
                ShowWindow(hwnd, 4);                       // SW_SHOWNOACTIVATE
                System.Threading.Thread.Sleep(80);
                if (IsIconic(hwnd)) ShowWindow(hwnd, 9);   // SW_RESTORE (holt es notfalls in den Vordergrund)
                System.Threading.Thread.Sleep(150);
            }

            // GraphicsCaptureItem fuer das Fenster. Ist der HWND ungueltig (Fenster
            // inzwischen geschlossen), wirft CreateItemForWindow eine Exception
            // ("Value does not fall within the expected range") -> abfangen und KLAR
            // melden statt kryptisch abzustuerzen.
            GraphicsCaptureItem item;
            try { item = CreateItemForWindow(hwnd); }
            catch { Console.Error.WriteLine("ERR Fenster nicht mehr verfuegbar"); return 4; }
            if (item == null) { Console.Error.WriteLine("ERR Fenster nicht mehr verfuegbar"); return 4; }
            // Textur- und CLIENT-Groesse trennen: die Pipeline arbeitet auf dem
            // sichtbaren Inhalt (Client), nicht auf der Fenster-Textur mit Rahmen.
            _texW = item.Size.Width; _texH = item.Size.Height;
            if (_texW <= 0 || _texH <= 0) { Console.Error.WriteLine("ERR Fenstergroesse 0"); return 5; }
            if (!GetClientBox(hwnd, _texW, _texH, out _cropX, out _cropY, out int cliW, out int cliH)) { _cropX = 0; _cropY = 0; cliW = _texW; cliH = _texH; }
            _width = Math.Min(cliW, _texW - _cropX);
            _height = Math.Min(cliH, _texH - _cropY);
            if (_width <= 0 || _height <= 0) { _cropX = 0; _cropY = 0; _width = _texW; _height = _texH; }
            // H.264 verlangt GERADE Masse - ungerade Fenstergroessen (frei gezogene
            // Fenster) auf gerade abrunden (1 px Beschnitt statt Encoder-Fehler).
            _width &= ~1; _height &= ~1;
            if (_width <= 0 || _height <= 0) { Console.Error.WriteLine("ERR Fenstergroesse 0"); return 5; }

            // HDR-Quelle -> per FP16 erfassen und im eigenen Shader tonemappen.
            bool hdr = DetectHdr();
            Console.Error.WriteLine("HDR " + (hdr ? "1" : "0"));

            // Ausgabegroesse + Skalierweg bestimmen. HDR -> IMMER Video-Processor,
            // denn nur er macht das Tonemapping (scRGB->SDR); Skalierung nimmt er
            // gleich mit. SDR -> glatte Halbierung per Mipmap (schnell), krumme
            // Faktoren per Video-Processor, sonst 1:1.
            _numMips = 0; _useVp = false; _useShader = false; _outW = _width; _outH = _height;
            _curW = _width; _curH = _height;
            bool needScale = maxHeight > 0 && maxHeight < _height;
            if (needScale) { _outH = maxHeight; _outW = ((int)Math.Round(_width * (double)maxHeight / _height)) & ~1; }
            if (hdr) _useShader = true;                                          // HDR -> Shader (Tonemap + Scale)
            else if (needScale)
            {
                int tW = _width, tH = _height, m = 0;
                while (tH / 2 >= maxHeight) { tW /= 2; tH /= 2; m++; }
                // &~1: Halbierungen koennen UNGERADE Masse ergeben (z.B. 2838/2 = 1419) -
                // H.264 und NV12-Texturen verlangen gerade; die Mip-Kopie croppt das
                // ueberstehende Pixel per SrcBox (s. CopyFrameInto). Vorher lief die
                // ungerade Breite ungeprueft durch (Encoder-Fehler-Risiko, latent).
                if (tH == maxHeight) { _numMips = m; _outW = tW & ~1; _outH = tH & ~1; }   // exakt -> Mipmap
                else _useVp = true;                                              // krumm -> Video-Processor
            }

            EnsureCropTex(hdr);
            // NV12-Endstufe VOR der SIZE-Meldung initialisieren: Lumora liest FMT
            // und startet FFmpeg direkt nach SIZE mit dem passenden -pixel_format.
            if (wantNv12) TryInitNv12();
            Console.Error.WriteLine("FMT " + (_nv12On ? "nv12" : "bgra"));
            Console.Error.WriteLine($"SIZE {_outW} {_outH}");
            if (_cropOn) Console.Error.WriteLine($"CLIP {_cropX},{_cropY} {_curW}x{_curH} in {_texW}x{_texH}");
            Console.Error.Flush();

            // Staging-Textur (CPU-lesbar) in AUSGABE-Groesse.
            _staging = _device.CreateTexture2D(new Texture2DDescription
            {
                Width = (uint)_outW, Height = (uint)_outH, MipLevels = 1, ArraySize = 1,
                Format = Vortice.DXGI.Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Staging, BindFlags = BindFlags.None,
                CPUAccessFlags = CpuAccessFlags.Read, MiscFlags = ResourceOptionFlags.None,
            });
            // Mip-Textur (Content-Groesse, mit Mip-Kette) fuer den GPU-Downscale.
            if (_numMips > 0)
            {
                _mipTex = _device.CreateTexture2D(new Texture2DDescription
                {
                    Width = (uint)_width, Height = (uint)_height, MipLevels = (uint)(_numMips + 1), ArraySize = 1,
                    Format = Vortice.DXGI.Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
                    Usage = ResourceUsage.Default, BindFlags = BindFlags.ShaderResource | BindFlags.RenderTarget,
                    CPUAccessFlags = CpuAccessFlags.None, MiscFlags = ResourceOptionFlags.GenerateMips,
                });
                _mipSrv = _device.CreateShaderResourceView(_mipTex);
            }
            else if (_useShader) SetupShader();
            else if (_useVp) SetupVideoProcessor(_width, _height, _outW, _outH, hdr);

            var fmt = hdr ? DirectXPixelFormat.R16G16B16A16Float : DirectXPixelFormat.B8G8R8A8UIntNormalized;
            var framePool = Direct3D11CaptureFramePool.Create(winrtDevice, fmt, 2, new SizeInt32(_texW, _texH));
            var session = framePool.CreateCaptureSession(item);
            try { session.IsCursorCaptureEnabled = true; } catch { }
            // Gelben WGC-Aufnahme-Rahmen entfernen. Ab Win11 22000 reicht dafuer
            // IsBorderRequired=false. Auf Win10 (und teils Win11) verpufft das aber,
            // wenn die App nicht ZUVOR die Borderless-Berechtigung angefordert hat.
            // RequestAccessAsync(Borderless) laeuft still (kein Dialog); fehlt die
            // API auf aelteren Builds -> catch, dann bleibt der Rahmen wie bisher.
            try { GraphicsCaptureAccess.RequestAccessAsync(GraphicsCaptureAccessKind.Borderless).GetAwaiter().GetResult(); } catch { }
            try { session.IsBorderRequired = false; } catch { }
            item.Closed += (s, e) => { _running = false; };
            session.StartCapture();

            // Ausgabe mit KONSTANTER Framerate. Wir POLLEN den FramePool
            // (TryGetNextFrame) statt das FrameArrived-Event zu nutzen: in einer
            // Konsolen-App ohne Message-Loop/Dispatcher feuert das Event nicht,
            // Polling funktioniert dagegen zuverlaessig. Kein neues Frame ->
            // letztes erneut senden (CFR).
            // Verbindung abwarten BEVOR die Takt-Stopwatch startet: die Wartezeit
            // bis FFmpeg die Pipe oeffnet darf nicht als Takt-Rueckstand zaehlen
            // (Zaehl-Zeitbasis!). FFmpeg verbindet direkt nach seinem Start.
            System.IO.Stream stdout;
            if (vidPipe != null) { vidPipe.WaitForConnection(); stdout = vidPipe; }
            else stdout = Console.OpenStandardOutput();
            double interval = 1000.0 / fps;
            var sw = System.Diagnostics.Stopwatch.StartNew();
            double next = 0;
            double pace = 0;   // Aufhol-Drossel: fruehester naechster Schreibzeitpunkt bei Rueckstand
            byte[] frameBuf = null;
            int rszW = 0, rszH = 0; double rszSince = 0;   // Resize-Kandidat fuer die Stabilitaets-Hysterese
            // Telemetrie: alle 10 s eine VSTAT-Zeile (frische vs. duplizierte Frames +
            // aktueller Takt-Rueckstand). Macht "fuehlt sich weniger fluessig an"
            // messbar: viele dup = Quelle liefert nicht; wachsender lag = Encoder
            // kommt nicht nach (Latenz baut sich auf).
            long statFresh = 0, statDup = 0; double statLast = 0;
            double statMaxCopy = 0, statMaxWrite = 0;   // Stall-Quelle: GPU-Kopie vs. Pipe-Write
            while (_running)
            {
                // Praezise bis zum naechsten Tick warten: grob per Sleep(1) (dank
                // timeBeginPeriod ~1 ms genau), Feinschliff per SpinWait -> sauberes
                // CFR auch bei 120 fps (8 ms Takt) statt grobem 15-ms-Sleep-Jitter.
                double now;
                double waitUntil = Math.Max(next, pace);
                while ((now = sw.Elapsed.TotalMilliseconds) < waitUntil - 0.3)
                {
                    if (waitUntil - now > 1.5) Thread.Sleep(1); else Thread.SpinWait(200);
                }
                next += interval;
                // AUFHOL-DROSSEL: Bei Rueckstand (Encoder-Spitze) NICHT back-to-back
                // nachschiessen - der Frame-BURST blaeht den adaptiven Abspielpuffer
                // des Zuschauers auf, und der baut sich kaum wieder ab (faktisch
                // belegt: VSTAT lag=515ms -> Burst -> bleibendes Delay). Stattdessen
                // hoechstens ~6% schneller als Echtzeit schreiben: ein 500-ms-
                // Rueckstand baut sich damit unsichtbar ueber ~9 s ab. Im
                // Normalbetrieb (kein Rueckstand) ist pace=0 und dieser Pfad inaktiv;
                // die Frame-ZAEHLUNG (A/V-Sync) bleibt in jedem Fall unveraendert.
                pace = (now - next > interval) ? now + interval * 0.94 : 0;
                // A/V-SYNC: FFmpeg taktet das Rohvideo durch ZAEHLEN (Frame N = N/fps) -
                // die geschriebene Frame-Zahl MUSS der Realzeit entsprechen, sonst
                // driftet das Bild gegen den Ton. Der fruehere Takt-Reset bei Rueckstand
                // verschluckte Ticks (-> wachsender Versatz); der erste Fix dagegen
                // schrieb Aufhol-BURSTS desselben alten Bildes (-> sichtbares Stocken +
                // fd3-Ton-Stau, faktisch belegt via VCATCHUP-Log). Die saubere Loesung
                // ist KEIN Sonderpfad: Haengt der Takt hinterher, entfaellt oben die
                // Wartezeit und jeder Durchlauf schreibt SOFORT seinen Tick - mit
                // frisch gepolltem Bild statt Duplikat-Salve. Der Loop holt dadurch
                // von selbst gleichmaessig auf, ohne je einen Tick zu verlieren.
                // Einziges Notventil: >5 s Rueckstand (Encoder faktisch tot) -> Takt
                // neu setzen und den Verlust sichtbar machen.
                if (now - next > 5000)
                {
                    Console.Error.WriteLine($"VDROP {(now - next):F0}ms");
                    Console.Error.Flush();
                    next = now;
                }

                // Alle aufgestauten Frames abholen, nur das neueste verarbeiten.
                Direct3D11CaptureFrame latest = null, f;
                while ((f = framePool.TryGetNextFrame()) != null) { latest?.Dispose(); latest = f; }
                if (latest != null)
                {
                    // Textur-Groessenwechsel? ZWEI Faelle sauber trennen:
                    //  (a) Nur die WGC-TEXTUR wechselt (Rahmen dazu/weg - faktisch belegtes
                    //      Pingpong z.B. 3876x2196 <-> 3840x2160 bei Browser-Fenstern):
                    //      Der INHALT ist unveraendert -> lautlos Crop-Offset nachfuehren,
                    //      KEIN Pipeline-Umbau, kein Stocken, keine SIZE-Aenderung.
                    //  (b) Der CLIENT-Inhalt aendert sich wirklich (Video -> Vollbild):
                    //      Letterbox-Scaler umbauen wie gehabt.
                    // Dazu STABILITAETS-HYSTERESE (~500 ms), damit hektische Wechsel
                    // gar nicht erst Arbeit ausloesen.
                    var cs = latest.ContentSize;
                    bool differs = cs.Width > 1 && cs.Height > 1 && (Math.Abs(cs.Width - _texW) > 1 || Math.Abs(cs.Height - _texH) > 1);
                    if (differs)
                    {
                        if (cs.Width != rszW || cs.Height != rszH) { rszW = cs.Width; rszH = cs.Height; rszSince = now; }
                        if (now - rszSince >= 500)
                        {
                            rszW = rszH = 0;
                            _texW = cs.Width; _texH = cs.Height;
                            try { framePool.Recreate(winrtDevice, fmt, 2, new SizeInt32(_texW, _texH)); } catch { }
                            if (!GetClientBox(hwnd, _texW, _texH, out int ox, out int oy, out int cw, out int ch)) { ox = 0; oy = 0; cw = _texW; ch = _texH; }
                            _cropX = ox; _cropY = oy;
                            cw = Math.Min(cw, _texW - _cropX); ch = Math.Min(ch, _texH - _cropY);
                            if (cw <= 0 || ch <= 0) { _cropX = 0; _cropY = 0; cw = _texW; ch = _texH; }
                            cw &= ~1; ch &= ~1;   // H.264 verlangt gerade Masse
                            if (Math.Abs(cw - _curW) > 1 || Math.Abs(ch - _curH) > 1)
                            {
                                // (b) echter Inhalts-Resize
                                _curW = cw; _curH = ch;
                                EnsureCropTex(hdr);
                                try { ConfigureLetterbox(hdr); }
                                catch (Exception ex) { if (!_vpLogged) { _vpLogged = true; Console.Error.WriteLine("RSZERR " + ex.Message); } }
                                // Raender schwarz: nur im BGRA-Weg noetig (CPU-Komposition).
                                // Im NV12-Weg kommt jedes Frame KOMPLETT aus dem GPU-Blt
                                // (Raender per ClearRenderTargetView in ConfigureLetterbox);
                                // Array.Clear waere hier sogar falsch (Y=0/UV=0 = Gruenstich).
                                if (frameBuf != null && !_nv12On) Array.Clear(frameBuf, 0, frameBuf.Length);
                            }
                            else
                            {
                                // (a) nur Textur/Rahmen gewechselt - Inhalt identisch
                                EnsureCropTex(hdr);
                                Console.Error.WriteLine($"CLIP {_cropX},{_cropY} {_curW}x{_curH} in {_texW}x{_texH}");
                            }
                        }
                        // Noch nicht stabil bzw. gerade umgebaut: dieses Frame ueberspringen
                        // (letztes Bild laeuft weiter) - kein Stocken.
                        latest.Dispose();
                    }
                    else
                    {
                        rszW = rszH = 0;
                        if (frameBuf == null) frameBuf = new byte[_nv12On ? _outW * _outH * 3 / 2 : _outW * 4 * _outH];
                        double c0 = sw.Elapsed.TotalMilliseconds;
                        try { CopyFrameInto(latest, frameBuf); } catch { }
                        statMaxCopy = Math.Max(statMaxCopy, sw.Elapsed.TotalMilliseconds - c0);
                        latest.Dispose();
                    }
                }
                if (frameBuf != null)
                {
                    double w0 = sw.Elapsed.TotalMilliseconds;
                    bool wrote = true;
                    try { stdout.Write(frameBuf, 0, frameBuf.Length); stdout.Flush(); }
                    catch
                    {
                        wrote = false;
                        // Pipe gebrochen. Ohne Reconnect-Modus (oder kein FFmpeg
                        // in Sicht): beenden wie bisher. Mit Reconnect-Modus: auf
                        // einen neuen FFmpeg warten und WEITERLAUFEN.
                        if (vidPipe == null || reconnectMs <= 0 || !_running) break;
                        Console.Error.WriteLine("PIPEDROP"); Console.Error.Flush();
                        try { vidPipe.Disconnect(); } catch { }
                        bool reconnected = false;
                        try { reconnected = vidPipe.WaitForConnectionAsync().Wait(reconnectMs); } catch { reconnected = false; }
                        if (!reconnected || !_running) { try { vidPipe.Dispose(); } catch { } break; }
                        // Takt-Basis frisch setzen: der neue FFmpeg zaehlt Frames ab
                        // 0, also ab JETZT takten (kein Burst-Nachholen der Wartezeit,
                        // A/V-Sync beginnt sauber neu). Frame-Puffer NICHT neu bauen.
                        next = sw.Elapsed.TotalMilliseconds; pace = 0;
                        Console.Error.WriteLine("PIPERECON"); Console.Error.Flush();
                    }
                    if (!wrote) continue;   // dieses Frame ging verloren; naechster Tick schreibt frisch
                    double wDur = sw.Elapsed.TotalMilliseconds - w0;
                    statMaxWrite = Math.Max(statMaxWrite, wDur);
                    // Einzelereignis mit Zeitstempel (VSTAT zeigt nur das 10s-Maximum):
                    // erlaubt die Korrelation der Stalls mit ff-stat/gpu/node-lag-Zeilen.
                    if (wDur >= 100) Console.Error.WriteLine($"WSTALL {wDur:F0}ms");
                }
                if (latest != null) statFresh++; else statDup++;
                if (now - statLast >= 10000)
                {
                    if (statLast > 0) Console.Error.WriteLine($"VSTAT fresh={statFresh} dup={statDup} lag={Math.Max(0, now - next):F0}ms maxcopy={statMaxCopy:F0}ms maxwrite={statMaxWrite:F0}ms");
                    statFresh = 0; statDup = 0; statMaxCopy = 0; statMaxWrite = 0; statLast = now;
                }
            }
            try { session.Dispose(); framePool.Dispose(); } catch { }
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("ERR " + ex.Message);
            return 1;
        }
    }

    // Ein WGC-Frame (GPU-Textur) in den dichten BGRA-Puffer kopieren.
    static void CopyFrameInto(Direct3D11CaptureFrame frame, byte[] dst)
    {
        var access = frame.Surface.As<IDirect3DDxgiInterfaceAccess>();
        Guid iid = ID3D11Texture2D_IID;
        IntPtr texPtr = access.GetInterface(ref iid);
        using var wgcTex = new ID3D11Texture2D(texPtr);
        // Zuerst den CLIENT-Ausschnitt aus der Fenster-Textur schneiden (keine
        // Rahmenpixel im Stream); die gesamte Pipeline arbeitet auf dem Ausschnitt.
        var srcTex = PrepareSource(wgcTex);
        if (_resized) { CopyFrameLetterbox(srcTex, dst); return; }
        // Ziel der Verarbeitungspfade: im NV12-Modus die BGRA-Sammel-Textur
        // (_finalTex, GPU-intern, danach EIN Konvertier-Blt), im BGRA-Modus wie
        // gehabt direkt die CPU-Staging-Textur. Die Pfade selbst sind identisch.
        ID3D11Texture2D target = _nv12On ? _finalTex : _staging;
        if (_numMips > 0)
        {
            // Content -> mip0, Mip-Kette auf der GPU erzeugen, Ziel-Mip (kleiner)
            // in die Ziel-Textur kopieren. SrcBox croppt auf die GERADE Ausgabe-
            // groesse (die Mip selbst kann 1 px breiter/hoeher sein, s. &~1 oben).
            _context.CopySubresourceRegion(_mipTex, 0, 0, 0, 0, srcTex, 0);
            _context.GenerateMips(_mipSrv);
            _context.CopySubresourceRegion(target, 0, 0, 0, 0, _mipTex, (uint)_numMips,
                new Vortice.Mathematics.Box(0, 0, 0, _outW, _outH, 1));
        }
        else if (_useShader)
        {
            RenderShaderTonemap(srcTex);
            _context.CopyResource(target, _scaleTex);
        }
        else if (_useVp)
        {
            try
            {
                using var inView = _vdev.CreateVideoProcessorInputView(srcTex, _venum,
                    new VideoProcessorInputViewDescription { FourCC = 0, ViewDimension = VideoProcessorInputViewDimension.Texture2D });
                var stream = new VideoProcessorStream { Enable = true, InputSurface = inView };
                _vctx.VideoProcessorBlt(_vproc, _voutView, 0, 1, new[] { stream });
                _context.CopyResource(target, _scaleTex);
            }
            catch (Exception ex) { if (!_vpLogged) { _vpLogged = true; Console.Error.WriteLine("VPERR " + ex.Message); } throw; }
        }
        else _context.CopyResource(target, srcTex);
        if (_nv12On) { Nv12Finish(dst); return; }
        var map = _context.Map(_staging, 0, Vortice.Direct3D11.MapMode.Read, Vortice.Direct3D11.MapFlags.None);
        int rowBytes = _outW * 4, pitch = (int)map.RowPitch;
        IntPtr src = map.DataPointer;
        if (pitch == rowBytes) Marshal.Copy(src, dst, 0, rowBytes * _outH);   // dicht gepackt -> ein Rutsch
        else for (int y = 0; y < _outH; y++) Marshal.Copy(src + y * pitch, dst, y * rowBytes, rowBytes);
        _context.Unmap(_staging, 0);
    }

    // NV12-Endstufe initialisieren: VideoProcessor (BGRA _outW x _outH -> NV12
    // gleicher Groesse) + Sammel-/Ziel-/Staging-Texturen. Farbraeume EXPLIZIT
    // setzen (Eingang sRGB voll, Ausgang BT.709 Studio/limited - der Standard
    // fuer H.264-Streams; ohne explizite Angabe waehlen Treiber unterschiedlich
    // -> Farbstich-Risiko). Scheitert IRGENDWAS: sauber aufraeumen und im
    // bewaehrten BGRA-Modus weiterlaufen (kein Risiko einer Verschlechterung).
    static void TryInitNv12()
    {
        try
        {
            if (_vdev == null) _vdev = _device.QueryInterface<ID3D11VideoDevice>();
            if (_vctx == null) _vctx = _context.QueryInterface<ID3D11VideoContext1>();
            _nvVenum = _vdev.CreateVideoProcessorEnumerator(new VideoProcessorContentDescription
            {
                InputFrameFormat = VideoFrameFormat.Progressive,
                InputWidth = (uint)_outW, InputHeight = (uint)_outH,
                OutputWidth = (uint)_outW, OutputHeight = (uint)_outH,
                Usage = VideoUsage.PlaybackNormal,
            });
            _nvVproc = _vdev.CreateVideoProcessor(_nvVenum, 0);
            _finalTex = _device.CreateTexture2D(new Texture2DDescription
            {
                Width = (uint)_outW, Height = (uint)_outH, MipLevels = 1, ArraySize = 1,
                Format = Vortice.DXGI.Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Default, BindFlags = BindFlags.RenderTarget | BindFlags.ShaderResource,
            });
            _finalRtv = _device.CreateRenderTargetView(_finalTex);
            _nv12Tex = _device.CreateTexture2D(new Texture2DDescription
            {
                Width = (uint)_outW, Height = (uint)_outH, MipLevels = 1, ArraySize = 1,
                Format = Vortice.DXGI.Format.NV12, SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Default, BindFlags = BindFlags.RenderTarget,
            });
            _nvVout = _vdev.CreateVideoProcessorOutputView(_nv12Tex, _nvVenum,
                new VideoProcessorOutputViewDescription { ViewDimension = VideoProcessorOutputViewDimension.Texture2D });
            _nv12Staging = _device.CreateTexture2D(new Texture2DDescription
            {
                Width = (uint)_outW, Height = (uint)_outH, MipLevels = 1, ArraySize = 1,
                Format = Vortice.DXGI.Format.NV12, SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Staging, BindFlags = BindFlags.None,
                CPUAccessFlags = CpuAccessFlags.Read, MiscFlags = ResourceOptionFlags.None,
            });
            try { _vctx.VideoProcessorSetStreamColorSpace1(_nvVproc, 0u, ColorSpaceType.RgbFullG22NoneP709); } catch { }
            try { _vctx.VideoProcessorSetOutputColorSpace1(_nvVproc, ColorSpaceType.YcbcrStudioG22LeftP709); } catch { }
            _nv12On = true;
            _nv12Primed = false;   // frische Staging: das erste Frame synchron abholen, danach doppelgepuffert
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("NV12ERR init: " + ex.Message);
            _nvVout?.Dispose(); _nvVout = null;
            _nv12Staging?.Dispose(); _nv12Staging = null;
            _nv12Tex?.Dispose(); _nv12Tex = null;
            _finalRtv?.Dispose(); _finalRtv = null;
            _finalTex?.Dispose(); _finalTex = null;
            _nvVproc?.Dispose(); _nvVproc = null;
            _nvVenum?.Dispose(); _nvVenum = null;
            _nv12On = false;
        }
    }

    // Finale NV12-Stufe eines Frames: BGRA-Sammel-Textur -> VideoProcessorBlt
    // (Farbkonvertierung auf der GPU) -> NV12-Staging -> dicht gepackt in den
    // Ausgabepuffer (erst Y-Plane, dann UV-Plane; D3D11-Layout: UV beginnt bei
    // DataPointer + RowPitch * Hoehe, beide Planes teilen sich die RowPitch).
    static void Nv12Finish(byte[] dst)
    {
        try
        {
            // DOPPELPUFFER-READBACK. Ein Map() auf eine Staging-Textur BLOCKIERT, bis die
            // GPU die davor eingereihte Farbkonvertierung + Kopie fertig hat - und dieselbe
            // GPU rendert das aufgenommene Spiel. Unter Last dauerte dieser Map 25-40 ms
            // (Log: maxcopy), bei 60 fps stehen aber nur 16 ms pro Frame zur Verfuegung ->
            // der Takt-Rueckstand (lag) baute sich bis zum 5-s-VDROP auf (sichtbarer Ruckler).
            // Loesung: Wir mappen NICHT das gerade erzeugte Frame, sondern das des VORIGEN
            // Aufrufs. Dessen Kopie wurde per Flush sofort angestossen und hatte einen ganzen
            // Frame Zeit auf der GPU -> der Map wartet praktisch nicht mehr. Kostet einen
            // konstanten Frame (~16 ms) Latenz, im gepufferten Live-Stream unsichtbar.
            if (_nv12Primed)
            {
                // Voriges (laengst fertig kopiertes) Frame abholen - blockiert kaum noch.
                Nv12Drain(dst);
            }
            else
            {
                // Allererstes Frame nach (Neu-)Aufbau: es gibt noch kein voriges Ergebnis.
                // Ein neutrales Bild ausgeben (Y=0, UV=128) statt eines gruenen Frames aus
                // uninitialisiertem Speicher; ab dem naechsten Frame kommt echter Inhalt.
                int y0 = _outW * _outH;
                for (int i = 0; i < y0; i++) dst[i] = 0;
                for (int i = y0; i < y0 + y0 / 2; i++) dst[i] = 128;
            }
            // Aktuelles Frame konvertieren, in die Staging kopieren und die GPU SOFORT
            // anstossen (Flush), damit die Kopie bis zum naechsten Tick fertig ist.
            using var inView = _vdev.CreateVideoProcessorInputView(_finalTex, _nvVenum,
                new VideoProcessorInputViewDescription { FourCC = 0, ViewDimension = VideoProcessorInputViewDimension.Texture2D });
            var stream = new VideoProcessorStream { Enable = true, InputSurface = inView };
            _vctx.VideoProcessorBlt(_nvVproc, _nvVout, 0, 1, new[] { stream });
            _context.CopyResource(_nv12Staging, _nv12Tex);
            _context.Flush();
            _nv12Primed = true;
        }
        catch (Exception ex)
        {
            // Blt zur LAUFZEIT kaputt (Treiber-Eigenheit): NICHT still schwarz
            // weiterstreamen - klare Meldung + Selbstbeendigung. Lumora erkennt
            // NV12ERR, merkt sich den Ausfall und startet die Aufnahme
            // automatisch im bewaehrten BGRA-Modus neu.
            Console.Error.WriteLine("NV12ERR blt: " + ex.Message);
            Console.Error.Flush();
            Environment.Exit(6);
        }
    }

    // CPU-Abholung des zuletzt in die NV12-Staging kopierten Frames (Y-Plane dicht,
    // danach UV-Plane; beide teilen sich die RowPitch). Ausgelagert, weil der
    // Doppelpuffer-Readback dies einen Tick spaeter aufruft als die GPU-Kopie.
    static void Nv12Drain(byte[] dst)
    {
        var map = _context.Map(_nv12Staging, 0, Vortice.Direct3D11.MapMode.Read, Vortice.Direct3D11.MapFlags.None);
        int pitch = (int)map.RowPitch;
        IntPtr src = map.DataPointer;
        int ySize = _outW * _outH;
        if (pitch == _outW)
        {
            Marshal.Copy(src, dst, 0, ySize);                                   // Y dicht -> ein Rutsch
            Marshal.Copy(src + (nint)pitch * _outH, dst, ySize, ySize / 2);     // UV dicht -> ein Rutsch
        }
        else
        {
            for (int y = 0; y < _outH; y++) Marshal.Copy(src + (nint)y * pitch, dst, y * _outW, _outW);
            IntPtr uv = src + (nint)pitch * _outH;
            for (int y = 0; y < _outH / 2; y++) Marshal.Copy(uv + (nint)y * pitch, dst, ySize + y * _outW, _outW);
        }
        _context.Unmap(_nv12Staging, 0);
    }

    // Nach einer Fenster-Groessenaenderung: Scaler (neu) aufbauen, der den aktuellen
    // Inhalt _curW x _curH seitenverhaeltnis-treu in ein zentriertes Rechteck
    // _dw x _dh @(_dx,_dy) des festen Ausgabepuffers _outW x _outH skaliert.
    static void ConfigureLetterbox(bool hdr)
    {
        double s = Math.Min((double)_outW / _curW, (double)_outH / _curH);
        _dw = ((int)Math.Round(_curW * s)) & ~1; if (_dw < 2) _dw = 2; if (_dw > _outW) _dw = _outW;
        _dh = ((int)Math.Round(_curH * s)) & ~1; if (_dh < 2) _dh = 2; if (_dh > _outH) _dh = _outH;
        _dx = ((_outW - _dw) / 2) & ~1; _dy = (_outH - _dh) / 2;

        // alte Letterbox-Ressourcen freigeben (bei erneuter Groessenaenderung)
        _lbVout?.Dispose(); _lbVout = null;
        _lbScaleTex?.Dispose(); _lbScaleTex = null;
        _lbStaging?.Dispose(); _lbStaging = null;
        _lbRtv?.Dispose(); _lbRtv = null;
        _lbSrcCopy?.Dispose(); _lbSrcCopy = null;
        _lbVproc?.Dispose(); _lbVproc = null;
        _lbVenum?.Dispose(); _lbVenum = null;

        // Skalierziel (_dw x _dh) + CPU-lesbare Staging-Kopie
        _lbScaleTex = _device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)_dw, Height = (uint)_dh, MipLevels = 1, ArraySize = 1,
            Format = Vortice.DXGI.Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Default, BindFlags = BindFlags.RenderTarget,
        });
        _lbStaging = _device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)_dw, Height = (uint)_dh, MipLevels = 1, ArraySize = 1,
            Format = Vortice.DXGI.Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Staging, BindFlags = BindFlags.None,
            CPUAccessFlags = CpuAccessFlags.Read, MiscFlags = ResourceOptionFlags.None,
        });

        if (hdr)
        {
            // HDR: FP16-Quelle per eigenem Shader tonemappen (VP kann kein FP16).
            if (_vs == null) SetupShader();          // Shader/Sampler einmalig kompilieren
            _lbSrcCopy = _device.CreateTexture2D(new Texture2DDescription
            {
                Width = (uint)_curW, Height = (uint)_curH, MipLevels = 1, ArraySize = 1,
                Format = Vortice.DXGI.Format.R16G16B16A16_Float, SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Default, BindFlags = BindFlags.ShaderResource,
            });
            _lbRtv = _device.CreateRenderTargetView(_lbScaleTex);
        }
        else
        {
            // SDR: GPU-Video-Processor skaliert _curW x _curH -> _dw x _dh.
            _vdev = _device.QueryInterface<ID3D11VideoDevice>();
            _vctx = _context.QueryInterface<ID3D11VideoContext1>();
            _lbVenum = _vdev.CreateVideoProcessorEnumerator(new VideoProcessorContentDescription
            {
                InputFrameFormat = VideoFrameFormat.Progressive,
                InputWidth = (uint)_curW, InputHeight = (uint)_curH,
                OutputWidth = (uint)_dw, OutputHeight = (uint)_dh,
                Usage = VideoUsage.PlaybackNormal,
            });
            _lbVproc = _vdev.CreateVideoProcessor(_lbVenum, 0);
            _lbVout = _vdev.CreateVideoProcessorOutputView(_lbScaleTex, _lbVenum,
                new VideoProcessorOutputViewDescription { ViewDimension = VideoProcessorOutputViewDimension.Texture2D });
        }
        // NV12-Weg: Die Letterbox-Raender leben in der BGRA-Sammel-Textur (der
        // GPU-Blt liefert jedes Frame KOMPLETT inkl. Raender) - einmal pro
        // Umbau schwarz clearen; das zentrierte Inhalts-Rechteck ueberschreibt
        // CopyFrameLetterbox danach in jedem Frame.
        if (_nv12On && _finalRtv != null)
            _context.ClearRenderTargetView(_finalRtv, new Vortice.Mathematics.Color4(0f, 0f, 0f, 1f));
        _resized = true;
        Console.Error.WriteLine($"RESIZE {_curW} {_curH} -> {_dw}x{_dh}@{_dx},{_dy}");
        Console.Error.Flush();
    }

    // Ein Frame im Letterbox-Modus: skalieren -> _lbScaleTex, dann zeilenweise in
    // das zentrierte Rechteck des (schwarz vorbelegten) Ausgabepuffers kopieren.
    static void CopyFrameLetterbox(ID3D11Texture2D srcTex, byte[] dst)
    {
        if (_lbSrcCopy != null)   // HDR-Weg: Shader-Tonemap
        {
            _context.CopyResource(_lbSrcCopy, srcTex);
            using var srv = _device.CreateShaderResourceView(_lbSrcCopy);
            _context.VSSetShader(_vs);
            _context.PSSetShader(_ps);
            _context.PSSetShaderResource(0, srv);
            _context.PSSetSampler(0, _sampler);
            _context.OMSetRenderTargets(_lbRtv);
            _context.RSSetViewport(0, 0, _dw, _dh);
            _context.IASetPrimitiveTopology(PrimitiveTopology.TriangleList);
            _context.Draw(3, 0);
            _context.OMSetRenderTargets((ID3D11RenderTargetView)null);
        }
        else                      // SDR-Weg: Video-Processor
        {
            using var inView = _vdev.CreateVideoProcessorInputView(srcTex, _lbVenum,
                new VideoProcessorInputViewDescription { FourCC = 0, ViewDimension = VideoProcessorInputViewDimension.Texture2D });
            var stream = new VideoProcessorStream { Enable = true, InputSurface = inView };
            _vctx.VideoProcessorBlt(_lbVproc, _lbVout, 0, 1, new[] { stream });
        }
        if (_nv12On)
        {
            // Skalierter Inhalt zentriert in die (schwarz geclearte) Sammel-
            // Textur - Komposition auf der GPU statt zeilenweise auf der CPU;
            // danach dieselbe NV12-Endstufe wie im Normalpfad.
            _context.CopySubresourceRegion(_finalTex, 0, (uint)_dx, (uint)_dy, 0, _lbScaleTex, 0);
            Nv12Finish(dst);
            return;
        }
        _context.CopyResource(_lbStaging, _lbScaleTex);
        var map = _context.Map(_lbStaging, 0, Vortice.Direct3D11.MapMode.Read, Vortice.Direct3D11.MapFlags.None);
        int rowBytes = _dw * 4, pitch = (int)map.RowPitch, stride = _outW * 4;
        IntPtr src = map.DataPointer;
        for (int y = 0; y < _dh; y++)
            Marshal.Copy(src + y * pitch, dst, (_dy + y) * stride + _dx * 4, rowBytes);
        _context.Unmap(_lbStaging, 0);
    }

    // Video-Processor der GPU einrichten: flexibler Scaler + (bei HDR) HDR->SDR-
    // Tonemapping ueber die Farbraum-Konvertierung.
    static void SetupVideoProcessor(int sw, int sh, int dw, int dh, bool hdr)
    {
        _vdev = _device.QueryInterface<ID3D11VideoDevice>();
        _vctx = _context.QueryInterface<ID3D11VideoContext1>();
        var desc = new VideoProcessorContentDescription
        {
            InputFrameFormat = VideoFrameFormat.Progressive,
            InputWidth = (uint)sw, InputHeight = (uint)sh,
            OutputWidth = (uint)dw, OutputHeight = (uint)dh,
            Usage = VideoUsage.PlaybackNormal,
        };
        _venum = _vdev.CreateVideoProcessorEnumerator(desc);
        _vproc = _vdev.CreateVideoProcessor(_venum, 0);
        _scaleTex = _device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)dw, Height = (uint)dh, MipLevels = 1, ArraySize = 1,
            Format = Vortice.DXGI.Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Default, BindFlags = BindFlags.RenderTarget,
        });
        _voutView = _vdev.CreateVideoProcessorOutputView(_scaleTex, _venum,
            new VideoProcessorOutputViewDescription { ViewDimension = VideoProcessorOutputViewDimension.Texture2D });
        if (hdr)
        {
            // Eingang scRGB (linear FP16, was WGC bei HDR liefert) -> Ausgang SDR
            // (sRGB/BT.709). Der Treiber macht die HDR->SDR-Umsetzung (Tonemapping).
            try { _vctx.VideoProcessorSetStreamColorSpace1(_vproc, 0u, ColorSpaceType.RgbFullG10NoneP709); } catch { }
            try { _vctx.VideoProcessorSetOutputColorSpace1(_vproc, ColorSpaceType.RgbFullG22NoneP709); } catch { }
            try { _vctx.VideoProcessorSetStreamAutoProcessingMode(_vproc, 0u, true); } catch { }
        }
    }
    // HDR-Status ALLER Monitore in ddagrab-Reihenfolge (Adapter-, dann Output-
    // Index) auf stdout: "HDR <idx> <0|1>" je Monitor. Lumora warnt im Monitor-
    // Weg vor blassen Farben, wenn der gewaehlte Bildschirm in HDR laeuft - der
    // ddagrab-Weg hat (anders als der Fenster-Weg) kein Tonemapping (Audit).
    static int HdrCheck()
    {
        try
        {
            Vortice.DXGI.DXGI.CreateDXGIFactory1(out Vortice.DXGI.IDXGIFactory1 factory).CheckError();
            using (factory)
            {
                int idx = 0;
                for (uint ai = 0; factory.EnumAdapters1(ai, out Vortice.DXGI.IDXGIAdapter1 adapter).Success; ai++)
                {
                    using (adapter)
                    {
                        for (uint oi = 0; adapter.EnumOutputs(oi, out IDXGIOutput output).Success; oi++)
                        {
                            using (output)
                            {
                                bool hdrOn = false;
                                try { using var o6 = output.QueryInterface<IDXGIOutput6>(); hdrOn = o6.Description1.ColorSpace == ColorSpaceType.RgbFullG2084NoneP2020; } catch { }
                                Console.Out.WriteLine("HDR " + idx + " " + (hdrOn ? "1" : "0"));
                                idx++;
                            }
                        }
                    }
                }
            }
        }
        catch (Exception ex) { Console.Error.WriteLine("ERR " + ex.Message); }
        return 0;
    }
    // HDR aktiv? Prueft, ob ein angeschlossener Monitor im HDR-Modus (PQ/BT.2020) laeuft.
    static bool DetectHdr()
    {
        try
        {
            using var dxgiDev = _device.QueryInterface<IDXGIDevice>();
            dxgiDev.GetAdapter(out IDXGIAdapter adapter);
            using (adapter)
            {
                for (uint oi = 0; adapter.EnumOutputs(oi, out IDXGIOutput output).Success; oi++)
                {
                    using (output)
                    using (var o6 = output.QueryInterface<IDXGIOutput6>())
                    {
                        if (o6.Description1.ColorSpace == ColorSpaceType.RgbFullG2084NoneP2020) return true;
                    }
                }
            }
        }
        catch { }
        return false;
    }

    // HDR->SDR-Tonemapping + Skalierung per eigenem Pixel-Shader. Eingang ist
    // scRGB (linear, FP16); ACES-Kurve komprimiert die Highlights, Ausgang sRGB.
    const string SHADER_HLSL = @"
cbuffer Params : register(b0) { int uMode; float uExposure; float uP1; float uP2; };
Texture2D tex : register(t0);
SamplerState samp : register(s0);
void vsmain(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
// Tonemap-Kurven (Eingang linear, bereits mit Exposure skaliert):
float3 tmACES(float3 x)     { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
float3 hableU(float3 x)     { float A=0.15,B=0.50,C=0.10,D=0.20,E=0.02,F=0.30; return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F; }
float3 tmHable(float3 x)    { return saturate(hableU(x) / hableU(11.2)); }
float3 tmReinhard(float3 x) { float L=4.0; return saturate(x * (1.0 + x/(L*L)) / (1.0 + x)); }
float4 psmain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 c = max(tex.Sample(samp, uv).rgb, 0.0) * uExposure;
    float3 x;
    if (uMode == 1)      x = tmHable(c);
    else if (uMode == 2) x = tmReinhard(c);
    else if (uMode == 3) x = saturate(c);        // Linear + Clip (hellste Referenz)
    else                 x = tmACES(c);          // 0 = ACES (Default, wie bisher)
    x = pow(x, 1.0 / 2.2);
    return float4(x, 1.0);
}
";
    static void SetupShader()
    {
        var vsBlob = Compiler.Compile(SHADER_HLSL, "vsmain", "s", "vs_5_0");
        var psBlob = Compiler.Compile(SHADER_HLSL, "psmain", "s", "ps_5_0");
        _vs = _device.CreateVertexShader(vsBlob.Span);
        _ps = _device.CreatePixelShader(psBlob.Span);
        _sampler = _device.CreateSamplerState(new SamplerDescription
        {
            Filter = Filter.MinMagMipLinear,
            AddressU = TextureAddressMode.Clamp, AddressV = TextureAddressMode.Clamp, AddressW = TextureAddressMode.Clamp,
        });
        _srcCopy = _device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)_width, Height = (uint)_height, MipLevels = 1, ArraySize = 1,
            Format = Vortice.DXGI.Format.R16G16B16A16_Float, SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Default, BindFlags = BindFlags.ShaderResource,
        });
        _scaleTex = _device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)_outW, Height = (uint)_outH, MipLevels = 1, ArraySize = 1,
            Format = Vortice.DXGI.Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Default, BindFlags = BindFlags.RenderTarget,
        });
        _rtv = _device.CreateRenderTargetView(_scaleTex);
        _psParams = _device.CreateBuffer(new BufferDescription(16, BindFlags.ConstantBuffer, ResourceUsage.Dynamic, CpuAccessFlags.Write));
        TmReadControl(true);   // Startwerte (Steuerdatei, falls vorhanden) -> Buffer initialisieren
    }
    static void RenderShaderTonemap(ID3D11Texture2D src)
    {
        TmReadControl(false);   // Live-Justierung: Steuerdatei alle paar Frames pruefen
        _context.CopyResource(_srcCopy, src);   // WGC-Textur -> ShaderResource-faehige Kopie
        using var srv = _device.CreateShaderResourceView(_srcCopy);
        _context.VSSetShader(_vs);
        _context.PSSetShader(_ps);
        _context.PSSetShaderResource(0, srv);
        _context.PSSetSampler(0, _sampler);
        _context.PSSetConstantBuffer(0, _psParams);
        _context.OMSetRenderTargets(_rtv);
        _context.RSSetViewport(0, 0, _outW, _outH);
        _context.IASetPrimitiveTopology(PrimitiveTopology.TriangleList);
        _context.Draw(3, 0);
        _context.OMSetRenderTargets((ID3D11RenderTargetView)null);   // RTV loesen fuer die Kopie danach
    }
    // Steuerdatei %TEMP%\lumora-hdr.txt (Format: "<mode> <exposure>") LIVE einlesen -
    // so justiert der Nutzer Kurve + Helligkeit im laufenden Stream, ohne Neustart.
    // Nur alle 15 Frames per mtime-Check (billig); force=true beim ersten Setup.
    static void TmReadControl(bool force)
    {
        if (!force && (++_tmCtlCtr % 15) != 0) return;
        if (_tmCtlPath == null) _tmCtlPath = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "lumora-hdr.txt");
        try
        {
            long mt = System.IO.File.Exists(_tmCtlPath) ? System.IO.File.GetLastWriteTimeUtc(_tmCtlPath).ToFileTimeUtc() : 0;
            if (mt == _tmCtlMtime) { if (force) TmUpdateBuffer(); return; }
            _tmCtlMtime = mt;
            if (mt != 0)
            {
                var parts = System.IO.File.ReadAllText(_tmCtlPath).Trim().Split(' ');
                if (parts.Length >= 1) int.TryParse(parts[0], out _tmMode);
                if (parts.Length >= 2) float.TryParse(parts[1], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out _tmExposure);
                Console.Error.WriteLine("HDRTM mode=" + _tmMode + " exp=" + _tmExposure.ToString(System.Globalization.CultureInfo.InvariantCulture));
            }
            TmUpdateBuffer();
        }
        catch { }
    }
    static void TmUpdateBuffer()
    {
        if (_psParams == null) return;
        try
        {
            var m = _context.Map(_psParams, 0, Vortice.Direct3D11.MapMode.WriteDiscard, Vortice.Direct3D11.MapFlags.None);
            System.Runtime.InteropServices.Marshal.WriteInt32(m.DataPointer, 0, _tmMode);
            System.Runtime.InteropServices.Marshal.WriteInt32(m.DataPointer, 4, BitConverter.SingleToInt32Bits(_tmExposure));
            _context.Unmap(_psParams, 0);
        }
        catch { }
    }

    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT val, int size);
    // SICHTBAREN Ausschnitt relativ zur WGC-Textur bestimmen. Die TEXTURGROESSE ist
    // dabei die Wahrheit, die Fenster-Geometrie nur die Interpretation:
    //  - Entspricht die Textur der Fenstergroesse INKL. unsichtbarer Rahmen
    //    (GetWindowRect), wird auf die DWM Extended-Frame-Bounds (= das, was der
    //    Nutzer wirklich sieht) beschnitten.
    //  - Entspricht die Textur bereits den Extended-Frame-Bounds (neuere Windows-
    //    Builds liefern im Borderless-Modus rahmenfreie Texturen), wird NICHT
    //    beschnitten. Faktisch belegt: blindes Abziehen des Rahmenversatzes hat
    //    hier 18 px sichtbaren Inhalt gekappt (CLIP 18,18 in 3840x2160 -> 1926x1080).
    //  - Unerwartete Kombination -> ganze Textur (sicherer Default).
    // WICHTIG: NICHT GetClientRect - Browser mit eigener Titelleiste definieren ihre
    // Client-Flaeche selbst und ragen maximiert in die Off-Screen-Rahmenzone.
    static bool GetClientBox(IntPtr hwnd, int texW, int texH, out int ox, out int oy, out int cw, out int ch)
    {
        ox = 0; oy = 0; cw = texW; ch = texH;
        if (!GetWindowRect(hwnd, out RECT wr)) return false;
        if (DwmGetWindowAttribute(hwnd, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, out RECT vis, Marshal.SizeOf<RECT>()) != 0) return false;
        int wrW = wr.Right - wr.Left, wrH = wr.Bottom - wr.Top;
        int visW = vis.Right - vis.Left, visH = vis.Bottom - vis.Top;
        bool texIsWindow = Math.Abs(texW - wrW) <= 2 && Math.Abs(texH - wrH) <= 2;
        bool texIsVisible = Math.Abs(texW - visW) <= 2 && Math.Abs(texH - visH) <= 2;
        if (texIsWindow && !texIsVisible && visW > 0 && visH > 0)
        {
            ox = Math.Max(0, vis.Left - wr.Left);
            oy = Math.Max(0, vis.Top - wr.Top);
            cw = visW; ch = visH;
        }
        return cw > 0 && ch > 0;
    }
    // Zwischen-Textur fuer den Client-Crop (in _curW x _curH) anlegen bzw. verwerfen.
    static void EnsureCropTex(bool hdr)
    {
        _cropTex?.Dispose(); _cropTex = null;
        _cropOn = _cropX != 0 || _cropY != 0 || _curW != _texW || _curH != _texH;
        if (!_cropOn) return;
        _cropTex = _device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)_curW, Height = (uint)_curH, MipLevels = 1, ArraySize = 1,
            Format = hdr ? Vortice.DXGI.Format.R16G16B16A16_Float : Vortice.DXGI.Format.B8G8R8A8_UNorm,
            SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Default, BindFlags = BindFlags.ShaderResource | BindFlags.RenderTarget,
        });
    }
    // Liefert die Quelle fuer die Verarbeitungs-Pipeline: bei Crop den Client-
    // Ausschnitt der WGC-Textur (GPU-Kopie), sonst die Textur direkt.
    static ID3D11Texture2D PrepareSource(ID3D11Texture2D srcTex)
    {
        if (!_cropOn || _cropTex == null) return srcTex;
        int w = Math.Min(_curW, _texW - _cropX), h = Math.Min(_curH, _texH - _cropY);
        if (w <= 0 || h <= 0) return srcTex;
        _context.CopySubresourceRegion(_cropTex, 0, 0, 0, 0, srcTex, 0, new Vortice.Mathematics.Box(_cropX, _cropY, 0, _cropX + w, _cropY + h, 1));
        return _cropTex;
    }

    static GraphicsCaptureItem CreateItemForWindow(IntPtr hwnd)
    {
        var factory = WinRT.ActivationFactory.Get("Windows.Graphics.Capture.GraphicsCaptureItem");
        var interop = factory.AsInterface<IGraphicsCaptureItemInterop>();
        Guid iid = new Guid("79C3F95B-31F7-4EC2-A464-632EF5D30760"); // IGraphicsCaptureItem
        IntPtr itemPtr = interop.CreateForWindow(hwnd, ref iid);
        var item = MarshalInterface<GraphicsCaptureItem>.FromAbi(itemPtr);
        Marshal.Release(itemPtr);
        return item;
    }

    static IntPtr FindByTitle(string needle)
    {
        IntPtr found = IntPtr.Zero;
        needle = needle.ToLowerInvariant();
        EnumWindows((h, p) =>
        {
            if (!IsWindowVisible(h)) return true;
            int len = GetWindowTextLength(h);
            if (len == 0) return true;
            var sb = new StringBuilder(len + 1);
            GetWindowText(h, sb, sb.Capacity);
            if (sb.ToString().ToLowerInvariant().Contains(needle)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    // Alle sichtbaren, betitelten Top-Level-Fenster als "hwnd\ttitel" auf stdout.
    // Ersetzt Electrons desktopCapturer, das viele Fenster (Explorer/Edge u.a.)
    // verschluckt. Cloaked Fenster (anderer virtueller Desktop / suspendierte UWP)
    // werden ausgelassen. Die HWND passt exakt zu CreateForWindow beim Aufnehmen.
    static int ListWindows()
    {
        // UTF-8 OHNE BOM, damit Fenstertitel mit Umlauten/Sonderzeichen in main.js
        // korrekt ankommen (Konsolen-Default waere OEM -> Mojibake).
        try { Console.OutputEncoding = new System.Text.UTF8Encoding(false); } catch { }
        var stdout = Console.Out;
        EnumWindows((h, p) =>
        {
            if (!IsWindowVisible(h)) return true;
            int len = GetWindowTextLength(h);
            if (len == 0) return true;
            var sb = new StringBuilder(len + 1);
            GetWindowText(h, sb, sb.Capacity);
            var title = sb.ToString().Trim();
            if (title.Length == 0) return true;
            int cloaked = 0;
            try { DwmGetWindowAttribute(h, 14, out cloaked, sizeof(int)); } catch { }   // DWMWA_CLOAKED = 14
            if (cloaked != 0) return true;
            stdout.WriteLine((long)h + "\t" + title + "\t" + IconBase64(h));
            return true;
        }, IntPtr.Zero);
        return 0;
    }

    // App-Icon eines Fensters als PNG (Base64) fuer die Quellen-Liste; "" wenn keins.
    // Reihenfolge: WM_GETICON (grosses, dann Small2), dann Fenster-Klasse (HICON/HICONSM).
    static string IconBase64(IntPtr hwnd)
    {
        try
        {
            IntPtr res;
            SendMessageTimeout(hwnd, 0x7F, (IntPtr)1, IntPtr.Zero, 0, 200, out res);        // WM_GETICON, ICON_BIG
            IntPtr hIcon = res;
            if (hIcon == IntPtr.Zero) { SendMessageTimeout(hwnd, 0x7F, (IntPtr)2, IntPtr.Zero, 0, 200, out res); hIcon = res; }  // ICON_SMALL2
            if (hIcon == IntPtr.Zero) hIcon = GetClassLongPtr(hwnd, -14);                    // GCLP_HICON
            if (hIcon == IntPtr.Zero) hIcon = GetClassLongPtr(hwnd, -34);                    // GCLP_HICONSM
            if (hIcon == IntPtr.Zero) return "";
            using (var ico = System.Drawing.Icon.FromHandle(hIcon))
            using (var bmp0 = ico.ToBitmap())
            using (var bmp = new System.Drawing.Bitmap(bmp0, new System.Drawing.Size(24, 24)))   // Anzeigegroesse -> kleine Base64
            using (var ms = new System.IO.MemoryStream())
            {
                bmp.Save(ms, System.Drawing.Imaging.ImageFormat.Png);
                return Convert.ToBase64String(ms.ToArray());
            }
        }
        catch { return ""; }
    }

    // --- Prozess-gebundene Audioaufnahme (WASAPI "Process Loopback", ab Win10 2004) --
    // Im FENSTER-Modus soll nur der Ton DIESES Fensters/Prozesses (inkl. Kindprozesse)
    // im Stream landen. Ohne das faengt WasapiLoopbackCapture (System-Loopback) IMMER
    // den kompletten Sound-Mix des PCs ein - z.B. Firefox-Ton, obwohl nur ein
    // Spielfenster geteilt wurde (genau der gemeldete Fehler). NAudio kennt die
    // noetigen Datentypen zwar (AudioClientActivationParams etc., NAudio.Wasapi
    // 2.2.1), verdrahtet sie aber nicht oeffentlich - daher hier die Aktivierung von
    // Hand per P/Invoke; danach uebernimmt NAudios eigener AudioClient/
    // AudioCaptureClient-Wrapper den Rest (dieselben Klassen wie beim System-Loopback).
    [DllImport("Mmdevapi.dll", ExactSpelling = true, PreserveSig = false)]
    static extern void ActivateAudioInterfaceAsync(
        [MarshalAs(UnmanagedType.LPWStr)] string deviceInterfacePath,
        Guid riid,
        IntPtr activationParams,
        NAudio.Wasapi.CoreAudioApi.Interfaces.IActivateAudioInterfaceCompletionHandler completionHandler,
        out NAudio.Wasapi.CoreAudioApi.Interfaces.IActivateAudioInterfaceAsyncOperation activationOperation);

    class AudioActivationHandler : NAudio.Wasapi.CoreAudioApi.Interfaces.IActivateAudioInterfaceCompletionHandler
    {
        public readonly ManualResetEvent Done = new ManualResetEvent(false);
        public NAudio.Wasapi.CoreAudioApi.Interfaces.IActivateAudioInterfaceAsyncOperation Operation;
        public void ActivateCompleted(NAudio.Wasapi.CoreAudioApi.Interfaces.IActivateAudioInterfaceAsyncOperation activateOperation)
        { Operation = activateOperation; Done.Set(); }
    }

    [StructLayout(LayoutKind.Sequential)]
    struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS { public uint TargetProcessId; public int ProcessLoopbackMode; }
    [StructLayout(LayoutKind.Sequential)]
    struct AUDIOCLIENT_ACTIVATION_PARAMS { public int ActivationType; public AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams; }
    // Minimal nachgebautes PROPVARIANT (nur der VT_BLOB-Zweig), da ActivateAudioInterfaceAsync
    // die Aktivierungsparameter als PROPVARIANT-verpackten Byte-Blob erwartet.
    [StructLayout(LayoutKind.Sequential)]
    struct PROPVARIANT_BLOB { public ushort vt, wReserved1, wReserved2, wReserved3; public uint blobSize; public IntPtr blobData; }

    static readonly Guid IID_IAudioClient = new Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");

    // Aktiviert WASAPI-Loopback fuer GENAU einen Prozess (samt Kindprozesse). Wirft bei
    // jedem Problem (aeltere Windows-Version, Aktivierung schlaegt fehl, Prozess weg
    // usw.) - der Aufrufer faengt das ab und faellt auf System-weites Loopback zurueck,
    // damit im schlimmsten Fall wieder der bisherige Zustand gilt (Ton laeuft, nur
    // ungefiltert) statt gar kein Ton mehr.
    static NAudio.CoreAudioApi.AudioClient ActivateProcessLoopback(uint pid)
    {
        var acParams = new AUDIOCLIENT_ACTIVATION_PARAMS
        {
            ActivationType = 1,   // AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
            ProcessLoopbackParams = new AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS { TargetProcessId = pid, ProcessLoopbackMode = 0 },   // 0 = inkl. Kindprozesse
        };
        int paramsSize = Marshal.SizeOf<AUDIOCLIENT_ACTIVATION_PARAMS>();
        IntPtr paramsPtr = Marshal.AllocHGlobal(paramsSize);
        IntPtr propPtr = Marshal.AllocHGlobal(Marshal.SizeOf<PROPVARIANT_BLOB>());
        try
        {
            Marshal.StructureToPtr(acParams, paramsPtr, false);
            var prop = new PROPVARIANT_BLOB { vt = 0x41 /* VT_BLOB */, blobSize = (uint)paramsSize, blobData = paramsPtr };
            Marshal.StructureToPtr(prop, propPtr, false);

            var handler = new AudioActivationHandler();
            ActivateAudioInterfaceAsync(@"VAD\Process_Loopback", IID_IAudioClient, propPtr, handler, out _);
            if (!handler.Done.WaitOne(4000)) throw new TimeoutException("Aktivierung (Process-Loopback) antwortet nicht");
            handler.Operation.GetActivateResult(out int hr, out object iface);
            if (hr != 0) throw new COMException("ActivateAudioInterfaceAsync fehlgeschlagen", hr);
            return new NAudio.CoreAudioApi.AudioClient((NAudio.CoreAudioApi.Interfaces.IAudioClient)iface);
        }
        finally { Marshal.FreeHGlobal(paramsPtr); Marshal.FreeHGlobal(propPtr); }
    }

    // Audio-Modus: PCM auf stdout, das Format auf stderr ("AUDIO <rate> <channels>
    // <bits> <f|i>"). Lumora haengt das als zweiten Eingang an FFmpeg (-> Opus fuer
    // mediamtx). Im Fenster-Modus (hwnd gesetzt) wird NUR der Ton des zugehoerigen
    // Prozesses aufgenommen (siehe ActivateProcessLoopback); im Monitor-Modus (hwnd
    // IntPtr.Zero) bleibt es beim bisherigen System-weiten Loopback - dort ist das
    // korrekt, weil ja der GANZE Bildschirm geteilt wird.
    static int RunAudio(IntPtr hwnd)
    {
        try
        {
            NAudio.CoreAudioApi.AudioClient procClient = null;
            NAudio.CoreAudioApi.AudioCaptureClient procCap = null;
            NAudio.Wave.WasapiLoopbackCapture sysCapture = null;
            NAudio.Wave.WaveFormat procFmt = null;
            if (hwnd != IntPtr.Zero)
            {
                // Jeder Schritt einzeln geloggt (statt nur der Gesamt-Fehlermeldung) - die
                // erste Runde zeigte nur "The method or operation is not implemented" ohne
                // erkennbar, WELCHER Aufruf das war. Fakten statt raten.
                string step = "start";
                try
                {
                    step = "GetWindowThreadProcessId";
                    GetWindowThreadProcessId(hwnd, out uint pid);
                    if (pid == 0) throw new Exception("keine Prozess-ID ermittelt");
                    step = "ActivateProcessLoopback";
                    procClient = ActivateProcessLoopback(pid);
                    // GetMixFormat() ist beim virtuellen Process-Loopback-Geraet NICHT
                    // zuverlaessig implementiert (liefert auf manchen Systemen E_NOTIMPL,
                    // in .NET als NotImplementedException sichtbar). Es gibt kein echtes
                    // Geraet zum Erfragen, daher fest das ueberall unterstuetzte
                    // Standardformat (f32/48kHz/stereo).
                    procFmt = NAudio.Wave.WaveFormat.CreateIeeeFloatWaveFormat(48000, 2);
                    step = "Initialize";
                    procClient.Initialize(NAudio.CoreAudioApi.AudioClientShareMode.Shared, NAudio.CoreAudioApi.AudioClientStreamFlags.Loopback, 200000 /* 20 ms, in 100-ns-Einheiten */, 0, procFmt, Guid.Empty);
                    step = "AudioCaptureClient";
                    procCap = procClient.AudioCaptureClient;
                    Console.Error.WriteLine("AUDIOSRC prozessgebunden (pid " + pid + ")");
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine("AUDPROCFALLBACK [" + step + "] " + ex.GetType().Name + ": " + ex.Message);
                    try { procClient?.Dispose(); } catch { }
                    procClient = null; procCap = null; procFmt = null;
                }
            }
            if (procClient == null) { sysCapture = new NAudio.Wave.WasapiLoopbackCapture(); Console.Error.WriteLine("AUDIOSRC system"); }

            var fmt = procClient != null ? procFmt : sysCapture.WaveFormat;
            // FFmpeg ist auf der Empfangsseite FEST auf f32/48kHz/stereo verdrahtet
            // (pipe:3). Das System-Loopback liefert aber das MIX-Format des Standard-
            // Ausgabegeraets - auf 44,1-kHz-Geraeten (Hi-Fi-DACs, AVR) waere der Ton
            // verstimmt und liefe dem Bild davon, bei 5.1/7.1 waere er unbrauchbar
            // (Audit-Befund). Weicht das Geraeteformat ab, wird deshalb VOR der Queue
            // per MediaFoundation-Resampler auf f32/48k/stereo gewandelt; Writer und
            // Echtzeit-Byte-Budget arbeiten dann wie gehabt auf dem Zielformat.
            var outFmt = NAudio.Wave.WaveFormat.CreateIeeeFloatWaveFormat(48000, 2);
            NAudio.Wave.BufferedWaveProvider convBuf = null;
            NAudio.Wave.MediaFoundationResampler conv = null;
            if (sysCapture != null && (fmt.SampleRate != 48000 || fmt.Channels != 2 || fmt.Encoding != NAudio.Wave.WaveFormatEncoding.IeeeFloat))
            {
                try
                {
                    // ReadFully=false: der Resampler liefert nur, was aus den bisher
                    // zugefuehrten Samples entsteht - KEIN Nullen-Padding (das wuerde
                    // Stille in den laufenden Ton mischen, die 2.2.8-Falle).
                    convBuf = new NAudio.Wave.BufferedWaveProvider(fmt) { ReadFully = false, BufferDuration = TimeSpan.FromSeconds(2), DiscardOnBufferOverflow = true };
                    conv = new NAudio.Wave.MediaFoundationResampler(convBuf, outFmt);
                    Console.Error.WriteLine($"AUDIOCONV {fmt.SampleRate}Hz/{fmt.Channels}ch/{fmt.Encoding} -> 48000Hz/2ch/f32");
                }
                catch (Exception ex)
                {
                    // Kein MediaFoundation? Dann wie bisher roh liefern (auf 48k/2-
                    // Systemen korrekt; die Meldung macht den Sonderfall im Log sichtbar).
                    Console.Error.WriteLine("AUDIOCONV FEHLER: " + ex.Message + " - liefere Geraeteformat");
                    try { conv?.Dispose(); } catch { }
                    conv = null; convBuf = null;
                }
            }
            var outInfo = conv != null ? outFmt : fmt;
            Console.Error.WriteLine($"AUDIO {outInfo.SampleRate} {outInfo.Channels} {outInfo.BitsPerSample} {(outInfo.Encoding == NAudio.Wave.WaveFormatEncoding.IeeeFloat ? "f" : "i")}");
            Console.Error.Flush();
            var stdout = Console.OpenStandardOutput();
            // Producer-Consumer: Der WASAPI-Thread darf NICHT im stdout-Write
            // blockieren – sonst verwirft Windows Audio-Samples (Aussetzer). Er
            // legt die Buffer nur in eine Queue; ein eigener Writer-Thread gibt
            // sie aus. Staut sich stdout, waechst die Queue kurz (statt WASAPI zu
            // blockieren); erst bei laengerem Stau wird verworfen.
            var queue = new System.Collections.Concurrent.BlockingCollection<byte[]>(512);
            bool broken = false;
            if (sysCapture != null)
            {
                sysCapture.DataAvailable += (s, e) =>
                {
                    if (broken || e.BytesRecorded <= 0) return;
                    if (conv != null)
                    {
                        // Geraeteformat -> f32/48k/stereo (s. Konverter-Setup oben).
                        convBuf.AddSamples(e.Buffer, 0, e.BytesRecorded);
                        var tmp = new byte[e.BytesRecorded * 4 + 65536];
                        int n = conv.Read(tmp, 0, tmp.Length / 8 * 8);
                        if (n > 0) { var outB = new byte[n]; Array.Copy(tmp, outB, n); queue.TryAdd(outB); }
                        return;
                    }
                    var buf = new byte[e.BytesRecorded];
                    Array.Copy(e.Buffer, buf, e.BytesRecorded);
                    queue.TryAdd(buf);
                };
                sysCapture.RecordingStopped += (s, e) => { try { queue.CompleteAdding(); } catch { } };
            }
            // A/V-SYNC-KERN: FFmpeg taktet den Rohton durch reines ZAEHLEN der Samples.
            // Bild und Ton bleiben also nur synchron, wenn wir pro realer Sekunde EXAKT
            // bps Bytes liefern. Frueher (Pausen-Schaetz-Logik) blieb jeder Fehler -
            // Schaetzfehler beim Pausenfuellen, unter Last verworfene WASAPI-Chunks,
            // Soundkarten-Uhr-Drift - FUER IMMER als wachsender Versatz stehen (bei zwei
            // parallelen Streams schnell hoerbar). Jetzt: ECHTZEIT-BYTE-BUDGET.
            //   budget(t) = (Stoppuhr seit dem ERSTEN echten Chunk) * bps
            // Liegt das Geschriebene mehr als 80 ms unter dem Budget, wird die Luecke
            // (bis auf 40 ms Reserve) mit Stille aufgefuellt - egal, WODURCH sie entstand.
            // Jeder Fehler ist damit nach spaetestens ~100 ms wieder ausgeglichen statt
            // sich zu addieren. Die 2.2.8-Falle (zyklische Stille im LAUFENDEN Ton, weil
            // gegen eine absolute Uhr ab Prozessstart gefuellt wurde) ist konstruktiv
            // vermieden: Nullpunkt ist der erste Chunk (die konstante WASAPI-Pipeline-
            // Latenz steckt damit im Nullpunkt und kuerzt sich heraus), und die 80-ms-
            // Hysterese liegt weit ueber dem normalen Chunk-Abstand (~10 ms).
            // Laeuft die Soundkarten-Uhr dauerhaft VOR (Budget-Ueberschuss > 500 ms),
            // werden ganze Chunks ausgelassen, bis der Ueberschuss abgebaut ist -
            // passiert nur bei starkem Uhren-Drift und in seltenen Einzelschritten.
            int bps = outInfo.AverageBytesPerSecond;              // 48000*2*4 = 384000 (Format NACH Konvertierung)
            int align = Math.Max(1, outInfo.BlockAlign);          // NIE mitten im Sample-Frame schneiden (Kanal-Versatz!)
            byte[] silence = new byte[Math.Max(align, bps / 50 / align * align)];   // 20-ms-Haeppchen, align-gerundet
            var writer = new Thread(() =>
            {
                try
                {
                    var clock = new System.Diagnostics.Stopwatch();
                    var startWait = System.Diagnostics.Stopwatch.StartNew();   // Anlaufzeit bis Stille-Notstart
                    long written = 0;
                    bool started = false;
                    long filledTotal = 0;
                    // inFill: Pause ist BESTAETIGT (150-ms-Hysterese einmal ueberschritten).
                    // Ab dann wird bei jedem 15-ms-Tick kontinuierlich in kleinen Haeppchen
                    // bis zur 60-ms-Reserve nachgefuellt, statt jeweils erneut auf 150 ms
                    // Defizit zu warten. WICHTIG (Latenz beim Zuschauer, faktisch belegt bei
                    // YouTube-Pausen): 90-ms-Stille-BURSTS liessen den adaptiven Abspielpuffer
                    // des Empfaenger-Browsers anwachsen - und der baut sich kaum wieder ab.
                    // Ein kontinuierlicher Fluss haelt das Paket-Timing glatt. Die MENGEN-
                    // Rechnung (Budget) und die 150-ms-Schutz-Hysterese fuer den laufenden
                    // Ton bleiben unveraendert - nur die Ausgabe-Kadenz der Stille aendert sich.
                    bool inFill = false;
                    long fillStartTotal = 0;
                    while (!broken)
                    {
                        if (queue.TryTake(out var buf, 15))
                        {
                            if (!started) { started = true; clock.Restart(); }
                            if (inFill)
                            {
                                inFill = false;
                                Console.Error.WriteLine($"SIL {(filledTotal - fillStartTotal) * 1000 / bps}ms (gesamt {filledTotal * 1000 / bps}ms)");
                            }
                            // VORLAUF-Selbstkorrektur: Wurde faelschlich gefuellt (Lieferstau
                            // sah wie eine Luecke aus) oder laeuft die Karten-Uhr vor, liegt
                            // written ueber dem Budget. Dann den Anfang des GESTAUTEN Chunks
                            // beschneiden (align-gerecht, bis auf 20 ms Rest) - der Vorlauf
                            // baut sich damit sofort wieder ab, statt dauerhaft zu bleiben.
                            long ahead = written - (long)(clock.Elapsed.TotalSeconds * bps);
                            if (ahead > bps / 10)   // > 100 ms
                            {
                                int cut = (int)Math.Min((long)(buf.Length - align), (ahead - bps / 50) / align * align);
                                if (cut > 0)
                                {
                                    stdout.Write(buf, cut, buf.Length - cut);
                                    written += buf.Length - cut;
                                    Console.Error.WriteLine($"CUT {cut * 1000 / bps}ms (Vorlauf-Abbau)");
                                    continue;
                                }
                            }
                            stdout.Write(buf, 0, buf.Length);
                            written += buf.Length;
                            continue;
                        }
                        if (queue.IsAddingCompleted) break;        // Recording gestoppt & leer
                        if (!started)
                        {
                            // Kommt binnen 200 ms KEIN echter Ton (das Ausgabegeraet spielt nichts ab
                            // -> WASAPI-Loopback feuert DataAvailable GAR NICHT), selbst mit Stille als
                            // Nullpunkt beginnen. Sonst wartet FFmpeg ewig auf den ersten Audio-Frame
                            // und der GANZE Bild-Stream blockiert -> der Bildschirm-Stream startet bei
                            // Stille nicht. Sobald echter Ton kommt, laeuft die Budget-Logik ab hier.
                            if (startWait.ElapsedMilliseconds < 200) continue;
                            started = true; clock.Restart();
                        }
                        // 150 ms Hysterese (praxiserprobter Wert) fuer den EINSTIEG in eine
                        // Pause: Lieferstaus unterhalb davon sind normale Latenzspitzen unter
                        // Last - dort NICHT fuellen, sonst entstuende genau dann ein hoerbarer
                        // Mini-Aussetzer, wenn die gestauten Samples doch noch eintreffen.
                        long target = (long)(clock.Elapsed.TotalSeconds * bps) / align * align;
                        long deficit = target - written;
                        if (!inFill && deficit >= bps * 150 / 1000) { inFill = true; fillStartTotal = filledTotal; }
                        if (inFill && deficit > bps * 60 / 1000)
                        {
                            long fill = (deficit - bps * 60 / 1000) / align * align;
                            long filled = 0;
                            while (filled + silence.Length <= fill) { stdout.Write(silence, 0, silence.Length); filled += silence.Length; }
                            written += filled;
                            filledTotal += filled;
                        }
                    }
                }
                catch { broken = true; try { queue.CompleteAdding(); } catch { } }
            }) { IsBackground = true };
            Thread capThread = null;
            if (sysCapture != null) sysCapture.StartRecording();
            else
            {
                // Prozessgebundener Weg: NAudio hat hier keinen DataAvailable-Event-Mechanismus
                // (der existiert nur fuer die MMDevice-basierten WasapiCapture-Klassen) - darum
                // ein eigener Poll-Thread, der 1:1 in dieselbe Queue liefert wie oben beim
                // System-Loopback, damit die Stille-Fuell-Logik im Writer unveraendert bleibt.
                procClient.Start();
                int bytesPerFrame = fmt.BlockAlign;
                capThread = new Thread(() =>
                {
                    try
                    {
                        while (!broken)
                        {
                            int packetSize = procCap.GetNextPacketSize();
                            if (packetSize == 0) { Thread.Sleep(5); continue; }
                            while (packetSize != 0)
                            {
                                IntPtr p = procCap.GetBuffer(out int numFrames, out var flags);
                                int nbytes = numFrames * bytesPerFrame;
                                if (nbytes > 0)
                                {
                                    var data = new byte[nbytes];
                                    if ((flags & NAudio.CoreAudioApi.AudioClientBufferFlags.Silent) == 0) Marshal.Copy(p, data, 0, nbytes);
                                    queue.TryAdd(data);
                                }
                                procCap.ReleaseBuffer(numFrames);
                                packetSize = procCap.GetNextPacketSize();
                            }
                        }
                    }
                    catch { }
                    finally { try { queue.CompleteAdding(); } catch { } }
                }) { IsBackground = true };
                capThread.Start();
            }
            writer.Start();
            writer.Join();   // laeuft, bis stdout bricht (FFmpeg weg) oder Recording stoppt
            broken = true;   // capThread (falls aktiv) ueber die Poll-Schleife hinaus beenden
            try { capThread?.Join(1000); } catch { }   // erst sauber auslaufen lassen, dann erst freigeben
            try { sysCapture?.StopRecording(); sysCapture?.Dispose(); } catch { }
            try { procClient?.Stop(); procClient?.Dispose(); } catch { }
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine("ERR audio " + ex.Message); return 1; }
    }
}
