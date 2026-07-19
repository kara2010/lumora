// PoC-B: komplette NVIDIA-Kette in C++ - echtes WGC-Bildschirm-Capture (Windows.
// Graphics.Capture) -> D3D11-Textur -> NVENC (Zero-Copy, kein CPU-Readback) -> H.264.
// Erfolg = poc_wgc.h264, das ffmpeg fehlerfrei dekodiert und den echten Desktop zeigt.
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
#include <thread>
#include <chrono>
#include "nvEncodeAPI.h"

namespace wg {
    using namespace winrt::Windows::Graphics;
    using namespace winrt::Windows::Graphics::Capture;
    using namespace winrt::Windows::Graphics::DirectX;
    using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
}

static NV_ENCODE_API_FUNCTION_LIST g_nv = { NV_ENCODE_API_FUNCTION_LIST_VER };
#define NVOK(call) do { NVENCSTATUS _s = (call); if (_s != NV_ENC_SUCCESS) { \
    printf("FEHLER %s -> NVENCSTATUS %d\n", #call, (int)_s); return 2; } } while(0)

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    if (!wg::GraphicsCaptureSession::IsSupported()) { printf("FEHLER: WGC nicht unterstuetzt\n"); return 1; }

    // --- Direct3D11 ---
    winrt::com_ptr<ID3D11Device> dev; winrt::com_ptr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, dev.put(), &fl, ctx.put());
    if (FAILED(hr)) { printf("FEHLER D3D11CreateDevice 0x%08lX\n", hr); return 1; }

    // WinRT-IDirect3DDevice aus dem DXGI-Device
    auto dxgiDev = dev.as<IDXGIDevice>();
    wg::IDirect3DDevice d3dDevice{ nullptr };
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(),
        reinterpret_cast<::IInspectable**>(winrt::put_abi(d3dDevice))));

    // --- GraphicsCaptureItem fuer den Hauptmonitor ---
    HMONITOR hmon = MonitorFromPoint({ 0,0 }, MONITOR_DEFAULTTOPRIMARY);
    auto interop = winrt::get_activation_factory<wg::GraphicsCaptureItem>().as<IGraphicsCaptureItemInterop>();
    wg::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForMonitor(hmon,
        winrt::guid_of<wg::GraphicsCaptureItem>(), winrt::put_abi(item)));
    auto sz = item.Size();
    int W = sz.Width & ~1, H = sz.Height & ~1;   // gerade Maße (H.264)
    printf("Monitor erfasst: %dx%d\n", W, H);

    auto framePool = wg::Direct3D11CaptureFramePool::CreateFreeThreaded(
        d3dDevice, wg::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, sz);
    auto session = framePool.CreateCaptureSession(item);
    session.StartCapture();
    printf("WGC-Session gestartet.\n");

    // --- eigene NVENC-Input-Textur (GPU->GPU-Copy-Ziel, kein CPU-Readback) ---
    D3D11_TEXTURE2D_DESC td{}; td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> encTex;
    winrt::check_hresult(dev->CreateTexture2D(&td, nullptr, encTex.put()));

    // --- NVENC ---
    HMODULE lib = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!lib) { printf("FEHLER nvEncodeAPI64.dll\n"); return 1; }
    typedef NVENCSTATUS(NVENCAPI* CreateFn)(NV_ENCODE_API_FUNCTION_LIST*);
    auto createInstance = (CreateFn)GetProcAddress(lib, "NvEncodeAPICreateInstance");
    NVOK(createInstance(&g_nv));

    void* enc = nullptr;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{ NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX; op.device = dev.get(); op.apiVersion = NVENCAPI_VERSION;
    NVOK(g_nv.nvEncOpenEncodeSessionEx(&op, &enc));

    NV_ENC_PRESET_CONFIG pc{ NV_ENC_PRESET_CONFIG_VER }; pc.presetCfg.version = NV_ENC_CONFIG_VER;
    NVOK(g_nv.nvEncGetEncodePresetConfigEx(enc, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &pc));
    NV_ENC_INITIALIZE_PARAMS ip{ NV_ENC_INITIALIZE_PARAMS_VER };
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID; ip.presetGUID = NV_ENC_PRESET_P4_GUID;
    ip.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    ip.encodeWidth = W; ip.encodeHeight = H; ip.darWidth = W; ip.darHeight = H;
    ip.frameRateNum = 60; ip.frameRateDen = 1; ip.enablePTD = 1; ip.encodeConfig = &pc.presetCfg;
    NVOK(g_nv.nvEncInitializeEncoder(enc, &ip));
    NV_ENC_CREATE_BITSTREAM_BUFFER cb{ NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    NVOK(g_nv.nvEncCreateBitstreamBuffer(enc, &cb));
    NV_ENC_OUTPUT_PTR outBuf = cb.bitstreamBuffer;
    printf("NVENC bereit: H.264 %dx%d.\n", W, H);

    FILE* f = nullptr; fopen_s(&f, "poc_wgc.h264", "wb");
    size_t totalBytes = 0; int got = 0, tries = 0;
    D3D11_BOX box{ 0, 0, 0, (UINT)W, (UINT)H, 1 };

    while (got < 90 && tries < 1200) {
        auto frame = framePool.TryGetNextFrame();
        if (!frame) { std::this_thread::sleep_for(std::chrono::milliseconds(4)); tries++; continue; }
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> frameTex;
        winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), frameTex.put_void()));
        ctx->CopySubresourceRegion(encTex.get(), 0, 0, 0, 0, frameTex.get(), 0, &box);
        ctx->Flush();

        NV_ENC_REGISTER_RESOURCE rr{ NV_ENC_REGISTER_RESOURCE_VER };
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX; rr.resourceToRegister = encTex.get();
        rr.width = W; rr.height = H; rr.pitch = 0; rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        NVOK(g_nv.nvEncRegisterResource(enc, &rr));
        NV_ENC_MAP_INPUT_RESOURCE mr{ NV_ENC_MAP_INPUT_RESOURCE_VER }; mr.registeredResource = rr.registeredResource;
        NVOK(g_nv.nvEncMapInputResource(enc, &mr));
        NV_ENC_PIC_PARAMS pp{ NV_ENC_PIC_PARAMS_VER };
        pp.inputBuffer = mr.mappedResource; pp.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
        pp.inputWidth = W; pp.inputHeight = H; pp.outputBitstream = outBuf; pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        NVENCSTATUS es = g_nv.nvEncEncodePicture(enc, &pp);
        if (es == NV_ENC_SUCCESS) {
            NV_ENC_LOCK_BITSTREAM lb{ NV_ENC_LOCK_BITSTREAM_VER }; lb.outputBitstream = outBuf;
            if (g_nv.nvEncLockBitstream(enc, &lb) == NV_ENC_SUCCESS) {
                fwrite(lb.bitstreamBufferPtr, 1, lb.bitstreamSizeInBytes, f);
                totalBytes += lb.bitstreamSizeInBytes; g_nv.nvEncUnlockBitstream(enc, outBuf);
            }
            got++;
        } else if (es != NV_ENC_ERR_NEED_MORE_INPUT) { printf("FEHLER EncodePicture %d\n", (int)es); return 2; }
        g_nv.nvEncUnmapInputResource(enc, mr.mappedResource);
        g_nv.nvEncUnregisterResource(enc, rr.registeredResource);
        if (got % 30 == 0 && got) printf("  %d Frames vom Desktop encodiert...\n", got);
    }

    NV_ENC_PIC_PARAMS eos{ NV_ENC_PIC_PARAMS_VER }; eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    g_nv.nvEncEncodePicture(enc, &eos);
    fclose(f);
    g_nv.nvEncDestroyBitstreamBuffer(enc, outBuf); g_nv.nvEncDestroyEncoder(enc);
    session.Close(); framePool.Close();
    printf("OK: %d Desktop-Frames encodiert (%d Polls), poc_wgc.h264 = %zu Bytes.\n", got, tries, totalBytes);
    return (got > 0 && totalBytes > 0) ? 0 : 3;
}
