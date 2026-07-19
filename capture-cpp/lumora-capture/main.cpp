// lumora-capture (C++, Phase 2 v1): der echte Helfer - fuehrt die bewiesenen Bausteine
// zusammen: WGC-Monitor -> GPU-NV12 (VideoProcessor) -> NVENC (Zero-Copy) -> eigener
// MPEG-TS-Mux -> UDP an mediamtx. LIVE (kontinuierlich), OHNE FFmpeg. NVIDIA zuerst;
// AMF/QSV, Fenster-Capture, Audio und Argument-Parsing wie beim C#-Helfer folgen.
// Aufruf: lumora-capture [--fps N] [--bitrate MBIT] [--mtx-host IP] [--mtx-port P] [--frames N]
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
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
#pragma comment(lib, "ws2_32.lib")

namespace wg {
    using namespace winrt::Windows::Graphics;
    using namespace winrt::Windows::Graphics::Capture;
    using namespace winrt::Windows::Graphics::DirectX;
    using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
}
static NV_ENCODE_API_FUNCTION_LIST g_nv = { NV_ENCODE_API_FUNCTION_LIST_VER };
#define NVOK(call) do { NVENCSTATUS _s = (call); if (_s != NV_ENC_SUCCESS) { printf("NVENC-FEHLER %s -> %d\n", #call, (int)_s); return 2; } } while(0)
#define HROK(call, msg) do { HRESULT _h = (call); if (FAILED(_h)) { printf("FEHLER %s 0x%08lX\n", msg, _h); return 1; } } while(0)

// ---- MPEG-TS-Muxer (aus poc-mpegts, bewiesen gegen mediamtx) ----
static const int PID_PMT = 0x1000, PID_VIDEO = 0x0100;
static uint8_t g_ccVideo = 0, g_ccPat = 0, g_ccPmt = 0;
static uint32_t crc32_mpeg(const uint8_t* d, int len) {
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; ++i) { crc ^= (uint32_t)d[i] << 24; for (int b = 0; b < 8; ++b) crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1); }
    return crc;
}
static void writeTS(std::vector<uint8_t>& out, int pid, bool pusi, uint8_t& cc, const uint8_t* payload, int payloadLen, bool wantPCR, uint64_t pcr) {
    uint8_t pkt[188]; memset(pkt, 0xFF, 188);
    pkt[0] = 0x47; pkt[1] = (pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F); pkt[2] = pid & 0xFF;
    int afLen = wantPCR ? 7 : 0; bool haveAF = wantPCR;
    int maxPayload = 188 - 4 - (haveAF ? 1 + afLen : 0);
    int take = payloadLen < maxPayload ? payloadLen : maxPayload;
    int stuffing = 0;
    if (take >= payloadLen) { int need = maxPayload - take; if (need > 0) { haveAF = true; if (afLen == 0) afLen = 1; stuffing = need; } }
    pkt[3] = (haveAF ? 0x30 : 0x10) | (cc & 0x0F); cc = (cc + 1) & 0x0F;
    int p = 4;
    if (haveAF) {
        pkt[p++] = (uint8_t)(afLen + stuffing);
        pkt[p++] = wantPCR ? 0x10 : 0x00;
        if (wantPCR) { uint64_t base = pcr / 300; uint32_t ext = (uint32_t)(pcr % 300);
            pkt[p++] = (uint8_t)(base >> 25); pkt[p++] = (uint8_t)(base >> 17); pkt[p++] = (uint8_t)(base >> 9); pkt[p++] = (uint8_t)(base >> 1);
            pkt[p++] = (uint8_t)(((base & 1) << 7) | 0x7E | ((ext >> 8) & 1)); pkt[p++] = (uint8_t)(ext & 0xFF); }
        for (int i = 0; i < stuffing; ++i) pkt[p++] = 0xFF;
    }
    memcpy(pkt + p, payload, take); out.insert(out.end(), pkt, pkt + 188);
    int off = take;
    while (off < payloadLen) {
        uint8_t pk2[188]; memset(pk2, 0xFF, 188);
        pk2[0] = 0x47; pk2[1] = (pid >> 8) & 0x1F; pk2[2] = pid & 0xFF;
        int rem = payloadLen - off; int mp = 184; int stf = 0;
        if (rem < mp) { stf = mp - rem; pk2[3] = 0x30 | (cc & 0x0F); } else pk2[3] = 0x10 | (cc & 0x0F);
        cc = (cc + 1) & 0x0F; int q = 4;
        if (stf > 0) { pk2[q++] = (uint8_t)(stf - 1); if (stf >= 2) pk2[q++] = 0x00; for (int i = 0; i < stf - 2; ++i) pk2[q++] = 0xFF; }
        int t2 = rem < (188 - q) ? rem : (188 - q); memcpy(pk2 + q, payload + off, t2); off += t2;
        out.insert(out.end(), pk2, pk2 + 188);
    }
}
static void writePSI(std::vector<uint8_t>& out, int pid, uint8_t& cc, const uint8_t* sec, int secLen) {
    uint8_t pkt[188]; memset(pkt, 0xFF, 188);
    pkt[0] = 0x47; pkt[1] = 0x40 | ((pid >> 8) & 0x1F); pkt[2] = pid & 0xFF;
    pkt[3] = 0x10 | (cc & 0x0F); cc = (cc + 1) & 0x0F; pkt[4] = 0x00; memcpy(pkt + 5, sec, secLen);
    out.insert(out.end(), pkt, pkt + 188);
}
static void buildPAT(std::vector<uint8_t>& out) {
    uint8_t s[16]; int n = 0; s[n++] = 0x00; s[n++] = 0xB0; s[n++] = 0x0D; s[n++] = 0x00; s[n++] = 0x01; s[n++] = 0xC1; s[n++] = 0x00; s[n++] = 0x00;
    s[n++] = 0x00; s[n++] = 0x01; s[n++] = 0xE0 | ((PID_PMT >> 8) & 0x1F); s[n++] = PID_PMT & 0xFF;
    uint32_t c = crc32_mpeg(s, n); s[n++] = c >> 24; s[n++] = c >> 16; s[n++] = c >> 8; s[n++] = c; writePSI(out, 0x0000, g_ccPat, s, n);
}
static void buildPMT(std::vector<uint8_t>& out) {
    uint8_t s[32]; int n = 0; s[n++] = 0x02; s[n++] = 0xB0; s[n++] = 0x12; s[n++] = 0x00; s[n++] = 0x01; s[n++] = 0xC1; s[n++] = 0x00; s[n++] = 0x00;
    s[n++] = 0xE0 | ((PID_VIDEO >> 8) & 0x1F); s[n++] = PID_VIDEO & 0xFF; s[n++] = 0xF0; s[n++] = 0x00;
    s[n++] = 0x1B; s[n++] = 0xE0 | ((PID_VIDEO >> 8) & 0x1F); s[n++] = PID_VIDEO & 0xFF; s[n++] = 0xF0; s[n++] = 0x00;
    uint32_t c = crc32_mpeg(s, n); s[n++] = c >> 24; s[n++] = c >> 16; s[n++] = c >> 8; s[n++] = c; writePSI(out, PID_PMT, g_ccPmt, s, n);
}
static void writePTS(std::vector<uint8_t>& v, int guard, uint64_t ts) {
    v.push_back((uint8_t)((guard << 4) | (((ts >> 30) & 7) << 1) | 1)); v.push_back((uint8_t)((ts >> 22) & 0xFF));
    v.push_back((uint8_t)((((ts >> 15) & 0x7F) << 1) | 1)); v.push_back((uint8_t)((ts >> 7) & 0xFF)); v.push_back((uint8_t)(((ts & 0x7F) << 1) | 1));
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int fps = 60, mbit = 20, port = 9998, maxFrames = 0; std::string host = "127.0.0.1"; std::string tsout;
    for (int i = 1; i < argc; ++i) { std::string a = argv[i];
        if (a == "--fps" && i + 1 < argc) fps = atoi(argv[++i]);
        else if (a == "--bitrate" && i + 1 < argc) mbit = atoi(argv[++i]);
        else if (a == "--mtx-host" && i + 1 < argc) host = argv[++i];
        else if (a == "--mtx-port" && i + 1 < argc) port = atoi(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) maxFrames = atoi(argv[++i]);
        else if (a == "--tsout" && i + 1 < argc) tsout = argv[++i]; }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (!wg::GraphicsCaptureSession::IsSupported()) { printf("WGC nicht unterstuetzt\n"); return 1; }

    winrt::com_ptr<ID3D11Device> dev; winrt::com_ptr<ID3D11DeviceContext> ctx; D3D_FEATURE_LEVEL fl;
    HROK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, dev.put(), &fl, ctx.put()), "D3D11CreateDevice");
    auto dxgiDev = dev.as<IDXGIDevice>();
    wg::IDirect3DDevice d3dDevice{ nullptr };
    HROK(CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(), reinterpret_cast<::IInspectable**>(winrt::put_abi(d3dDevice))), "CreateD3DDevice");
    HMONITOR hmon = MonitorFromPoint({ 0,0 }, MONITOR_DEFAULTTOPRIMARY);
    auto interop = winrt::get_activation_factory<wg::GraphicsCaptureItem>().as<IGraphicsCaptureItemInterop>();
    wg::GraphicsCaptureItem item{ nullptr };
    HROK(interop->CreateForMonitor(hmon, winrt::guid_of<wg::GraphicsCaptureItem>(), winrt::put_abi(item)), "CreateForMonitor");
    auto sz = item.Size(); int W = sz.Width & ~1, H = sz.Height & ~1;
    auto framePool = wg::Direct3D11CaptureFramePool::CreateFreeThreaded(d3dDevice, wg::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, sz);
    auto session = framePool.CreateCaptureSession(item); session.StartCapture();

    // VideoProcessor BGRA->NV12
    auto vdev = dev.as<ID3D11VideoDevice>(); auto vctx = ctx.as<ID3D11VideoContext>();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{}; cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = W; cd.InputHeight = H; cd.OutputWidth = W; cd.OutputHeight = H; cd.InputFrameRate = { (UINT)fps,1 }; cd.OutputFrameRate = { (UINT)fps,1 }; cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> venum; HROK(vdev->CreateVideoProcessorEnumerator(&cd, venum.put()), "VPEnum");
    winrt::com_ptr<ID3D11VideoProcessor> vproc; HROK(vdev->CreateVideoProcessor(venum.get(), 0, vproc.put()), "VProc");
    D3D11_TEXTURE2D_DESC nd{}; nd.Width = W; nd.Height = H; nd.MipLevels = 1; nd.ArraySize = 1; nd.Format = DXGI_FORMAT_NV12; nd.SampleDesc.Count = 1; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> nv12; HROK(dev->CreateTexture2D(&nd, nullptr, nv12.put()), "NV12Tex");
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> outView; HROK(vdev->CreateVideoProcessorOutputView(nv12.get(), venum.get(), &ovd, outView.put()), "OutView");

    // NVENC
    HMODULE lib = LoadLibraryW(L"nvEncodeAPI64.dll"); if (!lib) { printf("nvEncodeAPI64.dll fehlt\n"); return 1; }
    typedef NVENCSTATUS(NVENCAPI* CreateFn)(NV_ENCODE_API_FUNCTION_LIST*);
    NVOK(((CreateFn)GetProcAddress(lib, "NvEncodeAPICreateInstance"))(&g_nv));
    void* enc = nullptr; NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{ NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX; op.device = dev.get(); op.apiVersion = NVENCAPI_VERSION; NVOK(g_nv.nvEncOpenEncodeSessionEx(&op, &enc));
    NV_ENC_PRESET_CONFIG pc{ NV_ENC_PRESET_CONFIG_VER }; pc.presetCfg.version = NV_ENC_CONFIG_VER;
    NVOK(g_nv.nvEncGetEncodePresetConfigEx(enc, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &pc));
    pc.presetCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR; pc.presetCfg.rcParams.averageBitRate = mbit * 1000000;
    pc.presetCfg.gopLength = fps * 2; pc.presetCfg.frameIntervalP = 1;
    // SPS/PPS bei JEDEM Keyframe wiederholen + regelmaessige IDRs - sonst kann ein spaeter
    // einsteigender Zuschauer den Stream nicht dekodieren ("unspecified size").
    pc.presetCfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    pc.presetCfg.encodeCodecConfig.h264Config.idrPeriod = fps * 2;
    NV_ENC_INITIALIZE_PARAMS ip{ NV_ENC_INITIALIZE_PARAMS_VER };
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID; ip.presetGUID = NV_ENC_PRESET_P4_GUID; ip.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    ip.encodeWidth = W; ip.encodeHeight = H; ip.darWidth = W; ip.darHeight = H; ip.frameRateNum = fps; ip.frameRateDen = 1; ip.enablePTD = 1; ip.encodeConfig = &pc.presetCfg;
    NVOK(g_nv.nvEncInitializeEncoder(enc, &ip));
    NV_ENC_CREATE_BITSTREAM_BUFFER cb{ NV_ENC_CREATE_BITSTREAM_BUFFER_VER }; NVOK(g_nv.nvEncCreateBitstreamBuffer(enc, &cb));
    NV_ENC_OUTPUT_PTR outBuf = cb.bitstreamBuffer;

    // UDP zu mediamtx
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons((u_short)port); inet_pton(AF_INET, host.c_str(), &dst.sin_addr);
    printf("lumora-capture: %dx%d @ %dfps, %d Mbit -> mediamtx %s:%d (MPEG-TS/UDP, ohne FFmpeg)\n", W, H, fps, mbit, host.c_str(), port);

    static const uint8_t AUD[6] = { 0,0,0,1, 0x09, 0xF0 };
    FILE* tsf = nullptr; if (!tsout.empty()) fopen_s(&tsf, tsout.c_str(), "wb");
    D3D11_BOX box{ 0,0,0,(UINT)W,(UINT)H,1 };
    uint64_t frame = 0; int tries = 0; auto t0 = std::chrono::steady_clock::now();
    while ((maxFrames == 0 || (int)frame < maxFrames) && tries < 100000) {
        auto f = framePool.TryGetNextFrame();
        if (!f) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); tries++; continue; }
        auto access = f.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> ft; access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), ft.put_void());
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{}; ivd.FourCC = 0; ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; ivd.Texture2D.MipSlice = 0;
        winrt::com_ptr<ID3D11VideoProcessorInputView> iv; if (FAILED(vdev->CreateVideoProcessorInputView(ft.get(), venum.get(), &ivd, iv.put()))) continue;
        D3D11_VIDEO_PROCESSOR_STREAM st{}; st.Enable = TRUE; st.pInputSurface = iv.get();
        if (FAILED(vctx->VideoProcessorBlt(vproc.get(), outView.get(), 0, 1, &st))) continue;
        ctx->Flush();
        NV_ENC_REGISTER_RESOURCE rr{ NV_ENC_REGISTER_RESOURCE_VER }; rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX; rr.resourceToRegister = nv12.get();
        rr.width = W; rr.height = H; rr.pitch = 0; rr.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12; NVOK(g_nv.nvEncRegisterResource(enc, &rr));
        NV_ENC_MAP_INPUT_RESOURCE mr{ NV_ENC_MAP_INPUT_RESOURCE_VER }; mr.registeredResource = rr.registeredResource; NVOK(g_nv.nvEncMapInputResource(enc, &mr));
        NV_ENC_PIC_PARAMS pp{ NV_ENC_PIC_PARAMS_VER }; pp.inputBuffer = mr.mappedResource; pp.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12; pp.inputWidth = W; pp.inputHeight = H; pp.outputBitstream = outBuf; pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        NVENCSTATUS es = g_nv.nvEncEncodePicture(enc, &pp);
        if (es == NV_ENC_SUCCESS) {
            NV_ENC_LOCK_BITSTREAM lb{ NV_ENC_LOCK_BITSTREAM_VER }; lb.outputBitstream = outBuf;
            if (g_nv.nvEncLockBitstream(enc, &lb) == NV_ENC_SUCCESS) {
                uint64_t pts = 90000 + frame * (90000 / fps);
                std::vector<uint8_t> ts;
                if (frame % (fps / 2 ? fps / 2 : 30) == 0) { buildPAT(ts); buildPMT(ts); }
                std::vector<uint8_t> pes;
                pes.push_back(0); pes.push_back(0); pes.push_back(1); pes.push_back(0xE0); pes.push_back(0); pes.push_back(0);
                pes.push_back(0x80); pes.push_back(0x80); pes.push_back(5); writePTS(pes, 0x2, pts);
                pes.insert(pes.end(), AUD, AUD + 6);
                pes.insert(pes.end(), (uint8_t*)lb.bitstreamBufferPtr, (uint8_t*)lb.bitstreamBufferPtr + lb.bitstreamSizeInBytes);
                g_nv.nvEncUnlockBitstream(enc, outBuf);
                writeTS(ts, PID_VIDEO, true, g_ccVideo, pes.data(), (int)pes.size(), true, pts * 300);
                for (size_t o = 0; o < ts.size(); o += 1316) { int len = (int)((ts.size() - o) < 1316 ? (ts.size() - o) : 1316); sendto(s, (const char*)ts.data() + o, len, 0, (sockaddr*)&dst, sizeof(dst)); }
                if (tsf) fwrite(ts.data(), 1, ts.size(), tsf);
                frame++;
                if (frame % fps == 0) printf("  %llu Frames live gesendet (%.1fs)\n", (unsigned long long)frame, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
        }
        g_nv.nvEncUnmapInputResource(enc, mr.mappedResource); g_nv.nvEncUnregisterResource(enc, rr.registeredResource);
    }
    if (tsf) fclose(tsf);
    printf("Ende: %llu Frames.\n", (unsigned long long)frame);
    g_nv.nvEncDestroyBitstreamBuffer(enc, outBuf); g_nv.nvEncDestroyEncoder(enc); session.Close(); framePool.Close(); closesocket(s); WSACleanup();
    return 0;
}
