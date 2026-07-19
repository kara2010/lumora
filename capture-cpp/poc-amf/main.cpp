// AMD-PoC: WGC-Monitor -> BGRA -> ID3D11VideoProcessor NV12 (GPU) -> AMF-Encoder
// (AMFVideoEncoderVCE_AVC) mit D3D11-Surface -> H.264. Beweist den AMD-Zero-Copy-Weg
// analog zu den NVENC-PoCs. Braucht aktive AMD-GPU (amfrt64.dll). Kompiliert auch ohne
// (DLL wird erst zur Laufzeit geladen) -> startklar, sobald die iGPU aktiviert ist.
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
#include "public/include/core/Factory.h"
#include "public/include/core/Context.h"
#include "public/include/core/Surface.h"
#include "public/include/core/Buffer.h"
#include "public/include/components/Component.h"
#include "public/include/components/VideoEncoderVCE.h"

namespace wg {
    using namespace winrt::Windows::Graphics;
    using namespace winrt::Windows::Graphics::Capture;
    using namespace winrt::Windows::Graphics::DirectX;
    using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
}
#define HROK(call, msg) do { HRESULT _h = (call); if (FAILED(_h)) { printf("FEHLER %s 0x%08lX\n", msg, _h); return 1; } } while(0)
#define AMFOK(call, msg) do { AMF_RESULT _r = (call); if (_r != AMF_OK) { printf("FEHLER %s -> AMF_RESULT %d\n", msg, (int)_r); return 2; } } while(0)

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (!wg::GraphicsCaptureSession::IsSupported()) { printf("FEHLER: WGC nicht unterstuetzt\n"); return 1; }

    // --- D3D11 + WGC-Monitor (wie PoC-D) ---
    winrt::com_ptr<ID3D11Device> dev; winrt::com_ptr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    HROK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, dev.put(), &fl, ctx.put()), "D3D11CreateDevice");
    auto dxgiDev = dev.as<IDXGIDevice>();
    wg::IDirect3DDevice d3dDevice{ nullptr };
    HROK(CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(),
        reinterpret_cast<::IInspectable**>(winrt::put_abi(d3dDevice))), "CreateDirect3D11Device");
    HMONITOR hmon = MonitorFromPoint({ 0,0 }, MONITOR_DEFAULTTOPRIMARY);
    auto interop = winrt::get_activation_factory<wg::GraphicsCaptureItem>().as<IGraphicsCaptureItemInterop>();
    wg::GraphicsCaptureItem item{ nullptr };
    HROK(interop->CreateForMonitor(hmon, winrt::guid_of<wg::GraphicsCaptureItem>(), winrt::put_abi(item)), "CreateForMonitor");
    auto sz = item.Size(); int W = sz.Width & ~1, H = sz.Height & ~1;
    printf("Monitor: %dx%d\n", W, H);
    auto framePool = wg::Direct3D11CaptureFramePool::CreateFreeThreaded(
        d3dDevice, wg::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, sz);
    auto session = framePool.CreateCaptureSession(item);
    session.StartCapture();

    // --- VideoProcessor BGRA->NV12 ---
    auto vdev = dev.as<ID3D11VideoDevice>(); auto vctx = ctx.as<ID3D11VideoContext>();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{}; cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = W; cd.InputHeight = H; cd.OutputWidth = W; cd.OutputHeight = H;
    cd.InputFrameRate = { 60,1 }; cd.OutputFrameRate = { 60,1 }; cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> venum;
    HROK(vdev->CreateVideoProcessorEnumerator(&cd, venum.put()), "CreateVideoProcessorEnumerator");
    winrt::com_ptr<ID3D11VideoProcessor> vproc;
    HROK(vdev->CreateVideoProcessor(venum.get(), 0, vproc.put()), "CreateVideoProcessor");
    D3D11_TEXTURE2D_DESC nd{}; nd.Width = W; nd.Height = H; nd.MipLevels = 1; nd.ArraySize = 1;
    nd.Format = DXGI_FORMAT_NV12; nd.SampleDesc.Count = 1; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> nv12;
    HROK(dev->CreateTexture2D(&nd, nullptr, nv12.put()), "CreateTexture2D(NV12)");
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> outView;
    HROK(vdev->CreateVideoProcessorOutputView(nv12.get(), venum.get(), &ovd, outView.put()), "CreateOutputView");
    printf("VideoProcessor bereit (BGRA->NV12).\n");

    // --- AMF-Encoder ---
    HMODULE amfLib = LoadLibraryW(AMF_DLL_NAME);
    if (!amfLib) { printf("FEHLER: %ls nicht ladbar - AMD-GPU/Treiber aktiv?\n", AMF_DLL_NAME); return 1; }
    auto amfInit = (AMFInit_Fn)GetProcAddress(amfLib, AMF_INIT_FUNCTION_NAME);
    if (!amfInit) { printf("FEHLER: AMFInit fehlt\n"); return 1; }
    amf::AMFFactory* factory = nullptr;
    AMFOK(amfInit(AMF_FULL_VERSION, &factory), "AMFInit");
    amf::AMFContextPtr context;
    AMFOK(factory->CreateContext(&context), "CreateContext");
    AMFOK(context->InitDX11(dev.get()), "InitDX11");
    amf::AMFComponentPtr encoder;
    AMFOK(factory->CreateComponent(context, AMFVideoEncoderVCE_AVC, &encoder), "CreateComponent(AVC)");
    encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY);
    encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, (amf_int64)20000000);
    encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, ::AMFConstructSize(W, H));
    encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, ::AMFConstructRate(60, 1));
    AMFOK(encoder->Init(amf::AMF_SURFACE_NV12, W, H), "encoder->Init");
    printf("AMF-Encoder bereit (H.264 %dx%d).\n", W, H);

    FILE* f = nullptr; fopen_s(&f, "poc_amf.h264", "wb");
    size_t totalBytes = 0; int got = 0, tries = 0;
    auto writeOut = [&](amf::AMFDataPtr& data) {
        amf::AMFBufferPtr buf(data);
        if (buf) { fwrite(buf->GetNative(), 1, buf->GetSize(), f); totalBytes += buf->GetSize(); got++; }
    };
    while (got < 90 && tries < 2000) {
        auto frame = framePool.TryGetNextFrame();
        if (!frame) { std::this_thread::sleep_for(std::chrono::milliseconds(4)); tries++; continue; }
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> frameTex;
        winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), frameTex.put_void()));
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{}; ivd.FourCC = 0; ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; ivd.Texture2D.MipSlice = 0;
        winrt::com_ptr<ID3D11VideoProcessorInputView> inView;
        if (FAILED(vdev->CreateVideoProcessorInputView(frameTex.get(), venum.get(), &ivd, inView.put()))) { tries++; continue; }
        D3D11_VIDEO_PROCESSOR_STREAM st{}; st.Enable = TRUE; st.pInputSurface = inView.get();
        if (FAILED(vctx->VideoProcessorBlt(vproc.get(), outView.get(), 0, 1, &st))) { tries++; continue; }
        ctx->Flush();

        amf::AMFSurfacePtr surface;
        if (context->CreateSurfaceFromDX11Native(nv12.get(), &surface, nullptr) != AMF_OK) { tries++; continue; }
        AMF_RESULT sr = encoder->SubmitInput(surface);
        if (sr != AMF_OK && sr != AMF_NEED_MORE_INPUT) { printf("FEHLER SubmitInput %d\n", (int)sr); return 2; }
        amf::AMFDataPtr data;
        if (encoder->QueryOutput(&data) == AMF_OK && data) writeOut(data);
        tries++;
    }
    // Drain: restliche Frames aus der AMF-Pipeline holen
    encoder->Drain();
    for (int i = 0; i < 30; ++i) {
        amf::AMFDataPtr data;
        AMF_RESULT qr = encoder->QueryOutput(&data);
        if (qr == AMF_OK && data) writeOut(data);
        else if (qr == AMF_EOF) break;
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    fclose(f);
    session.Close(); framePool.Close();
    printf("OK: %d Frames per AMF encodiert, poc_amf.h264 = %zu Bytes.\n", got, totalBytes);
    return (got > 0 && totalBytes > 0) ? 0 : 3;
}
