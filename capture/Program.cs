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
    static int _width, _height;       // Content-Groesse (WGC)
    static int _outW, _outH;          // Ausgabe-Groesse (nach GPU-Downscale)
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

    static int Main(string[] args)
    {
        try
        {
            try { timeBeginPeriod(1); } catch { }   // praezises Thread.Sleep fuer sauberes CFR
            if (Array.IndexOf(args, "--audio") >= 0) return RunAudio();   // Audio-Modus (WASAPI-Loopback)
            IntPtr hwnd = IntPtr.Zero;
            int fps = 60, maxHeight = 0;
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--hwnd" && i + 1 < args.Length) hwnd = (IntPtr)long.Parse(args[++i]);
                else if (args[i] == "--title" && i + 1 < args.Length) hwnd = FindByTitle(args[++i]);
                else if (args[i] == "--fps" && i + 1 < args.Length) fps = int.Parse(args[++i]);
                else if (args[i] == "--max-height" && i + 1 < args.Length) maxHeight = int.Parse(args[++i]);
            }
            if (hwnd == IntPtr.Zero) { Console.Error.WriteLine("ERR kein Fenster gefunden"); return 2; }
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

            // GraphicsCaptureItem fuer das Fenster. Ist der HWND ungueltig (Fenster
            // inzwischen geschlossen), wirft CreateItemForWindow eine Exception
            // ("Value does not fall within the expected range") -> abfangen und KLAR
            // melden statt kryptisch abzustuerzen.
            GraphicsCaptureItem item;
            try { item = CreateItemForWindow(hwnd); }
            catch { Console.Error.WriteLine("ERR Fenster nicht mehr verfuegbar"); return 4; }
            if (item == null) { Console.Error.WriteLine("ERR Fenster nicht mehr verfuegbar"); return 4; }
            _width = item.Size.Width; _height = item.Size.Height;
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
                if (tH == maxHeight) { _numMips = m; _outW = tW; _outH = tH; }   // exakt -> Mipmap
                else _useVp = true;                                              // krumm -> Video-Processor
            }

            Console.Error.WriteLine($"SIZE {_outW} {_outH}");
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
            var framePool = Direct3D11CaptureFramePool.Create(winrtDevice, fmt, 2, new SizeInt32(_width, _height));
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
            var stdout = Console.OpenStandardOutput();
            double interval = 1000.0 / fps;
            var sw = System.Diagnostics.Stopwatch.StartNew();
            double next = 0;
            byte[] frameBuf = null;
            while (_running)
            {
                // Praezise bis zum naechsten Tick warten: grob per Sleep(1) (dank
                // timeBeginPeriod ~1 ms genau), Feinschliff per SpinWait -> sauberes
                // CFR auch bei 120 fps (8 ms Takt) statt grobem 15-ms-Sleep-Jitter.
                double now;
                while ((now = sw.Elapsed.TotalMilliseconds) < next - 0.3)
                {
                    if (next - now > 1.5) Thread.Sleep(1); else Thread.SpinWait(200);
                }
                next += interval;
                if (now > next + interval * 4) next = now;   // grosser Rueckstand -> Takt neu setzen

                // Alle aufgestauten Frames abholen, nur das neueste verarbeiten.
                Direct3D11CaptureFrame latest = null, f;
                while ((f = framePool.TryGetNextFrame()) != null) { latest?.Dispose(); latest = f; }
                if (latest != null)
                {
                    // Fenster-Groessenaenderung (z.B. Video -> Vollbild)? FramePool neu
                    // anlegen (sonst croppt WGC auf die alte Groesse) und auf den
                    // Letterbox-Scaler umschalten. Kleine Wackler (<=1 px) ignorieren.
                    var cs = latest.ContentSize;
                    if (cs.Width > 1 && cs.Height > 1 && (Math.Abs(cs.Width - _curW) > 1 || Math.Abs(cs.Height - _curH) > 1))
                    {
                        _curW = cs.Width; _curH = cs.Height;
                        try { framePool.Recreate(winrtDevice, fmt, 2, new SizeInt32(_curW, _curH)); } catch { }
                        try { ConfigureLetterbox(hdr); }
                        catch (Exception ex) { if (!_vpLogged) { _vpLogged = true; Console.Error.WriteLine("RSZERR " + ex.Message); } }
                        if (frameBuf != null) Array.Clear(frameBuf, 0, frameBuf.Length);   // Raender schwarz
                        latest.Dispose();
                        continue;   // naechstes Frame kommt in der neuen Groesse
                    }
                    if (frameBuf == null) frameBuf = new byte[_outW * 4 * _outH];
                    try { CopyFrameInto(latest, frameBuf); } catch { }
                    latest.Dispose();
                }
                if (frameBuf != null) { try { stdout.Write(frameBuf, 0, frameBuf.Length); stdout.Flush(); } catch { break; } }
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
        using var srcTex = new ID3D11Texture2D(texPtr);
        if (_resized) { CopyFrameLetterbox(srcTex, dst); return; }
        if (_numMips > 0)
        {
            // Content -> mip0, Mip-Kette auf der GPU erzeugen, Ziel-Mip (kleiner)
            // in die Staging-Textur kopieren.
            _context.CopySubresourceRegion(_mipTex, 0, 0, 0, 0, srcTex, 0);
            _context.GenerateMips(_mipSrv);
            _context.CopySubresourceRegion(_staging, 0, 0, 0, 0, _mipTex, (uint)_numMips);
        }
        else if (_useShader)
        {
            RenderShaderTonemap(srcTex);
            _context.CopyResource(_staging, _scaleTex);
        }
        else if (_useVp)
        {
            try
            {
                using var inView = _vdev.CreateVideoProcessorInputView(srcTex, _venum,
                    new VideoProcessorInputViewDescription { FourCC = 0, ViewDimension = VideoProcessorInputViewDimension.Texture2D });
                var stream = new VideoProcessorStream { Enable = true, InputSurface = inView };
                _vctx.VideoProcessorBlt(_vproc, _voutView, 0, 1, new[] { stream });
                _context.CopyResource(_staging, _scaleTex);
            }
            catch (Exception ex) { if (!_vpLogged) { _vpLogged = true; Console.Error.WriteLine("VPERR " + ex.Message); } throw; }
        }
        else _context.CopyResource(_staging, srcTex);
        var map = _context.Map(_staging, 0, Vortice.Direct3D11.MapMode.Read, Vortice.Direct3D11.MapFlags.None);
        int rowBytes = _outW * 4, pitch = (int)map.RowPitch;
        IntPtr src = map.DataPointer;
        if (pitch == rowBytes) Marshal.Copy(src, dst, 0, rowBytes * _outH);   // dicht gepackt -> ein Rutsch
        else for (int y = 0; y < _outH; y++) Marshal.Copy(src + y * pitch, dst, y * rowBytes, rowBytes);
        _context.Unmap(_staging, 0);
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
Texture2D tex : register(t0);
SamplerState samp : register(s0);
void vsmain(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 psmain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 c = max(tex.Sample(samp, uv).rgb, 0.0);
    float3 x = c / 2.54;
    x = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
    x = saturate(x);
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
    }
    static void RenderShaderTonemap(ID3D11Texture2D src)
    {
        _context.CopyResource(_srcCopy, src);   // WGC-Textur -> ShaderResource-faehige Kopie
        using var srv = _device.CreateShaderResourceView(_srcCopy);
        _context.VSSetShader(_vs);
        _context.PSSetShader(_ps);
        _context.PSSetShaderResource(0, srv);
        _context.PSSetSampler(0, _sampler);
        _context.OMSetRenderTargets(_rtv);
        _context.RSSetViewport(0, 0, _outW, _outH);
        _context.IASetPrimitiveTopology(PrimitiveTopology.TriangleList);
        _context.Draw(3, 0);
        _context.OMSetRenderTargets((ID3D11RenderTargetView)null);   // RTV loesen fuer die Kopie danach
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

    // Audio-Modus: System-Audio per WASAPI-Loopback als PCM auf stdout, das Format
    // auf stderr ("AUDIO <rate> <channels> <bits> <f|i>"). Lumora haengt das als
    // zweiten Eingang an FFmpeg (-> Opus fuer mediamtx).
    static int RunAudio()
    {
        try
        {
            var capture = new NAudio.Wave.WasapiLoopbackCapture();
            var fmt = capture.WaveFormat;
            Console.Error.WriteLine($"AUDIO {fmt.SampleRate} {fmt.Channels} {fmt.BitsPerSample} {(fmt.Encoding == NAudio.Wave.WaveFormatEncoding.IeeeFloat ? "f" : "i")}");
            Console.Error.Flush();
            var stdout = Console.OpenStandardOutput();
            // Producer-Consumer: Der WASAPI-Thread darf NICHT im stdout-Write
            // blockieren – sonst verwirft Windows Audio-Samples (Aussetzer). Er
            // legt die Buffer nur in eine Queue; ein eigener Writer-Thread gibt
            // sie aus. Staut sich stdout, waechst die Queue kurz (statt WASAPI zu
            // blockieren); erst bei laengerem Stau wird verworfen.
            var queue = new System.Collections.Concurrent.BlockingCollection<byte[]>(512);
            bool broken = false;
            capture.DataAvailable += (s, e) =>
            {
                if (broken || e.BytesRecorded <= 0) return;
                var buf = new byte[e.BytesRecorded];
                Array.Copy(e.Buffer, buf, e.BytesRecorded);
                queue.TryAdd(buf);
            };
            capture.RecordingStopped += (s, e) => { try { queue.CompleteAdding(); } catch { } };
            // WICHTIG: WasapiLoopbackCapture feuert bei STILLE keine DataAvailable-
            // Events. Ohne Daten blockiert FFmpeg beim Lesen von fd 3 und muxt kein
            // Video mehr -> die Video-Pipe laeuft voll, der Video-Helfer haengt im
            // stdout.Write, der ganze Stream friert ein (mediamtx-Timeout -> "Kein
            // Stream"). Darum GARANTIERT der Writer einen lueckenlosen Strom: fehlt
            // echtes Audio, schiebt er Stille (Nullen) nach, bis der Byte-Zaehler die
            // Echtzeit-Uhr eingeholt hat. Echtes Audio zaehlt gleichwertig mit -> kein
            // A/V-Drift. Die Toleranz (~150 ms) faengt die normale WASAPI-Puffer-Latenz
            // ab, damit bei LAUFENDEM Ton keine Stille eingemischt wird.
            int bps = fmt.AverageBytesPerSecond;                  // 48000*2*4 = 384000
            byte[] silence = new byte[Math.Max(2, bps / 50)];     // 20-ms-Haeppchen
            long tol = bps / 6;                                   // ~150 ms Toleranz
            var swAud = System.Diagnostics.Stopwatch.StartNew();
            long written = 0;
            var writer = new Thread(() =>
            {
                try
                {
                    while (!broken)
                    {
                        if (queue.TryTake(out var buf, 15))
                        {
                            stdout.Write(buf, 0, buf.Length);
                            written += buf.Length;
                        }
                        else if (queue.IsAddingCompleted) break;   // Recording gestoppt & leer
                        long expected = (long)(swAud.Elapsed.TotalSeconds * bps);
                        if (expected - written > tol)              // echte Stille-Luecke
                            while (expected - written >= silence.Length)
                            {
                                stdout.Write(silence, 0, silence.Length);
                                written += silence.Length;
                            }
                    }
                }
                catch { broken = true; try { queue.CompleteAdding(); } catch { } }
            }) { IsBackground = true };
            capture.StartRecording();
            writer.Start();
            writer.Join();   // laeuft, bis stdout bricht (FFmpeg weg) oder Recording stoppt
            try { capture.StopRecording(); capture.Dispose(); } catch { }
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine("ERR audio " + ex.Message); return 1; }
    }
}
