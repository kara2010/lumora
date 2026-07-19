// lumora-capture (C++, Phase 2 v2): echter Helfer mit Encoder-Abstraktion.
// WGC-Monitor -> VideoProcessor NV12 -> Encoder (NVENC ODER AMF, GPU-Auto-Auswahl,
// Zero-Copy) -> eigener MPEG-TS-Mux -> UDP an mediamtx. LIVE, OHNE FFmpeg.
// Aufruf: lumora-capture [--encoder auto|nvenc|amf] [--fps N] [--bitrate MBIT]
//                        [--mtx-host IP] [--mtx-port P] [--frames N] [--tsout DATEI]
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <d3d11.h>
#include <dxgi.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include "nvEncodeAPI.h"
#include "public/include/core/Factory.h"
#include "public/include/core/Context.h"
#include "public/include/core/Surface.h"
#include "public/include/core/Buffer.h"
#include "public/include/components/Component.h"
#include "public/include/components/VideoEncoderVCE.h"
#pragma comment(lib, "ws2_32.lib")

namespace wg {
    using namespace winrt::Windows::Graphics;
    using namespace winrt::Windows::Graphics::Capture;
    using namespace winrt::Windows::Graphics::DirectX;
    using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
}

// ================= MPEG-TS-Muxer (bewiesen gegen mediamtx) =================
static const int PID_PMT = 0x1000, PID_VIDEO = 0x0100;
static uint8_t g_ccVideo = 0, g_ccPat = 0, g_ccPmt = 0;
static uint32_t crc32_mpeg(const uint8_t* d, int len) { uint32_t c = 0xFFFFFFFF; for (int i = 0; i < len; ++i) { c ^= (uint32_t)d[i] << 24; for (int b = 0; b < 8; ++b) c = (c & 0x80000000) ? (c << 1) ^ 0x04C11DB7 : (c << 1); } return c; }
static void writeTS(std::vector<uint8_t>& out, int pid, bool pusi, uint8_t& cc, const uint8_t* payload, int payloadLen, bool wantPCR, uint64_t pcr) {
    uint8_t pkt[188]; memset(pkt, 0xFF, 188); pkt[0] = 0x47; pkt[1] = (pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F); pkt[2] = pid & 0xFF;
    int afLen = wantPCR ? 7 : 0; bool haveAF = wantPCR; int maxPayload = 188 - 4 - (haveAF ? 1 + afLen : 0);
    int take = payloadLen < maxPayload ? payloadLen : maxPayload; int stuffing = 0;
    if (take >= payloadLen) { int need = maxPayload - take; if (need > 0) { haveAF = true; if (afLen == 0) afLen = 1; stuffing = need; } }
    pkt[3] = (haveAF ? 0x30 : 0x10) | (cc & 0x0F); cc = (cc + 1) & 0x0F; int p = 4;
    if (haveAF) { pkt[p++] = (uint8_t)(afLen + stuffing); pkt[p++] = wantPCR ? 0x10 : 0x00;
        if (wantPCR) { uint64_t base = pcr / 300; uint32_t ext = (uint32_t)(pcr % 300); pkt[p++] = (uint8_t)(base >> 25); pkt[p++] = (uint8_t)(base >> 17); pkt[p++] = (uint8_t)(base >> 9); pkt[p++] = (uint8_t)(base >> 1); pkt[p++] = (uint8_t)(((base & 1) << 7) | 0x7E | ((ext >> 8) & 1)); pkt[p++] = (uint8_t)(ext & 0xFF); }
        for (int i = 0; i < stuffing; ++i) pkt[p++] = 0xFF; }
    memcpy(pkt + p, payload, take); out.insert(out.end(), pkt, pkt + 188); int off = take;
    while (off < payloadLen) { uint8_t k[188]; memset(k, 0xFF, 188); k[0] = 0x47; k[1] = (pid >> 8) & 0x1F; k[2] = pid & 0xFF;
        int rem = payloadLen - off, mp = 184, stf = 0; if (rem < mp) { stf = mp - rem; k[3] = 0x30 | (cc & 0x0F); } else k[3] = 0x10 | (cc & 0x0F); cc = (cc + 1) & 0x0F; int q = 4;
        if (stf > 0) { k[q++] = (uint8_t)(stf - 1); if (stf >= 2) k[q++] = 0x00; for (int i = 0; i < stf - 2; ++i) k[q++] = 0xFF; }
        int t = rem < (188 - q) ? rem : (188 - q); memcpy(k + q, payload + off, t); off += t; out.insert(out.end(), k, k + 188); }
}
static void writePSI(std::vector<uint8_t>& out, int pid, uint8_t& cc, const uint8_t* sec, int n) { uint8_t k[188]; memset(k, 0xFF, 188); k[0] = 0x47; k[1] = 0x40 | ((pid >> 8) & 0x1F); k[2] = pid & 0xFF; k[3] = 0x10 | (cc & 0x0F); cc = (cc + 1) & 0x0F; k[4] = 0x00; memcpy(k + 5, sec, n); out.insert(out.end(), k, k + 188); }
static void buildPAT(std::vector<uint8_t>& o) { uint8_t s[16]; int n = 0; s[n++] = 0; s[n++] = 0xB0; s[n++] = 0x0D; s[n++] = 0; s[n++] = 1; s[n++] = 0xC1; s[n++] = 0; s[n++] = 0; s[n++] = 0; s[n++] = 1; s[n++] = 0xE0 | ((PID_PMT >> 8) & 0x1F); s[n++] = PID_PMT & 0xFF; uint32_t c = crc32_mpeg(s, n); s[n++] = c >> 24; s[n++] = c >> 16; s[n++] = c >> 8; s[n++] = c; writePSI(o, 0, g_ccPat, s, n); }
static void buildPMT(std::vector<uint8_t>& o) { uint8_t s[32]; int n = 0; s[n++] = 2; s[n++] = 0xB0; s[n++] = 0x12; s[n++] = 0; s[n++] = 1; s[n++] = 0xC1; s[n++] = 0; s[n++] = 0; s[n++] = 0xE0 | ((PID_VIDEO >> 8) & 0x1F); s[n++] = PID_VIDEO & 0xFF; s[n++] = 0xF0; s[n++] = 0; s[n++] = 0x1B; s[n++] = 0xE0 | ((PID_VIDEO >> 8) & 0x1F); s[n++] = PID_VIDEO & 0xFF; s[n++] = 0xF0; s[n++] = 0; uint32_t c = crc32_mpeg(s, n); s[n++] = c >> 24; s[n++] = c >> 16; s[n++] = c >> 8; s[n++] = c; writePSI(o, PID_PMT, g_ccPmt, s, n); }
static void writePTS(std::vector<uint8_t>& v, int g, uint64_t t) { v.push_back((uint8_t)((g << 4) | (((t >> 30) & 7) << 1) | 1)); v.push_back((uint8_t)((t >> 22) & 0xFF)); v.push_back((uint8_t)((((t >> 15) & 0x7F) << 1) | 1)); v.push_back((uint8_t)((t >> 7) & 0xFF)); v.push_back((uint8_t)(((t & 0x7F) << 1) | 1)); }

// ================= Encoder-Abstraktion =================
struct Encoder {
    virtual ~Encoder() {}
    virtual bool init(ID3D11Device* dev, int w, int h, int fps, int mbit) = 0;
    virtual void encode(ID3D11Texture2D* nv12, std::vector<std::vector<uint8_t>>& out) = 0; // 0..N fertige H.264-AUs
    virtual const char* name() = 0;
};

struct NvencEncoder : Encoder {
    NV_ENCODE_API_FUNCTION_LIST nv{ NV_ENCODE_API_FUNCTION_LIST_VER }; void* enc = nullptr; NV_ENC_OUTPUT_PTR outBuf = nullptr; int W = 0, H = 0;
    const char* name() override { return "NVENC"; }
    bool init(ID3D11Device* dev, int w, int h, int fps, int mbit) override {
        W = w; H = h; HMODULE lib = LoadLibraryW(L"nvEncodeAPI64.dll"); if (!lib) return false;
        typedef NVENCSTATUS(NVENCAPI* Fn)(NV_ENCODE_API_FUNCTION_LIST*); if (((Fn)GetProcAddress(lib, "NvEncodeAPICreateInstance"))(&nv) != NV_ENC_SUCCESS) return false;
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{ NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER }; op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX; op.device = dev; op.apiVersion = NVENCAPI_VERSION;
        if (nv.nvEncOpenEncodeSessionEx(&op, &enc) != NV_ENC_SUCCESS) return false;
        NV_ENC_PRESET_CONFIG pc{ NV_ENC_PRESET_CONFIG_VER }; pc.presetCfg.version = NV_ENC_CONFIG_VER;
        nv.nvEncGetEncodePresetConfigEx(enc, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &pc);
        pc.presetCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR; pc.presetCfg.rcParams.averageBitRate = mbit * 1000000;
        pc.presetCfg.gopLength = fps * 2; pc.presetCfg.frameIntervalP = 1;
        pc.presetCfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1; pc.presetCfg.encodeCodecConfig.h264Config.idrPeriod = fps * 2;
        NV_ENC_INITIALIZE_PARAMS ip{ NV_ENC_INITIALIZE_PARAMS_VER }; ip.encodeGUID = NV_ENC_CODEC_H264_GUID; ip.presetGUID = NV_ENC_PRESET_P4_GUID; ip.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        ip.encodeWidth = w; ip.encodeHeight = h; ip.darWidth = w; ip.darHeight = h; ip.frameRateNum = fps; ip.frameRateDen = 1; ip.enablePTD = 1; ip.encodeConfig = &pc.presetCfg;
        if (nv.nvEncInitializeEncoder(enc, &ip) != NV_ENC_SUCCESS) return false;
        NV_ENC_CREATE_BITSTREAM_BUFFER cb{ NV_ENC_CREATE_BITSTREAM_BUFFER_VER }; nv.nvEncCreateBitstreamBuffer(enc, &cb); outBuf = cb.bitstreamBuffer; return true;
    }
    void encode(ID3D11Texture2D* nv12, std::vector<std::vector<uint8_t>>& out) override {
        NV_ENC_REGISTER_RESOURCE rr{ NV_ENC_REGISTER_RESOURCE_VER }; rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX; rr.resourceToRegister = nv12; rr.width = W; rr.height = H; rr.pitch = 0; rr.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        if (nv.nvEncRegisterResource(enc, &rr) != NV_ENC_SUCCESS) return;
        NV_ENC_MAP_INPUT_RESOURCE mr{ NV_ENC_MAP_INPUT_RESOURCE_VER }; mr.registeredResource = rr.registeredResource; nv.nvEncMapInputResource(enc, &mr);
        NV_ENC_PIC_PARAMS pp{ NV_ENC_PIC_PARAMS_VER }; pp.inputBuffer = mr.mappedResource; pp.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12; pp.inputWidth = W; pp.inputHeight = H; pp.outputBitstream = outBuf; pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        if (nv.nvEncEncodePicture(enc, &pp) == NV_ENC_SUCCESS) { NV_ENC_LOCK_BITSTREAM lb{ NV_ENC_LOCK_BITSTREAM_VER }; lb.outputBitstream = outBuf;
            if (nv.nvEncLockBitstream(enc, &lb) == NV_ENC_SUCCESS) { out.emplace_back((uint8_t*)lb.bitstreamBufferPtr, (uint8_t*)lb.bitstreamBufferPtr + lb.bitstreamSizeInBytes); nv.nvEncUnlockBitstream(enc, outBuf); } }
        nv.nvEncUnmapInputResource(enc, mr.mappedResource); nv.nvEncUnregisterResource(enc, rr.registeredResource);
    }
};

struct AmfEncoder : Encoder {
    amf::AMFContextPtr context; amf::AMFComponentPtr encoder;
    const char* name() override { return "AMF"; }
    bool init(ID3D11Device* dev, int w, int h, int fps, int mbit) override {
        HMODULE lib = LoadLibraryW(AMF_DLL_NAME); if (!lib) return false;
        auto amfInit = (AMFInit_Fn)GetProcAddress(lib, AMF_INIT_FUNCTION_NAME); if (!amfInit) return false;
        amf::AMFFactory* f = nullptr; if (amfInit(AMF_FULL_VERSION, &f) != AMF_OK) return false;
        if (f->CreateContext(&context) != AMF_OK) return false; if (context->InitDX11(dev) != AMF_OK) return false;
        if (f->CreateComponent(context, AMFVideoEncoderVCE_AVC, &encoder) != AMF_OK) return false;
        encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY);
        encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, (amf_int64)mbit * 1000000);
        encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, ::AMFConstructSize(w, h));
        encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, ::AMFConstructRate(fps, 1));
        encoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, (amf_int64)fps * 2);
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, (amf_int64)fps * 2); // SPS/PPS periodisch (Zuschauer-Einstieg)
        return encoder->Init(amf::AMF_SURFACE_NV12, w, h) == AMF_OK;
    }
    void encode(ID3D11Texture2D* nv12, std::vector<std::vector<uint8_t>>& out) override {
        amf::AMFSurfacePtr surf; if (context->CreateSurfaceFromDX11Native(nv12, &surf, nullptr) != AMF_OK) return;
        encoder->SubmitInput(surf);
        amf::AMFDataPtr data;
        while (encoder->QueryOutput(&data) == AMF_OK && data) { amf::AMFBufferPtr b(data); if (b) out.emplace_back((uint8_t*)b->GetNative(), (uint8_t*)b->GetNative() + b->GetSize()); data = nullptr; }
    }
};

// Groesstes sichtbares Fenster finden (fuer --window), analog zum PoC-C.
struct FindCtx { HWND best = nullptr; long area = 0; HWND self = nullptr; };
static BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    auto* c = (FindCtx*)lp;
    if (h == c->self || !IsWindowVisible(h) || IsIconic(h) || GetWindowTextLengthW(h) == 0) return TRUE;
    int cloaked = 0; DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof(cloaked)); if (cloaked) return TRUE;
    RECT r; if (!GetWindowRect(h, &r)) return TRUE; long w = r.right - r.left, ht = r.bottom - r.top;
    if (w < 400 || ht < 300) return TRUE; long ar = w * ht; if (ar > c->area) { c->area = ar; c->best = h; }
    return TRUE;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int fps = 60, mbit = 20, port = 9998, maxFrames = 0; std::string host = "127.0.0.1", tsout, encName = "auto";
    HWND targetHwnd = nullptr; bool findWindow = false;
    for (int i = 1; i < argc; ++i) { std::string a = argv[i];
        if (a == "--fps" && i + 1 < argc) fps = atoi(argv[++i]); else if (a == "--bitrate" && i + 1 < argc) mbit = atoi(argv[++i]);
        else if (a == "--mtx-host" && i + 1 < argc) host = argv[++i]; else if (a == "--mtx-port" && i + 1 < argc) port = atoi(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) maxFrames = atoi(argv[++i]); else if (a == "--tsout" && i + 1 < argc) tsout = argv[++i];
        else if (a == "--encoder" && i + 1 < argc) encName = argv[++i];
        else if (a == "--hwnd" && i + 1 < argc) targetHwnd = (HWND)(uintptr_t)strtoull(argv[++i], nullptr, 0);
        else if (a == "--window") findWindow = true; }
    if (findWindow && !targetHwnd) { FindCtx fc; fc.self = GetConsoleWindow(); EnumWindows(enumProc, (LPARAM)&fc); targetHwnd = fc.best; }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (!wg::GraphicsCaptureSession::IsSupported()) { printf("WGC nicht unterstuetzt\n"); return 1; }

    // GPU/Encoder-Auswahl: passenden Adapter finden (NVENC->NVIDIA, AMF->AMD)
    winrt::com_ptr<IDXGIFactory1> fac; CreateDXGIFactory1(winrt::guid_of<IDXGIFactory1>(), fac.put_void());
    winrt::com_ptr<IDXGIAdapter1> nvA, amdA; for (UINT ai = 0;; ++ai) { winrt::com_ptr<IDXGIAdapter1> a; if (fac->EnumAdapters1(ai, a.put()) == DXGI_ERROR_NOT_FOUND) break; DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d); if (d.VendorId == 0x10DE && !nvA) nvA = a; else if (d.VendorId == 0x1002 && !amdA) amdA = a; }
    bool useAmf = (encName == "amf") || (encName == "auto" && !nvA && amdA);
    winrt::com_ptr<IDXGIAdapter1> pick = useAmf ? amdA : nvA;
    if (!pick) { printf("FEHLER: kein passender GPU-Adapter fuer '%s'\n", encName.c_str()); return 1; }

    winrt::com_ptr<ID3D11Device> dev; winrt::com_ptr<ID3D11DeviceContext> ctx; D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(pick.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, dev.put(), &fl, ctx.put()))) { printf("D3D11CreateDevice fehlgeschlagen\n"); return 1; }
    auto dxgiDev = dev.as<IDXGIDevice>(); wg::IDirect3DDevice d3dDevice{ nullptr };
    CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(), reinterpret_cast<::IInspectable**>(winrt::put_abi(d3dDevice)));
    auto interop = winrt::get_activation_factory<wg::GraphicsCaptureItem>().as<IGraphicsCaptureItemInterop>();
    wg::GraphicsCaptureItem item{ nullptr };
    if (targetHwnd) {
        if (FAILED(interop->CreateForWindow(targetHwnd, winrt::guid_of<wg::GraphicsCaptureItem>(), winrt::put_abi(item)))) { printf("FEHLER: Fenster-Capture fehlgeschlagen (HWND ungueltig?)\n"); return 1; }
        wchar_t t[128] = {}; GetWindowTextW(targetHwnd, t, 127); printf("Quelle: Fenster \"%ls\"\n", t);
    } else {
        HMONITOR hmon = MonitorFromPoint({ 0,0 }, MONITOR_DEFAULTTOPRIMARY);
        interop->CreateForMonitor(hmon, winrt::guid_of<wg::GraphicsCaptureItem>(), winrt::put_abi(item)); printf("Quelle: Hauptmonitor\n");
    }
    auto sz = item.Size(); int W = sz.Width & ~1, H = sz.Height & ~1;
    auto framePool = wg::Direct3D11CaptureFramePool::CreateFreeThreaded(d3dDevice, wg::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, sz);
    auto session = framePool.CreateCaptureSession(item); session.StartCapture();

    auto vdev = dev.as<ID3D11VideoDevice>(); auto vctx = ctx.as<ID3D11VideoContext>();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{}; cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE; cd.InputWidth = W; cd.InputHeight = H; cd.OutputWidth = W; cd.OutputHeight = H; cd.InputFrameRate = { (UINT)fps,1 }; cd.OutputFrameRate = { (UINT)fps,1 }; cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> venum; vdev->CreateVideoProcessorEnumerator(&cd, venum.put());
    winrt::com_ptr<ID3D11VideoProcessor> vproc; vdev->CreateVideoProcessor(venum.get(), 0, vproc.put());
    D3D11_TEXTURE2D_DESC nd{}; nd.Width = W; nd.Height = H; nd.MipLevels = 1; nd.ArraySize = 1; nd.Format = DXGI_FORMAT_NV12; nd.SampleDesc.Count = 1; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> nv12; dev->CreateTexture2D(&nd, nullptr, nv12.put());
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> outView; vdev->CreateVideoProcessorOutputView(nv12.get(), venum.get(), &ovd, outView.put());
    // Eigene BGRA-Textur (W x H, volle Bind-Flags): die WGC-FramePool-Textur wird recycelt,
    // darum sofort hierein kopieren, bevor der naechste Frame sie ueberschreibt.
    D3D11_TEXTURE2D_DESC bd{}; bd.Width = W; bd.Height = H; bd.MipLevels = 1; bd.ArraySize = 1; bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bd.SampleDesc.Count = 1; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> bgraIn; dev->CreateTexture2D(&bd, nullptr, bgraIn.put());

    Encoder* encoder = useAmf ? (Encoder*)new AmfEncoder() : (Encoder*)new NvencEncoder();
    if (!encoder->init(dev.get(), W, H, fps, mbit)) { printf("FEHLER: Encoder-Init (%s) fehlgeschlagen\n", encoder->name()); return 2; }

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons((u_short)port); inet_pton(AF_INET, host.c_str(), &dst.sin_addr);
    DXGI_ADAPTER_DESC1 gd; pick->GetDesc1(&gd);
    printf("lumora-capture: %dx%d @ %dfps, %d Mbit | Encoder %s auf %ls -> mediamtx %s:%d (ohne FFmpeg)\n", W, H, fps, mbit, encoder->name(), gd.Description, host.c_str(), port);

    static const uint8_t AUD[6] = { 0,0,0,1, 0x09, 0xF0 };
    FILE* tsf = nullptr; if (!tsout.empty()) fopen_s(&tsf, tsout.c_str(), "wb");
    uint64_t inFrame = 0, outFrame = 0; int tries = 0; auto t0 = std::chrono::steady_clock::now();
    while ((maxFrames == 0 || (int)outFrame < maxFrames) && tries < 200000) {
        auto f = framePool.TryGetNextFrame();
        if (!f) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); tries++; continue; }
        auto access = f.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> ft; access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), ft.put_void());
        ctx->CopyResource(bgraIn.get(), ft.get()); // sofort in eigene Textur, bevor WGC den FramePool-Buffer recycelt
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{}; ivd.FourCC = 0; ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; ivd.Texture2D.MipSlice = 0;
        winrt::com_ptr<ID3D11VideoProcessorInputView> iv; if (FAILED(vdev->CreateVideoProcessorInputView(bgraIn.get(), venum.get(), &ivd, iv.put()))) continue;
        D3D11_VIDEO_PROCESSOR_STREAM st{}; st.Enable = TRUE; st.pInputSurface = iv.get();
        if (FAILED(vctx->VideoProcessorBlt(vproc.get(), outView.get(), 0, 1, &st))) continue;
        ctx->Flush(); inFrame++;
        std::vector<std::vector<uint8_t>> aus; encoder->encode(nv12.get(), aus);
        for (auto& au : aus) {
            uint64_t pts = 90000 + outFrame * (90000 / fps); std::vector<uint8_t> ts;
            if (outFrame % (fps / 2 ? fps / 2 : 30) == 0) { buildPAT(ts); buildPMT(ts); }
            std::vector<uint8_t> pes; pes.push_back(0); pes.push_back(0); pes.push_back(1); pes.push_back(0xE0); pes.push_back(0); pes.push_back(0); pes.push_back(0x80); pes.push_back(0x80); pes.push_back(5); writePTS(pes, 0x2, pts);
            pes.insert(pes.end(), AUD, AUD + 6); pes.insert(pes.end(), au.begin(), au.end());
            writeTS(ts, PID_VIDEO, true, g_ccVideo, pes.data(), (int)pes.size(), true, pts * 300);
            for (size_t o = 0; o < ts.size(); o += 1316) { int len = (int)((ts.size() - o) < 1316 ? (ts.size() - o) : 1316); sendto(s, (const char*)ts.data() + o, len, 0, (sockaddr*)&dst, sizeof(dst)); }
            if (tsf) fwrite(ts.data(), 1, ts.size(), tsf);
            outFrame++;
            if (outFrame % fps == 0) printf("  %llu Frames live (%.1fs, %s)\n", (unsigned long long)outFrame, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), encoder->name());
        }
    }
    if (tsf) fclose(tsf);
    printf("Ende: %llu in / %llu out Frames (%s).\n", (unsigned long long)inFrame, (unsigned long long)outFrame, encoder->name());
    session.Close(); framePool.Close(); closesocket(s); WSACleanup(); return 0;
}
