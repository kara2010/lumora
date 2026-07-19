// PoC-A: Beweist NUR den riskanten Teil - NVENC (API 13.1) nimmt auf der RTX 5080
// (Blackwell) eine Direct3D11-Textur direkt als Input und erzeugt H.264, OHNE
// CPU-Readback. Fuellt eine synthetische BGRA-Textur pro Frame mit wechselnder
// Farbe und encodet sie. Erfolg = gueltige poc.h264-Datei. WGC kommt in PoC-B.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include "nvEncodeAPI.h"

using Microsoft::WRL::ComPtr;

static NV_ENCODE_API_FUNCTION_LIST g_nv = { NV_ENCODE_API_FUNCTION_LIST_VER };

#define NVOK(call) do { NVENCSTATUS _s = (call); if (_s != NV_ENC_SUCCESS) { \
    printf("FEHLER %s -> NVENCSTATUS %d\n", #call, (int)_s); return 2; } } while(0)

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // ungepuffert: jede Zeile sofort sichtbar, auch bei Absturz
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);   // kein haengender Crash-Dialog
    const int W = 1920, H = 1080, FRAMES = 120;

    // --- Direct3D11-Device ---
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &dev, &fl, &ctx);
    if (FAILED(hr)) { printf("FEHLER D3D11CreateDevice hr=0x%08lX\n", hr); return 1; }

    // Eingabetextur (BGRA), als Render-Target zum Fuellen + fuer NVENC-Input.
    D3D11_TEXTURE2D_DESC td{}; td.Width = W; td.Height = H; td.MipLevels = 1;
    td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    hr = dev->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr)) { printf("FEHLER CreateTexture2D hr=0x%08lX\n", hr); return 1; }
    ComPtr<ID3D11RenderTargetView> rtv;
    dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv);

    // --- NVENC laden ---
    HMODULE lib = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!lib) { printf("FEHLER nvEncodeAPI64.dll nicht ladbar (Treiber?)\n"); return 1; }
    typedef NVENCSTATUS(NVENCAPI* CreateInstanceFn)(NV_ENCODE_API_FUNCTION_LIST*);
    auto createInstance = (CreateInstanceFn)GetProcAddress(lib, "NvEncodeAPICreateInstance");
    if (!createInstance) { printf("FEHLER NvEncodeAPICreateInstance fehlt\n"); return 1; }
    NVOK(createInstance(&g_nv));
    printf("NVENC-Funktionsliste geladen.\n");

    // --- Encode-Session auf dem D3D11-Device ---
    void* enc = nullptr;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{ NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    op.device = dev.Get();
    op.apiVersion = NVENCAPI_VERSION;
    NVOK(g_nv.nvEncOpenEncodeSessionEx(&op, &enc));
    printf("Encode-Session offen (DirectX-Device).\n");

    // --- Preset P4 / Ultra-Low-Latency, H.264 ---
    NV_ENC_PRESET_CONFIG pc{ NV_ENC_PRESET_CONFIG_VER };
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    NVOK(g_nv.nvEncGetEncodePresetConfigEx(enc, NV_ENC_CODEC_H264_GUID,
        NV_ENC_PRESET_P4_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &pc));

    NV_ENC_INITIALIZE_PARAMS ip{ NV_ENC_INITIALIZE_PARAMS_VER };
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
    ip.presetGUID = NV_ENC_PRESET_P4_GUID;
    ip.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    ip.encodeWidth = W; ip.encodeHeight = H;
    ip.darWidth = W; ip.darHeight = H;
    ip.frameRateNum = 60; ip.frameRateDen = 1;
    ip.enablePTD = 1;
    ip.encodeConfig = &pc.presetCfg;
    NVOK(g_nv.nvEncInitializeEncoder(enc, &ip));
    printf("Encoder initialisiert: H.264 %dx%d P4/ULL.\n", W, H);

    NV_ENC_CREATE_BITSTREAM_BUFFER cb{ NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    NVOK(g_nv.nvEncCreateBitstreamBuffer(enc, &cb));
    NV_ENC_OUTPUT_PTR outBuf = cb.bitstreamBuffer;

    FILE* f = nullptr; fopen_s(&f, "poc.h264", "wb");
    if (!f) { printf("FEHLER poc.h264 nicht schreibbar\n"); return 1; }
    size_t totalBytes = 0; int encoded = 0;

    auto drain = [&](void) {
        NV_ENC_LOCK_BITSTREAM lb{ NV_ENC_LOCK_BITSTREAM_VER };
        lb.outputBitstream = outBuf;
        if (g_nv.nvEncLockBitstream(enc, &lb) == NV_ENC_SUCCESS) {
            fwrite(lb.bitstreamBufferPtr, 1, lb.bitstreamSizeInBytes, f);
            totalBytes += lb.bitstreamSizeInBytes;
            g_nv.nvEncUnlockBitstream(enc, outBuf);
        }
    };

    for (int i = 0; i < FRAMES; ++i) {
        if (i % 30 == 0) printf("  Frame %d...\n", i);
        float col[4] = { (i % 60) / 60.0f, 0.2f, 1.0f - (i % 60) / 60.0f, 1.0f };
        ctx->ClearRenderTargetView(rtv.Get(), col);
        ctx->Flush();

        NV_ENC_REGISTER_RESOURCE rr{ NV_ENC_REGISTER_RESOURCE_VER };
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        rr.resourceToRegister = tex.Get();
        rr.width = W; rr.height = H; rr.pitch = 0;
        rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        NVOK(g_nv.nvEncRegisterResource(enc, &rr));

        NV_ENC_MAP_INPUT_RESOURCE mr{ NV_ENC_MAP_INPUT_RESOURCE_VER };
        mr.registeredResource = rr.registeredResource;
        NVOK(g_nv.nvEncMapInputResource(enc, &mr));

        NV_ENC_PIC_PARAMS pp{ NV_ENC_PIC_PARAMS_VER };
        pp.inputBuffer = mr.mappedResource;
        pp.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
        pp.inputWidth = W; pp.inputHeight = H;
        pp.outputBitstream = outBuf;
        pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        NVENCSTATUS es = g_nv.nvEncEncodePicture(enc, &pp);
        if (es == NV_ENC_SUCCESS) { drain(); encoded++; }
        else if (es != NV_ENC_ERR_NEED_MORE_INPUT) { printf("FEHLER EncodePicture %d\n", (int)es); return 2; }

        g_nv.nvEncUnmapInputResource(enc, mr.mappedResource);
        g_nv.nvEncUnregisterResource(enc, rr.registeredResource);
    }

    printf("Schleife fertig: %d Frames encodiert. EOS-Flush...\n", encoded);
    // Flush (EOS): erst danach den evtl. noch gepufferten Rest abholen - NUR wenn
    // EncodePicture Erfolg meldet, sonst blockiert LockBitstream auf leerem Puffer.
    NV_ENC_PIC_PARAMS eos{ NV_ENC_PIC_PARAMS_VER };
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    NVENCSTATUS fs = g_nv.nvEncEncodePicture(enc, &eos);
    // Bei P4/ULL (kein B-Frame/Lookahead) ist jeder Frame sofort raus - nach EOS gibt
    // es KEINEN gepufferten Rest, ein weiteres LockBitstream wuerde nur blockieren.
    printf("EOS-Status: %d (Flush fertig, kein Rest bei ULL).\n", (int)fs);

    fclose(f);
    g_nv.nvEncDestroyBitstreamBuffer(enc, outBuf);
    g_nv.nvEncDestroyEncoder(enc);
    printf("OK: %d Frames encodiert, poc.h264 = %zu Bytes.\n", encoded, totalBytes);
    return (totalBytes > 0) ? 0 : 3;
}
