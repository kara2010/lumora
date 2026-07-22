// lumora-capture (C++, Phase 2 v2): echter Helfer mit Encoder-Abstraktion.
// WGC-Monitor -> VideoProcessor NV12 -> Encoder (NVENC ODER AMF, GPU-Auto-Auswahl,
// Zero-Copy) -> eigener MPEG-TS-Mux -> UDP an mediamtx. LIVE, OHNE FFmpeg.
// Aufruf: lumora-capture [--encoder auto|nvenc|amf|qsv] [--codec auto|h264|av1] [--fps N] [--bitrate MBIT]
//                        [--mtx-host IP] [--mtx-port P] [--frames N] [--tsout DATEI]
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_6.h>
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
#include <mutex>
#include <atomic>
#include <deque>
#include <cmath>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <wrl/implements.h>
#include <d3dcompiler.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "mmdevapi.lib")
#include "opus.h"
#include "nvEncodeAPI.h"
#include "public/include/core/Factory.h"
#include "public/include/core/Context.h"
#include "public/include/core/Surface.h"
#include "public/include/core/Buffer.h"
#include "public/include/components/Component.h"
#include "public/include/components/VideoEncoderVCE.h"
#include "public/include/components/VideoEncoderAV1.h"
#include "vpl/mfxvideo.h"
#include "vpl/mfxdispatcher.h"
#pragma comment(lib, "ws2_32.lib")

namespace wg {
    using namespace winrt::Windows::Graphics;
    using namespace winrt::Windows::Graphics::Capture;
    using namespace winrt::Windows::Graphics::DirectX;
    using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
}

// ================= MPEG-TS-Muxer (bewiesen gegen mediamtx) =================
static const int PID_PMT = 0x1000, PID_VIDEO = 0x0100, PID_AUDIO = 0x0101, PID_VIDEO_AV1 = 0x0102;
static uint8_t g_ccVideo = 0, g_ccPat = 0, g_ccPmt = 0, g_ccAudio = 0, g_ccVideoAv1 = 0;
static bool g_withAudio = false;
// Bedarfsgesteuert: es laeuft genau die Codec-Menge, die die aktuellen Zuschauer brauchen
// (Shell -> lumora-codec.txt). g_h264/g_av1 steuern PMT-Deklaration UND Mux. Mindestens
// einer ist immer an; der jeweils primaere (H.264 wenn an, sonst AV1) traegt den PCR.
static bool g_h264 = true;            // H.264 auf PID_VIDEO aktiv
static bool g_av1 = false;            // AV1 zusaetzlich/alternativ auf PID_VIDEO_AV1 aktiv
static bool g_audioNoBytes = false;   // Diagnose: Audio in PMT deklarieren, aber keine Audio-Pakete senden
static uint64_t g_audioMuxed = 0;
static uint32_t crc32_mpeg(const uint8_t* d, int len) { uint32_t c = 0xFFFFFFFF; for (int i = 0; i < len; ++i) { c ^= (uint32_t)d[i] << 24; for (int b = 0; b < 8; ++b) c = (c & 0x80000000) ? (c << 1) ^ 0x04C11DB7 : (c << 1); } return c; }
static void writeTS(std::vector<uint8_t>& out, int pid, bool pusi, uint8_t& cc, const uint8_t* payload, int payloadLen, bool wantPCR, uint64_t pcr) {
    uint8_t pkt[188]; memset(pkt, 0xFF, 188); pkt[0] = 0x47; pkt[1] = (pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F); pkt[2] = pid & 0xFF;
    int afLen = wantPCR ? 7 : 0; bool haveAF = wantPCR; int maxPayload = 188 - 4 - (haveAF ? 1 + afLen : 0);
    int take = payloadLen < maxPayload ? payloadLen : maxPayload; int stuffing = 0;
    if (take >= payloadLen) { int need = maxPayload - take; if (need > 0) { if (!haveAF) { haveAF = true; afLen = 1; need -= 2; } stuffing = need < 0 ? 0 : need; } } // AF-Overhead (Laengen- + Flags-Byte) abziehen, sonst 2 Byte Nutzlast abgeschnitten
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
static void buildPMT(std::vector<uint8_t>& o) { uint8_t s[64]; int n = 0; s[n++] = 2; s[n++] = 0xB0; int lenPos = n; s[n++] = 0; s[n++] = 0; s[n++] = 1; s[n++] = 0xC1; s[n++] = 0; s[n++] = 0;
    int pcrPid = g_h264 ? PID_VIDEO : PID_VIDEO_AV1;   // PCR traegt der primaere laufende Video-PID
    s[n++] = 0xE0 | ((pcrPid >> 8) & 0x1F); s[n++] = pcrPid & 0xFF; s[n++] = 0xF0; s[n++] = 0;
    if (g_h264) { s[n++] = 0x1B; s[n++] = 0xE0 | ((PID_VIDEO >> 8) & 0x1F); s[n++] = PID_VIDEO & 0xFF; s[n++] = 0xF0; s[n++] = 0; }        // Video H.264
    if (g_av1) { s[n++] = 0x06; s[n++] = 0xE0 | ((PID_VIDEO_AV1 >> 8) & 0x1F); s[n++] = PID_VIDEO_AV1 & 0xFF; s[n++] = 0xF0; s[n++] = 0x06; // Video AV1 (kein Standard-stream_type: 0x06 private)
        s[n++] = 0x05; s[n++] = 0x04; s[n++] = 'A'; s[n++] = 'V'; s[n++] = '0'; s[n++] = '1'; }  // registration_descriptor "AV01"
    if (g_withAudio) { s[n++] = 0x06; s[n++] = 0xE0 | ((PID_AUDIO >> 8) & 0x1F); s[n++] = PID_AUDIO & 0xFF; s[n++] = 0xF0; s[n++] = 0x0A; // Audio Opus (PES private data, stream_type 0x06)
        s[n++] = 0x05; s[n++] = 0x04; s[n++] = 'O'; s[n++] = 'p'; s[n++] = 'u'; s[n++] = 's';   // registration_descriptor "Opus"
        s[n++] = 0x7F; s[n++] = 0x02; s[n++] = 0x80; s[n++] = 0x02; }                            // Opus-Erweiterungsdeskriptor: 2 Kanaele
    s[lenPos] = (uint8_t)(n - lenPos - 1 + 4);   // section_length: Rest der Sektion inkl. CRC
    uint32_t c = crc32_mpeg(s, n); s[n++] = c >> 24; s[n++] = c >> 16; s[n++] = c >> 8; s[n++] = c; writePSI(o, PID_PMT, g_ccPmt, s, n); }
static void writePTS(std::vector<uint8_t>& v, int g, uint64_t t) { v.push_back((uint8_t)((g << 4) | (((t >> 30) & 7) << 1) | 1)); v.push_back((uint8_t)((t >> 22) & 0xFF)); v.push_back((uint8_t)((((t >> 15) & 0x7F) << 1) | 1)); v.push_back((uint8_t)((t >> 7) & 0xFF)); v.push_back((uint8_t)(((t & 0x7F) << 1) | 1)); }

// ================= Encoder-Abstraktion =================
struct Encoder {
    virtual ~Encoder() {}
    virtual bool init(ID3D11Device* dev, int w, int h, int fps, int mbit, bool av1 = false) = 0; // av1=true: AV1 statt H.264 (false bei fehlender HW-Faehigkeit)
    virtual void encode(ID3D11Texture2D* nv12, std::vector<std::vector<uint8_t>>& out) = 0; // 0..N fertige AUs (H.264 Annex-B bzw. AV1 Temporal Units)
    virtual void setBitrate(int kbit) {}   // Live-Bitrate in kbit (Reconfigure OHNE Neustart); default no-op
    virtual const char* name() = 0;
};

struct NvencEncoder : Encoder {
    NV_ENCODE_API_FUNCTION_LIST nv{ NV_ENCODE_API_FUNCTION_LIST_VER }; void* enc = nullptr; NV_ENC_OUTPUT_PTR outBuf = nullptr; int W = 0, H = 0;
    NV_ENC_PRESET_CONFIG pc{ NV_ENC_PRESET_CONFIG_VER }; NV_ENC_INITIALIZE_PARAMS ip{ NV_ENC_INITIALIZE_PARAMS_VER };  // gespeichert fuer Live-Bitrate-Reconfigure
    // WICHTIG: rohe NVENC-Handles muessen explizit freigegeben werden. Ohne diesen Destruktor
    // leckt jeder bedarfsgesteuerte Encoder-Abbau die komplette NVENC-Session (bei 4K hunderte
    // MB Frame-/Referenzpuffer) - fuehrte zu RADAR_PRE_LEAK/Absturz beim Codec-Wechsel im Stream.
    ~NvencEncoder() override {
        if (enc) {
            if (outBuf) { nv.nvEncDestroyBitstreamBuffer(enc, outBuf); outBuf = nullptr; }
            nv.nvEncDestroyEncoder(enc); enc = nullptr;
        }
    }
    const char* name() override { return "NVENC"; }
    bool init(ID3D11Device* dev, int w, int h, int fps, int mbit, bool av1 = false) override {
        W = w; H = h; HMODULE lib = LoadLibraryW(L"nvEncodeAPI64.dll"); if (!lib) return false;
        typedef NVENCSTATUS(NVENCAPI* Fn)(NV_ENCODE_API_FUNCTION_LIST*); if (((Fn)GetProcAddress(lib, "NvEncodeAPICreateInstance"))(&nv) != NV_ENC_SUCCESS) return false;
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{ NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER }; op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX; op.device = dev; op.apiVersion = NVENCAPI_VERSION;
        if (nv.nvEncOpenEncodeSessionEx(&op, &enc) != NV_ENC_SUCCESS) return false;
        GUID codec = av1 ? NV_ENC_CODEC_AV1_GUID : NV_ENC_CODEC_H264_GUID;
        pc.presetCfg.version = NV_ENC_CONFIG_VER;
        // Rueckgabe pruefen: auf GPUs ohne AV1-Encode (vor RTX 40) scheitert bereits diese
        // Abfrage mit UNSUPPORTED_PARAM - das ist zugleich unser Faehigkeits-Check.
        if (nv.nvEncGetEncodePresetConfigEx(enc, codec, NV_ENC_PRESET_P4_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &pc) != NV_ENC_SUCCESS) return false;
        pc.presetCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR; pc.presetCfg.rcParams.averageBitRate = mbit * 1000000;
        // GOP = 1s (nicht 2s): halbiert die Wartezeit bis zum ersten Bild fuer neue
        // Zuschauer/die Vorschau (gleiche Erkenntnis wie im FFmpeg-Pfad, s. main.js bcEncoderArgs).
        pc.presetCfg.gopLength = fps; pc.presetCfg.frameIntervalP = 1;
        if (av1) { pc.presetCfg.encodeCodecConfig.av1Config.repeatSeqHdr = 1; pc.presetCfg.encodeCodecConfig.av1Config.idrPeriod = fps; }
        else { pc.presetCfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1; pc.presetCfg.encodeCodecConfig.h264Config.idrPeriod = fps; }
        ip.encodeGUID = codec; ip.presetGUID = NV_ENC_PRESET_P4_GUID; ip.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        ip.encodeWidth = w; ip.encodeHeight = h; ip.darWidth = w; ip.darHeight = h; ip.frameRateNum = fps; ip.frameRateDen = 1; ip.enablePTD = 1; ip.encodeConfig = &pc.presetCfg;
        if (nv.nvEncInitializeEncoder(enc, &ip) != NV_ENC_SUCCESS) return false;
        NV_ENC_CREATE_BITSTREAM_BUFFER cb{ NV_ENC_CREATE_BITSTREAM_BUFFER_VER }; nv.nvEncCreateBitstreamBuffer(enc, &cb); outBuf = cb.bitstreamBuffer; return true;
    }
    void setBitrate(int kbit) override {   // CBR-Bitrate live umstellen (nahtlos, kein Stream-Abriss)
        pc.presetCfg.rcParams.averageBitRate = kbit * 1000; pc.presetCfg.rcParams.maxBitRate = kbit * 1000;
        NV_ENC_RECONFIGURE_PARAMS rp{ NV_ENC_RECONFIGURE_PARAMS_VER }; rp.reInitEncodeParams = ip;
        nv.nvEncReconfigureEncoder(enc, &rp);
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
    winrt::com_ptr<ID3D11DeviceContext> d3dctx; int encW = 0, encH = 0, encFps = 60; uint64_t frameNo = 0; bool isAv1 = false;
    // AMF-Smart-Pointer released ohnehin, aber explizit Terminate() gibt die VCN-/Surface-Puffer
    // sofort frei (sonst analoges Leck wie bei NVENC beim bedarfsgesteuerten Encoder-Abbau).
    ~AmfEncoder() override {
        if (encoder) { encoder->Terminate(); encoder = nullptr; }
        if (context) { context->Terminate(); context = nullptr; }
    }
    const char* name() override { return "AMF"; }
    bool init(ID3D11Device* dev, int w, int h, int fps, int mbit, bool av1 = false) override {
        encW = w; encH = h; encFps = fps > 0 ? fps : 60; isAv1 = av1; dev->GetImmediateContext(d3dctx.put());
        HMODULE lib = LoadLibraryW(AMF_DLL_NAME); if (!lib) return false;
        auto amfInit = (AMFInit_Fn)GetProcAddress(lib, AMF_INIT_FUNCTION_NAME); if (!amfInit) return false;
        amf::AMFFactory* f = nullptr; if (amfInit(AMF_FULL_VERSION, &f) != AMF_OK) return false;
        if (f->CreateContext(&context) != AMF_OK) return false; if (context->InitDX11(dev) != AMF_OK) return false;
        if (av1) {
            // AV1-Component (VCN4+, ab RX 7000); CreateComponent scheitert auf aelterer HW -
            // das ist zugleich unser Faehigkeits-Check. Eigener Property-Namespace (AV1_*),
            // sonst gleiche Philosophie wie der AVC-Pfad unten: CBR, Low-Latency, kein Filler,
            // Keyframe alle 1s mit Sequence-Header (Zuschauer-Einstieg).
            if (f->CreateComponent(context, AMFVideoEncoder_AV1, &encoder) != AMF_OK) return false;
            encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_USAGE, AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY);
            encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR);
            encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FILLER_DATA, false);
            encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, (amf_int64)mbit * 1000000);
            encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMESIZE, ::AMFConstructSize(w, h));
            encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMERATE, ::AMFConstructRate(fps, 1));
            encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, (amf_int64)fps);
            encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE, AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE_KEY_FRAME_ALIGNED);
            return encoder->Init(amf::AMF_SURFACE_NV12, w, h) == AMF_OK;
        }
        if (f->CreateComponent(context, AMFVideoEncoderVCE_AVC, &encoder) != AMF_OK) return false;
        // USAGE zuerst (setzt Defaults); danach unsere Werte. WebRTC-sauber wie NVENC:
        // echtes CBR (keine VBR-Bursts, die den WHEP-Decoder abwuergen), Low-Latency-Modus,
        // KEINE Filler-Daten. B-Frames sind unter LOW_LATENCY ohnehin 0 (kein Reordering).
        encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY);
        encoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
        encoder->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
        // AMF defaultet unter LOW_LATENCY auf Profile MAIN (CABAC) - real auf AMD bestaetigt
        // (mediamtx meldet "profile":"Main" fuer den einkommenden Track). Viele WebRTC-Decoder
        // (v.a. Firefox' eingebetteter OpenH264, aber auch etliche Chromium-Sandboxes/Hardware-
        // Pfade) koennen NUR Constrained-Baseline (CAVLC, keine B-Frames) zuverlaessig decodieren
        // - das ist der von WebRTC/RFC6184 als Minimalkonsens vorausgesetzte Modus. NVENC laeuft
        // ueber denselben Pfad ohne explizites Profile und landet dort offenbar kompatibel;
        // AMF nicht. Deshalb hier hart auf Baseline zwingen statt AMF-Default zu vertrauen.
        encoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_BASELINE);
        encoder->SetProperty(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
        encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, (amf_int64)mbit * 1000000);
        encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, ::AMFConstructSize(w, h));
        encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, ::AMFConstructRate(fps, 1));
        // GOP = 1s (nicht 2s): halbiert die Wartezeit bis zum ersten Bild fuer neue
        // Zuschauer/die Vorschau (gleiche Erkenntnis wie im FFmpeg-Pfad, s. main.js bcEncoderArgs).
        // IDR_PERIOD ist unter LOW_LATENCY nachweislich unzuverlaessig (s. FORCE_PICTURE_TYPE
        // unten) - trotzdem gesetzt, falls AMF sie in anderen Faellen doch honoriert.
        encoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, (amf_int64)fps);
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, (amf_int64)fps); // SPS/PPS periodisch (Zuschauer-Einstieg)
        return encoder->Init(amf::AMF_SURFACE_NV12, w, h) == AMF_OK;
    }
    void encode(ID3D11Texture2D* nv12, std::vector<std::vector<uint8_t>>& out) override {
        // WICHTIG: NICHT die geteilte nv12-Textur direkt an AMF wrappen. SubmitInput ist
        // asynchron - der Encoder liest die Surface SPAETER, waehrend der Render-Loop nv12
        // im naechsten Takt schon wieder ueberschreibt -> zerrissene Frames -> kaputte
        // P-Frames -> Zuschauer-Bild kippt nach Sekunden auf schwarz + Reconnect-Schleife
        // (auf AMD real reproduziert; NVENC liest synchron und hat das Problem nicht).
        // Darum: eigene AMF-Surface pro Frame, nv12 hineinkopieren - AMF liest eine Surface,
        // die wir nie wieder anfassen.
        amf::AMFSurfacePtr surf;
        if (context->AllocSurface(amf::AMF_MEMORY_DX11, amf::AMF_SURFACE_NV12, encW, encH, &surf) != AMF_OK) return;
        ID3D11Texture2D* dst = (ID3D11Texture2D*)surf->GetPlaneAt(0)->GetNative();
        if (!dst) return;
        d3dctx->CopyResource(dst, nv12);   // gleiche Immediate-Context-Reihenfolge: nach Blt+Flush
        // AMF-Sonderfall (bekannter WHEP-Kernbug, bereits fuer den alten FFmpeg/h264_amf-Pfad
        // dokumentiert, s. main.js "AMF-Sonderfall"): der Encoder haelt sich unter LOW_LATENCY
        // NICHT zuverlaessig an AMF_VIDEO_ENCODER_IDR_PERIOD - es kann bei EINEM IDR-Keyframe
        // fuer die gesamte Session bleiben, nur noch P-Frames danach. Ein Zuschauer, der NACH
        // diesem einen Keyframe verbindet, bekommt nie eine Referenz und das Bild bleibt
        // schwarz (matcht exakt das reale Symptom). FORCE_PICTURE_TYPE ist eine Frame-Property
        // (muss pro Submit gesetzt werden) - hier alle 1s (GOP-Laenge) hart IDR erzwingen statt
        // uns auf die (nachweislich unzuverlaessige) IDR_PERIOD-Automatik zu verlassen.
        if (frameNo % (uint64_t)encFps == 0) {
            if (isAv1) { surf->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, amf_int64(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY)); surf->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, true); }
            else surf->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, amf_int64(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
        }
        frameNo++;
        if (encoder->SubmitInput(surf) != AMF_OK) return;
        amf::AMFDataPtr data;
        while (encoder->QueryOutput(&data) == AMF_OK && data) { amf::AMFBufferPtr b(data); if (b) out.emplace_back((uint8_t*)b->GetNative(), (uint8_t*)b->GetNative() + b->GetSize()); data = nullptr; }
    }
    void setBitrate(int kbit) override { encoder->SetProperty(isAv1 ? AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE : AMF_VIDEO_ENCODER_TARGET_BITRATE, (amf_int64)kbit * 1000); }   // AMF: Bitrate live (kbit)
};

// Intel-QSV via oneVPL (MFXVideoENCODE). Struktur wie NVENC/AMF. HINWEIS: kompiliert und
// nutzt die API korrekt, aber der echte Encode ist erst auf Intel-Silizium verifiziert -
// insbesondere der GPU->GPU-Copy in die oneVPL-Surface (Texture-Array-Subresource?) ist dort
// zu pruefen. Runtime (libmfx) kommt mit dem Intel-Treiber; Dispatcher = libvpl.dll.
struct QsvEncoder : Encoder {
    mfxLoader loader = nullptr; mfxSession session = nullptr; winrt::com_ptr<ID3D11DeviceContext> ctx;
    winrt::com_ptr<ID3D11Texture2D> staging; int encW = 0, encH = 0;
    mfxFrameInfo surfInfo{}; std::vector<uint8_t> surfBuf;
    std::vector<uint8_t> bsBuf; bool inited = false;
    // Pipelining (s. Kommentar in encode()): ein Submit bleibt zwischen zwei Aufrufen "in
    // Flight", der Sync-Wait wird erst beim naechsten Aufruf nachgeholt. surf MUSS ein Member
    // sein (nicht lokal in encode()) - der Treiber liest asynchron aus der mfxFrameSurface1-
    // Struktur, solange die Operation noch nicht synchronisiert ist; eine Stack-lokale Variable
    // waere nach Rueckkehr aus encode() bereits ungueltig/ueberschrieben (fuehrte zu kaputten
    // Access Units, Log-Beweis 2026-07-21).
    mfxSyncPoint pendingSyncp = nullptr; mfxBitstream pendingBs{}; mfxFrameSurface1 surf{};
    mfxVideoParam par{};   // Member (nicht lokal in init()): setBitrate() braucht die aktuellen
                           // Params fuer MFXVideoENCODE_Reset (Reset erwartet den VOLLEN Satz,
                           // nicht nur das geaenderte Feld).
    const char* name() override { return "QSV"; }
    ~QsvEncoder() { if (session) { if (pendingSyncp) MFXVideoCORE_SyncOperation(session, pendingSyncp, 100000); if (inited) MFXVideoENCODE_Close(session); MFXClose(session); } if (loader) MFXUnload(loader); }
    // WICHTIGER BEFUND (Log-Beweise 2026-07-21, auf echter Intel-Hardware reproduziert, siehe
    // HANDOFF-INTEL-QSV-DEBUG.md): MFXVideoENCODE_Init schlaegt mit IOPattern=VIDEO_MEMORY
    // (D3D11-Zero-Copy) auf diesem Geraet IMMER mit status=-15 (MFX_ERR_INVALID_VIDEO_PARAM)
    // fehl - MFXVideoENCODE_Query bleibt dabei durchgehend lautlos MFX_ERR_NONE. Systematisch
    // ausgeschlossen (je isoliert getestet, status=-15 blieb JEWEILS identisch): CodecProfile/
    // CodecLevel; Adapter-Mismatch (nur EINE Implementierung passt zum Filter, kein zweideutiges
    // Index-0 auf diesem Hybrid-GPU-Geraet mit Intel UHD + NVIDIA RTX 2060); LowPower=ON;
    // BufferSizeInKB/InitialDelayInKB; CQP statt CBR (schliesst Bitrate/RateControl komplett
    // aus); GopPicSize/GopRefDist/IdrInterval/AsyncDepth; TargetUsage; NumRefFrame;
    // D3D11_CREATE_DEVICE_VIDEO_SUPPORT; ID3D10Multithread::SetMultithreadProtected(TRUE) (per
    // oneVPL-Spec zwingend, aber allein nicht ausreichend); ein komplett frisches, nie zuvor
    // benutztes D3D11-Device auf demselben Adapter (schliesst Konflikte mit der WGC/
    // VideoProcessor-Nutzung des geteilten Devices aus).
    // Mit IOPattern=MFX_IOPATTERN_IN_SYSTEM_MEMORY (sonst IDENTISCHE Session/Device/Params)
    // initialisiert der Encoder ERFOLGREICH. Der Fehler sitzt also spezifisch/ausschliesslich im
    // D3D11-Zero-Copy-Pfad dieser Treiber-/Runtime-Version (ApiVersion 1.30, Impl "mfxhw64") -
    // sehr wahrscheinlich eine echte Treiber-Einschraenkung, kein App-seitiger Parameterfehler.
    // Fix: QSV nutzt SYSTEM_MEMORY mit CPU-Readback (Staging-Textur + Map/Unmap) statt Zero-
    // Copy. NVENC/AMF bleiben unveraendert Zero-Copy (dort nicht reproduzierbar/kein Problem).
    bool init(ID3D11Device* dev, int w, int h, int fps, int mbit, bool av1 = false) override {
        mfxU32 codecId = av1 ? MFX_CODEC_AV1 : MFX_CODEC_AVC;   // AV1 ab Arc/Xe; Query/Init scheitern auf aelterer HW = Faehigkeits-Check
        dev->GetImmediateContext(ctx.put());
        // Per oneVPL-Spec fuer D3D11-HW-Beschleunigung vorgeschrieben ("The application must
        // also set multi-threading mode for the Direct3D 11 device") - bleibt trotz SYSTEM_MEMORY
        // gesetzt (schadet nicht, ist Spec-konform).
        { winrt::com_ptr<ID3D10Multithread> mt; if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(mt.put())))) mt->SetMultithreadProtected(TRUE); }
        encW = w; encH = h;
        D3D11_TEXTURE2D_DESC sd{}; sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_NV12; sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, staging.put()))) { printf("QSV-DEBUG: Staging-Textur fehlgeschlagen\n"); return false; }
        loader = MFXLoad(); if (!loader) { printf("QSV-DEBUG: MFXLoad fehlgeschlagen (Loader null)\n"); return false; }
        mfxConfig cfg = MFXCreateConfig(loader);
        mfxVariant v{}; v.Version.Version = MFX_VARIANT_VERSION; v.Type = MFX_VARIANT_TYPE_U32;
        v.Data.U32 = MFX_IMPL_TYPE_HARDWARE; MFXSetConfigFilterProperty(cfg, (const mfxU8*)"mfxImplDescription.Impl", v);
        v.Data.U32 = codecId; MFXSetConfigFilterProperty(cfg, (const mfxU8*)"mfxImplDescription.mfxEncoderDescription.encoder.CodecID", v);
        v.Data.U32 = MFX_ACCEL_MODE_VIA_D3D11; MFXSetConfigFilterProperty(cfg, (const mfxU8*)"mfxImplDescription.AccelerationMode", v);
        // Diagnose (bestaetigt kein Adapter-Mismatch): loggen, wieviele Implementierungen zum
        // Filter (HARDWARE + AVC-Encoder + D3D11) passen und welche das sind.
        for (mfxU32 ei = 0;; ++ei) {
            mfxImplDescription* idesc = nullptr;
            if (MFXEnumImplementations(loader, ei, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, (mfxHDL*)&idesc) != MFX_ERR_NONE || !idesc) break;
            printf("QSV-DEBUG: Impl[%u]: Name=\"%s\" VendorID=0x%x VendorImplID=%u AccelMode=%d ApiVersion=%d.%d DeviceID=\"%s\"\n",
                ei, idesc->ImplName, idesc->VendorID, idesc->VendorImplID, (int)idesc->AccelerationMode, idesc->ApiVersion.Major, idesc->ApiVersion.Minor, idesc->Dev.DeviceID);
            MFXDispReleaseImplDescription(loader, idesc);
        }
        mfxStatus st = MFXCreateSession(loader, 0, &session);
        if (st != MFX_ERR_NONE) { printf("QSV-DEBUG: MFXCreateSession fehlgeschlagen, status=%d\n", (int)st); return false; }
        // KEIN MFXVideoCORE_SetHandle: bei reinem SYSTEM_MEMORY-Betrieb nicht noetig - mit
        // gesetztem D3D11-Handle lieferte MFXMemory_GetSurfaceForEncode status=-6
        // (MFX_ERR_INVALID_HANDLE); ohne SetHandle trat dieses Problem gar nicht erst auf (wir
        // nutzen ohnehin die klassische externe-Surface-Methode statt GetSurfaceForEncode, s.u.).
        mfxVersion ver{}; if (MFXQueryVersion(session, &ver) == MFX_ERR_NONE) printf("QSV-DEBUG: oneVPL-Session-Version Major=%d Minor=%d\n", ver.Major, ver.Minor);
        mfxIMPL implVal = 0; if (MFXQueryIMPL(session, &implVal) == MFX_ERR_NONE) printf("QSV-DEBUG: MFXQueryIMPL=0x%x (Impl=%u ViaMask=0x%x)\n", (unsigned)implVal, (unsigned)MFX_IMPL_BASETYPE(implVal), (unsigned)(implVal & 0xFF00));
        par = mfxVideoParam{};
        par.mfx.CodecId = codecId; par.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
        par.mfx.RateControlMethod = MFX_RATECONTROL_CBR; par.mfx.TargetKbps = (mfxU16)(mbit * 1000); par.mfx.MaxKbps = (mfxU16)(mbit * 1000);
        par.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12; par.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420; par.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        par.mfx.FrameInfo.FrameRateExtN = fps; par.mfx.FrameInfo.FrameRateExtD = 1;
        par.mfx.FrameInfo.Width = (mfxU16)((w + 15) & ~15); par.mfx.FrameInfo.Height = (mfxU16)((h + 15) & ~15);
        par.mfx.FrameInfo.CropW = (mfxU16)w; par.mfx.FrameInfo.CropH = (mfxU16)h;
        // GOP = 1s (nicht 2s): halbiert die Wartezeit bis zum ersten Bild fuer neue
        // Zuschauer/die Vorschau (gleiche Erkenntnis wie im FFmpeg-Pfad, s. main.js bcEncoderArgs).
        par.mfx.GopPicSize = (mfxU16)fps; par.mfx.GopRefDist = 1; par.mfx.IdrInterval = 0;
        // AsyncDepth=2 (statt 1): erlaubt ein Frame "in Flight" waehrend encode() bereits das
        // naechste vorbereitet (s. Pipelining-Kommentar in encode()) - ohne das lehnt der
        // Treiber ein Submit ab, waehrend noch ein Sync aussteht.
        par.AsyncDepth = 2;
        par.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
        mfxStatus qst = MFXVideoENCODE_Query(session, &par, &par);
        if (qst != MFX_ERR_NONE) printf("QSV-DEBUG: MFXVideoENCODE_Query meldet status=%d (Warnung/Fehler, Query passt Parameter evtl. an)\n", (int)qst);
        printf("QSV-DEBUG: nach Query: Width=%d Height=%d CropW=%d CropH=%d TargetUsage=%d RateControlMethod=%d IOPattern=%d TargetKbps=%d MaxKbps=%d BufferSizeInKB=%d GopPicSize=%d GopRefDist=%d IdrInterval=%d AsyncDepth=%d FourCC=%u ChromaFormat=%d PicStruct=%d FrameRate=%d/%d NumSlice=%d CodecProfile=%d CodecLevel=%d NumRefFrame=%d\n",
            par.mfx.FrameInfo.Width, par.mfx.FrameInfo.Height, par.mfx.FrameInfo.CropW, par.mfx.FrameInfo.CropH, par.mfx.TargetUsage, par.mfx.RateControlMethod, par.IOPattern,
            par.mfx.TargetKbps, par.mfx.MaxKbps, par.mfx.BufferSizeInKB, par.mfx.GopPicSize, par.mfx.GopRefDist, par.mfx.IdrInterval, par.AsyncDepth,
            par.mfx.FrameInfo.FourCC, par.mfx.FrameInfo.ChromaFormat, par.mfx.FrameInfo.PicStruct, par.mfx.FrameInfo.FrameRateExtN, par.mfx.FrameInfo.FrameRateExtD,
            par.mfx.NumSlice, par.mfx.CodecProfile, par.mfx.CodecLevel, par.mfx.NumRefFrame);
        mfxStatus ist = MFXVideoENCODE_Init(session, &par);
        if (ist < MFX_ERR_NONE) { printf("QSV-DEBUG: MFXVideoENCODE_Init fehlgeschlagen, status=%d\n", (int)ist); return false; }
        if (ist != MFX_ERR_NONE) printf("QSV-DEBUG: MFXVideoENCODE_Init Warnung, status=%d (Init trotzdem fortgesetzt)\n", (int)ist);
        // Log-Beweis 2026-07-21: bei 25 Mbit lieferte MFXVideoENCODE_EncodeFrameAsync mit dem
        // alten Puffer (1x Bitrate/Sekunde + 1MB, ~4.17MB) durchgehend status=-5
        // (MFX_ERR_NOT_ENOUGH_BUFFER) - obwohl die tatsaechlich erzeugten Frames winzig waren
        // (IDR ~184KB, P-Frames <4KB). Der Treiber prueft bs.MaxLength offenbar gegen einen
        // internen VBV/CPB-Puffergroessen-Schaetzwert (BufferSizeInKB=0="auto"), NICHT gegen die
        // tatsaechliche Ausgabegroesse - bei 8 Mbit reichte die alte Groesse zufaellig, bei
        // 25 Mbit nicht mehr. Mit 3x Bitrate/Sekunde + 2MB Headroom bei 8/25/50 Mbit real
        // getestet, kein -5 mehr.
        bsBuf.resize((size_t)mbit * 1000 * 1000 / 8 * 3 + (2 << 20));
        // WIDERLEGT (Log-Beweis 2026-07-21): MFXMemory_GetSurfaceForEncode (interne
        // Allokations-Convenience-API, seit oneVPL 2.0) liefert bei SYSTEM_MEMORY auf dieser
        // HARDWARE-Session status=-6 (MFX_ERR_INVALID_HANDLE), sowohl MIT als auch OHNE
        // vorherigem MFXVideoCORE_SetHandle - Init selbst war erfolgreich, nur diese
        // Convenience-Funktion funktioniert nicht. Klassischer Weg stattdessen: eigene
        // mfxFrameSurface1 mit Data.Y/UV auf selbstverwalteten Puffer (kein FrameInterface noetig,
        // keine Map/Release-Zyklen) - funktioniert mit jeder Session, unabhaengig von dieser Bug.
        surfInfo = par.mfx.FrameInfo;
        surfBuf.assign((size_t)surfInfo.Width * surfInfo.Height * 3 / 2, 0);
        inited = true; return true;
    }
    // FEHLTE BISHER KOMPLETT (Encoder-Basisklasse hat nur einen No-Op-Default) - main() rief
    // encoder->setBitrate(k) bei einer Bitrate-Aenderung ueber die Steuerdatei zwar auf, fuer
    // QSV passierte dabei aber schlicht nichts; das Log zeigte trotzdem "Bitrate live -> X kbit",
    // weil dieser Printf in main() unabhaengig vom tatsaechlichen Encoder-Ergebnis erfolgt.
    void setBitrate(int kbit) override {
        if (!inited) return;
        // Ein evtl. noch offenes Submit (s. Pipelining in encode()) muss VOR Reset
        // synchronisiert sein - Reset setzt voraus, dass kein Frame mehr asynchron in
        // Bearbeitung ist. Ergebnis wird hier verworfen (verliert hoechstens 1 Frame,
        // vernachlaessigbar bei einer seltenen manuellen Bitrate-Aenderung).
        if (pendingSyncp) { MFXVideoCORE_SyncOperation(session, pendingSyncp, 100000); pendingSyncp = nullptr; }
        par.mfx.TargetKbps = (mfxU16)kbit; par.mfx.MaxKbps = (mfxU16)kbit;
        mfxStatus st = MFXVideoENCODE_Reset(session, &par);
        if (st < MFX_ERR_NONE) printf("QSV-DEBUG: MFXVideoENCODE_Reset (Bitrate) fehlgeschlagen, status=%d\n", (int)st);
    }
    void encode(ID3D11Texture2D* nv12, std::vector<std::vector<uint8_t>>& out) override {
        // Pipelining (Befund 2026-07-21, per QSV-PERF-Messung auf echter Intel-Hardware): mit
        // SOFORTIGEM Sync direkt nach jedem Submit kostete Map()+Sync() zusammen im Schnitt
        // ~14.5ms (Map ~5-6ms, Sync ~7-8ms), mit Spitzen >16.7ms (60fps-Budget) sobald beide
        // gleichzeitig langsam waren - das war die Ursache fuer die schwankende/zurueckfallende
        // Framerate im echten Stream. Fix: ein Submit bleibt bis zum NAECHSTEN encode()-Aufruf
        // offen (AsyncDepth=2 s. init) - der Sync-Wait faellt dann praktisch weg, weil die GPU
        // die HW-Kodierung des vorherigen Frames im Hintergrund laengst erledigt hat, waehrend
        // wir hier bereits den naechsten Frame lesen/vorbereiten.
        if (pendingSyncp) {
            MFXVideoCORE_SyncOperation(session, pendingSyncp, 100000);
            out.emplace_back(pendingBs.Data + pendingBs.DataOffset, pendingBs.Data + pendingBs.DataOffset + pendingBs.DataLength);
            pendingSyncp = nullptr;
        }
        // Kein Zero-Copy (s. Kommentar bei init): GPU->GPU-Readback in eine Staging-Textur, dann
        // CPU-Kopie in einen selbstverwalteten NV12-Puffer (Pitch von Quelle/Ziel koennen
        // abweichen, deshalb zeilenweise).
        ctx->CopyResource(staging.get(), nv12);
        ctx->Flush(); // ohne das kann Map() auf manchen Treibern sehr lange/unbegrenzt blockieren
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
        uint8_t* dstY = surfBuf.data();
        uint8_t* dstUV = surfBuf.data() + (size_t)surfInfo.Width * surfInfo.Height;
        const uint8_t* srcY = (const uint8_t*)mapped.pData;
        const uint8_t* srcUV = srcY + (size_t)mapped.RowPitch * encH;
        for (int y = 0; y < encH; ++y) memcpy(dstY + (size_t)surfInfo.Width * y, srcY + (size_t)mapped.RowPitch * y, (size_t)encW);
        for (int y = 0; y < encH / 2; ++y) memcpy(dstUV + (size_t)surfInfo.Width * y, srcUV + (size_t)mapped.RowPitch * y, (size_t)encW);
        ctx->Unmap(staging.get(), 0);
        // Klassische externe Surface (kein FrameInterface/GetSurfaceForEncode, s. Kommentar bei
        // init): mfxFrameSurface1 direkt mit Data.Y/UV auf unseren Puffer, Pitch=Width. surfBuf/
        // bsBuf/surf sind an dieser Stelle sicher wiederverwendbar - das vorherige Ergebnis wurde
        // oben (falls vorhanden) bereits synchronisiert und in `out` kopiert, bevor es
        // ueberschrieben wird.
        surf = mfxFrameSurface1{}; surf.Info = surfInfo; surf.Data.Y = dstY; surf.Data.UV = dstUV; surf.Data.Pitch = (mfxU16)surfInfo.Width;
        pendingBs = mfxBitstream{}; pendingBs.Data = bsBuf.data(); pendingBs.MaxLength = (mfxU32)bsBuf.size();
        mfxStatus st;
        do { st = MFXVideoENCODE_EncodeFrameAsync(session, nullptr, &surf, &pendingBs, &pendingSyncp); if (st == MFX_WRN_DEVICE_BUSY) Sleep(1); } while (st == MFX_WRN_DEVICE_BUSY);
        if (st != MFX_ERR_NONE) pendingSyncp = nullptr;
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

// =============== Audio: WASAPI-Loopback -> libopus -> Opus-in-MPEG-TS ===============
// Ersetzt den FFmpeg-Ton-Weg. WebRTC/WHEP braucht OPUS (AAC geht dort nicht durch).
// Eigener Thread; legt fertige Opus-TS-Nutzlasten (Control-Header + Opus-Paket) mit
// 90-kHz-PTS in eine Queue. Lueckenlos Wall-Clock-getaktet (stiller Endpoint liefert
// gar nichts -> mit Stille fuellen), sonst puffert mediamtx den Ton-Track bis zum Limit.
struct AudioFrame { uint64_t pts; std::vector<uint8_t> data; };  // data = Opus-Control-Header + Opus-Paket

// Completion-Handler fuer ActivateAudioInterfaceAsync (Prozess-Loopback laeuft asynchron).
// FtmBase liefert den Free-Threaded-Marshaller (IAgileObject), den die API verlangt.
struct ActivateHandler : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr); HRESULT actHr = E_FAIL; Microsoft::WRL::ComPtr<IAudioClient> client;
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) {
        HRESULT ar = E_FAIL; Microsoft::WRL::ComPtr<IUnknown> iface;
        op->GetActivateResult(&ar, iface.GetAddressOf());
        actHr = ar; if (SUCCEEDED(ar) && iface) iface.As(&client);
        SetEvent(ev); return S_OK;
    }
};

struct AudioCapture {
    std::thread th; std::atomic<bool> stop{ false }; std::mutex mtx; std::deque<AudioFrame> q;
    std::chrono::steady_clock::time_point epoch; uint64_t basePts = 0; int SR = 48000, CH = 2; DWORD targetPid = 0;
    void push(uint64_t pts, std::vector<uint8_t>&& d) { std::lock_guard<std::mutex> l(mtx); if (q.size() < 256) q.push_back({ pts, std::move(d) }); }
    bool pop(AudioFrame& f) { std::lock_guard<std::mutex> l(mtx); if (q.empty()) return false; f = std::move(q.front()); q.pop_front(); return true; }
    bool empty() { std::lock_guard<std::mutex> l(mtx); return q.empty(); }
    void start(std::chrono::steady_clock::time_point ep, uint64_t base) { epoch = ep; basePts = base; th = std::thread([this] { run(); }); }
    void join() { stop = true; if (th.joinable()) th.join(); }

    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        winrt::com_ptr<IAudioClient> ac;
        int devSR = 48000, devCH = 2; bool devFloat = true, procMode = false;
        if (targetPid) {
            // Prozess-Loopback: NUR der Ton dieses Prozesses (+ Kindprozesse). Virtuelles Geraet ->
            // kein GetMixFormat, festes f32/48k/stereo. Faellt bei Fehler auf System-Ton zurueck.
            WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; wf.nChannels = 2; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 32; wf.nBlockAlign = 8; wf.nAvgBytesPerSec = 48000 * 8;
            AUDIOCLIENT_ACTIVATION_PARAMS ap{};
            ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
            ap.ProcessLoopbackParams.TargetProcessId = targetPid;
            ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
            PROPVARIANT pv{}; pv.vt = VT_BLOB; pv.blob.cbSize = sizeof(ap); pv.blob.pBlobData = (BYTE*)&ap;
            auto handler = Microsoft::WRL::Make<ActivateHandler>();
            Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> op;
            HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler.Get(), op.GetAddressOf());
            if (SUCCEEDED(hr)) WaitForSingleObject(handler->ev, 3000);
            if (SUCCEEDED(hr) && SUCCEEDED(handler->actHr) && handler->client) {
                handler->client.CopyTo(ac.put());
                if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, &wf, nullptr))) { printf("AUDIO: Prozess-Loopback-Init fehlgeschlagen -> System-Ton\n"); ac = nullptr; }
                else { procMode = true; printf("AUDIO: Prozess-Loopback (pid %lu, f32/48k/stereo)\n", targetPid); }
            } else printf("AUDIO: Prozess-Loopback-Aktivierung fehlgeschlagen (hr 0x%08lX) -> System-Ton\n", hr);
        }
        if (!ac) {
            // System-Loopback (Standard-Ausgabegeraet, ganzer System-Ton).
            winrt::com_ptr<IMMDeviceEnumerator> en;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), en.put_void()))) { printf("AUDIO: kein Enumerator\n"); return; }
            winrt::com_ptr<IMMDevice> mmdev; if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, mmdev.put()))) { printf("AUDIO: kein Ausgabegeraet\n"); return; }
            if (FAILED(mmdev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, ac.put_void()))) { printf("AUDIO: Activate fehlgeschlagen\n"); return; }
            WAVEFORMATEX* mix = nullptr; ac->GetMixFormat(&mix);
            bool mix48 = mix && mix->nSamplesPerSec == 48000;
            if (mix48) {
                // Standardfall (fast immer): Mischformat ist 48 kHz -> bewaehrter Weg, unveraendert.
                if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, mix, nullptr))) { printf("AUDIO: Init fehlgeschlagen\n"); CoTaskMemFree(mix); return; }
                devSR = 48000; devCH = mix->nChannels; devFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) || (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->wBitsPerSample == 32);
                CoTaskMemFree(mix);
            } else {
                // Sonderfall (z.B. 44,1-kHz-DAC/AVR): festes 48k/f32/stereo mit AUTOCONVERTPCM ->
                // WASAPI resampelt selbst, Opus bekommt trotzdem 48 kHz (kein eigener Resampler noetig).
                if (mix) { printf("AUDIO: Geraet %d Hz -> WASAPI-Resampling auf 48 kHz\n", (int)mix->nSamplesPerSec); CoTaskMemFree(mix); }
                WAVEFORMATEX want{}; want.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; want.nChannels = 2; want.nSamplesPerSec = 48000; want.wBitsPerSample = 32; want.nBlockAlign = 8; want.nAvgBytesPerSec = 48000 * 8;
                if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 2000000, 0, &want, nullptr))) { printf("AUDIO: 48k-AUTOCONVERT fehlgeschlagen -> Ton aus (Video laeuft)\n"); return; }
                devSR = 48000; devCH = 2; devFloat = true;
            }
        }
        winrt::com_ptr<IAudioCaptureClient> cap; if (FAILED(ac->GetService(__uuidof(IAudioCaptureClient), cap.put_void()))) { printf("AUDIO: GetService fehlgeschlagen\n"); return; }
        // Opus erlaubt 48/24/16/12/8 kHz. Mix ist praktisch immer 48 kHz; sonst Ton aus (Video laeuft weiter).
        if (devSR != 48000) { printf("AUDIO: Mischrate %d Hz != 48000 -> Ton deaktiviert (Resampler folgt)\n", devSR); return; }
        SR = 48000; CH = 2;

        int oerr = 0; OpusEncoder* enc = opus_encoder_create(SR, CH, OPUS_APPLICATION_AUDIO, &oerr);
        if (!enc || oerr != OPUS_OK) { printf("AUDIO: opus_encoder_create Fehler %d\n", oerr); return; }
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(128000));
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        opus_encoder_ctl(enc, OPUS_SET_VBR(1));

        const int FR = 960;                                   // 20 ms bei 48 kHz = ein Opus-Frame
        const uint64_t tickPerFrame = (uint64_t)FR * 90000 / SR;   // = 1800
        // inFrames auf die gemeinsame Epoche synchronisieren: wird die Ton-Aufnahme mitten im
        // Stream neu gestartet (Quellwechsel Monitor<->Fenster), laufen PTS kontinuierlich
        // weiter statt bei 0 zu beginnen - und die due-Schleife holt nichts rueckwirkend auf.
        uint64_t inFrames = (uint64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count() * SR / FR);
        uint64_t produced = 0; long long captured = 0;
        printf("AUDIO: %s 48000 Hz/2 ch (dev %d ch, %s) -> Opus 128k (WebRTC-tauglich)\n", procMode ? "Prozess-Loopback" : "System-Loopback", devCH, devFloat ? "f32" : "int");
        ac->Start();

        std::vector<int16_t> pending; pending.reserve(SR);    // s16-Stereo-Vorrat aus WASAPI
        std::vector<opus_int16> frame(FR * CH);
        std::vector<uint8_t> opkt(4000);
        while (!stop) {
            // 1) alle bereitliegenden WASAPI-Pakete abholen -> pending (echtes Audio)
            UINT32 pkt = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt > 0) {
                BYTE* d = nullptr; UINT32 n = 0; DWORD fl = 0; if (FAILED(cap->GetBuffer(&d, &n, &fl, nullptr, nullptr))) break;
                if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    if (devFloat) { const float* fp = (const float*)d; for (UINT32 i = 0; i < n; ++i) for (int c = 0; c < 2; ++c) { float v = fp[i * devCH + (c < devCH ? c : 0)]; if (v > 1) v = 1; if (v < -1) v = -1; pending.push_back((int16_t)(v * 32767)); } }
                    else { const int16_t* ip = (const int16_t*)d; for (UINT32 i = 0; i < n; ++i) for (int c = 0; c < 2; ++c) pending.push_back(ip[i * devCH + (c < devCH ? c : 0)]); }
                } else pending.insert(pending.end(), (size_t)n * 2, 0);
                cap->ReleaseBuffer(n); captured += n;
            }
            // 2) nach Wall-Clock faellige 960-Sample-Frames LUECKENLOS encodieren (Stille auffuellen).
            uint64_t due = (uint64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count() * SR / FR);
            while (inFrames < due) {
                std::fill(frame.begin(), frame.end(), (opus_int16)0);
                size_t haveFrames = pending.size() / 2, take = haveFrames < (size_t)FR ? haveFrames : (size_t)FR;
                if (take > 0) { memcpy(frame.data(), pending.data(), take * 2 * sizeof(int16_t)); pending.erase(pending.begin(), pending.begin() + take * 2); }
                int len = opus_encode(enc, frame.data(), FR, opkt.data(), (int)opkt.size());
                uint64_t pts = basePts + inFrames * tickPerFrame; inFrames++;
                if (len > 1) {   // >1 = echtes Paket (1 waere ein DTX-Leerpaket)
                    std::vector<uint8_t> ts; ts.reserve(len + 5);
                    ts.push_back(0x7F); ts.push_back(0xE0);   // Opus-Control-Header: 11-bit-Prefix 0x3FF, keine Trims
                    int sz = len; while (sz >= 255) { ts.push_back(0xFF); sz -= 255; } ts.push_back((uint8_t)sz); // Groesse (255er-Kette)
                    ts.insert(ts.end(), opkt.begin(), opkt.begin() + len);
                    push(pts, std::move(ts)); produced++;
                }
            }
            if (pending.size() > (size_t)SR * 2) pending.erase(pending.begin(), pending.end() - SR); // Burst-Schutz
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        ac->Stop(); opus_encoder_destroy(enc);
        printf("AUDIO-ENDE: %lld Samples, %llu Opus-Frames\n", captured, (unsigned long long)produced);
    }
};

// =============== HDR: Erkennung + Tonemap-Shader (scRGB FP16 -> SDR BGRA) ===============
// Portiert aus dem C#-Helfer: HDR-Quelle wird als FP16 (scRGB linear) erfasst; ein eigener
// Pixel-Shader komprimiert die Highlights (ACES/Hable/Reinhard/Linear) und skaliert zugleich
// auf die Zielgroesse (der VideoProcessor kann kein FP16). Kurve + Helligkeit live justierbar.
static bool detectHdr(IDXGIAdapter1* adapter) {
    for (UINT oi = 0; ; ++oi) {
        winrt::com_ptr<IDXGIOutput> o; if (adapter->EnumOutputs(oi, o.put()) == DXGI_ERROR_NOT_FOUND) break;
        auto o6 = o.try_as<IDXGIOutput6>();
        DXGI_OUTPUT_DESC1 d1{};
        if (o6 && SUCCEEDED(o6->GetDesc1(&d1)) && d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) return true;
    }
    return false;
}
static const char* TONEMAP_HLSL = R"(
cbuffer Params : register(b0) { int uMode; float uExposure; float uP1; float uP2; };
Texture2D tex : register(t0);
SamplerState samp : register(s0);
void vsmain(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 tmACES(float3 x)     { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
float3 hableU(float3 x)     { float A=0.15,B=0.50,C=0.10,D=0.20,E=0.02,F=0.30; return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F; }
float3 tmHable(float3 x)    { return saturate(hableU(x) / hableU(11.2)); }
float3 tmReinhard(float3 x) { float L=4.0; return saturate(x * (1.0 + x/(L*L)) / (1.0 + x)); }
float4 psmain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 c = max(tex.Sample(samp, uv).rgb, 0.0) * uExposure;
    float3 x;
    if (uMode == 1)      x = tmHable(c);
    else if (uMode == 2) x = tmReinhard(c);
    else if (uMode == 3) x = saturate(c);
    else                 x = tmACES(c);
    x = pow(x, 1.0 / 2.2);
    return float4(x, 1.0);
}
)";
struct TonemapStage {
    winrt::com_ptr<ID3D11VertexShader> vs; winrt::com_ptr<ID3D11PixelShader> ps;
    winrt::com_ptr<ID3D11SamplerState> samp; winrt::com_ptr<ID3D11Texture2D> srcCopy, sdrTex;
    winrt::com_ptr<ID3D11ShaderResourceView> srv; winrt::com_ptr<ID3D11RenderTargetView> rtv; winrt::com_ptr<ID3D11Buffer> cb;
    int W = 0, H = 0, outW = 0, outH = 0, mode = 0, ctlCtr = 0; float exposure = 1.0f;
    int lbX = 0, lbY = 0, lbW = 0, lbH = 0;   // Letterbox-Zielrechteck in sdrTex (Aspect-Erhalt beim Quellwechsel)
    std::string ctlPath; uint64_t ctlMtime = ~0ull;

    // Eingangsseite (FP16-Kopie der Quelle) neu aufbauen - beim Start UND bei Quellwechsel/Resize.
    bool reinitInput(ID3D11Device* dev, int w, int h) {
        W = w; H = h; srcCopy = nullptr; srv = nullptr;
        D3D11_TEXTURE2D_DESC td{}; td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, srcCopy.put()))) return false;
        return SUCCEEDED(dev->CreateShaderResourceView(srcCopy.get(), nullptr, srv.put()));
    }
    void setViewport(int x, int y, int w, int h) { lbX = x; lbY = y; lbW = w; lbH = h; }   // Letterbox-Ziel im sdrTex
    bool init(ID3D11Device* dev, int w, int h, int ow, int oh) {
        outW = ow; outH = oh; lbX = 0; lbY = 0; lbW = ow; lbH = oh;
        winrt::com_ptr<ID3DBlob> vsb, psb, err;
        if (FAILED(D3DCompile(TONEMAP_HLSL, strlen(TONEMAP_HLSL), nullptr, nullptr, nullptr, "vsmain", "vs_5_0", 0, 0, vsb.put(), err.put()))) return false;
        if (FAILED(D3DCompile(TONEMAP_HLSL, strlen(TONEMAP_HLSL), nullptr, nullptr, nullptr, "psmain", "ps_5_0", 0, 0, psb.put(), err.put()))) return false;
        if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, vs.put()))) return false;
        if (FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, ps.put()))) return false;
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        dev->CreateSamplerState(&sd, samp.put());
        if (!reinitInput(dev, w, h)) return false;
        D3D11_TEXTURE2D_DESC od{}; od.Width = outW; od.Height = outH; od.MipLevels = 1; od.ArraySize = 1; od.Format = DXGI_FORMAT_B8G8R8A8_UNORM; od.SampleDesc.Count = 1; od.Usage = D3D11_USAGE_DEFAULT; od.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&od, nullptr, sdrTex.put()))) return false;
        if (FAILED(dev->CreateRenderTargetView(sdrTex.get(), nullptr, rtv.put()))) return false;
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, cb.put()))) return false;
        char* tmp = getenv("TEMP"); ctlPath = std::string(tmp ? tmp : ".") + "\\lumora-hdr.txt";
        return true;
    }
    void updateBuffer(ID3D11DeviceContext* ctx) {
        D3D11_MAPPED_SUBRESOURCE m; if (SUCCEEDED(ctx->Map(cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            struct { int mode; float exp; float p1, p2; } p{ mode, exposure, 0, 0 };
            memcpy(m.pData, &p, sizeof(p)); ctx->Unmap(cb.get(), 0);
        }
    }
    // Steuerdatei %TEMP%\lumora-hdr.txt ("<mode> <exposure>") live einlesen (alle 15 Frames).
    void readControl(ID3D11DeviceContext* ctx, bool force) {
        if (!force && (++ctlCtr % 15) != 0) return;
        WIN32_FILE_ATTRIBUTE_DATA fa{}; uint64_t mt = 0;
        if (GetFileAttributesExA(ctlPath.c_str(), GetFileExInfoStandard, &fa)) mt = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
        if (mt == ctlMtime) { if (force) updateBuffer(ctx); return; }
        ctlMtime = mt;
        if (mt != 0) { FILE* f = nullptr; fopen_s(&f, ctlPath.c_str(), "r"); if (f) { int m = 0; float e = 1; int n = fscanf_s(f, "%d %f", &m, &e); if (n >= 1) mode = m; if (n >= 2 && e > 0.01f && e < 20.0f) exposure = e; fclose(f); printf("HDR-TM mode=%d exp=%.2f\n", mode, exposure); } }
        updateBuffer(ctx);
    }
    // Neuen WGC-Frame uebernehmen (getrennt vom Rendern: beim Frame-Pacing wird der letzte
    // Frame aus srcCopy erneut gerendert, wenn WGC bei ruhigem Bild nichts Neues liefert).
    void update(ID3D11DeviceContext* ctx, ID3D11Texture2D* srcFp16) { ctx->CopyResource(srcCopy.get(), srcFp16); }
    // srcCopy tonemappen + auf Zielgroesse rendern -> sdrTex (BGRA, SDR).
    void render(ID3D11DeviceContext* ctx) {
        readControl(ctx, false);
        ctx->VSSetShader(vs.get(), nullptr, 0); ctx->PSSetShader(ps.get(), nullptr, 0);
        ID3D11ShaderResourceView* s = srv.get(); ctx->PSSetShaderResources(0, 1, &s);
        ID3D11SamplerState* sm = samp.get(); ctx->PSSetSamplers(0, 1, &sm);
        ID3D11Buffer* c = cb.get(); ctx->PSSetConstantBuffers(0, 1, &c);
        ID3D11RenderTargetView* r = rtv.get(); ctx->OMSetRenderTargets(1, &r, nullptr);
        const float black[4] = { 0, 0, 0, 1 }; ctx->ClearRenderTargetView(rtv.get(), black);   // Letterbox-Raender schwarz
        D3D11_VIEWPORT vp{ (float)lbX, (float)lbY, (float)lbW, (float)lbH, 0, 1 }; ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->Draw(3, 0);
        ID3D11RenderTargetView* nullr = nullptr; ctx->OMSetRenderTargets(1, &nullr, nullptr); // RTV loesen fuer den folgenden VideoProcessor-Input
    }
};

// =============== Fensterliste (--list) + HDR-Status (--hdr-check) fuer die UI ===============
// main.js ruft "lumora-capture --list" und erwartet je sichtbarem Fenster: HWND \t Titel \t
// Icon-PNG-Base64 (UTF-8). Ohne diese Ausgabe bliebe die Fensterauswahl in der App leer.
static std::string b64encode(const uint8_t* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t x = (uint32_t)d[i] << 16; if (i + 1 < n) x |= (uint32_t)d[i + 1] << 8; if (i + 2 < n) x |= d[i + 2];
        o.push_back(t[(x >> 18) & 63]); o.push_back(t[(x >> 12) & 63]);
        o.push_back(i + 1 < n ? t[(x >> 6) & 63] : '='); o.push_back(i + 2 < n ? t[x & 63] : '=');
    }
    return o;
}
static int pngEncoderClsid(CLSID* clsid) {
    UINT num = 0, size = 0; Gdiplus::GetImageEncodersSize(&num, &size); if (size == 0) return -1;
    std::vector<uint8_t> buf(size); auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) if (wcscmp(codecs[i].MimeType, L"image/png") == 0) { *clsid = codecs[i].Clsid; return 0; }
    return -1;
}
static std::string iconBase64(HWND hwnd) {
    HICON hIcon = nullptr; DWORD_PTR r = 0;
    SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 200, &r); hIcon = (HICON)r;
    if (!hIcon) { SendMessageTimeoutW(hwnd, WM_GETICON, 2 /*ICON_SMALL2*/, 0, SMTO_ABORTIFHUNG, 200, &r); hIcon = (HICON)r; }
    if (!hIcon) hIcon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICON);
    if (!hIcon) hIcon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICONSM);
    if (!hIcon) return "";
    Gdiplus::Bitmap* ico = Gdiplus::Bitmap::FromHICON(hIcon); if (!ico) return "";
    std::string out;
    Gdiplus::Bitmap sm24(24, 24, PixelFormat32bppARGB);   // Anzeigegroesse -> kleine Base64 ('small' ist ein Win-Makro!)
    { Gdiplus::Graphics g(&sm24); g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic); g.DrawImage(ico, 0, 0, 24, 24); }
    CLSID clsid; IStream* stm = nullptr;
    if (pngEncoderClsid(&clsid) == 0 && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stm))) {
        if (sm24.Save(stm, &clsid, nullptr) == Gdiplus::Ok) {
            HGLOBAL hg = nullptr; GetHGlobalFromStream(stm, &hg);
            if (hg) { SIZE_T sz = GlobalSize(hg); void* p = GlobalLock(hg); if (p) out = b64encode((uint8_t*)p, sz); GlobalUnlock(hg); }
        }
        stm->Release();
    }
    delete ico;
    return out;
}
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static int listWindows() {
    Gdiplus::GdiplusStartupInput gsi; ULONG_PTR gtoken = 0; Gdiplus::GdiplusStartup(&gtoken, &gsi, nullptr);
    EnumWindows([](HWND h, LPARAM) -> BOOL {
        if (!IsWindowVisible(h)) return TRUE;
        int len = GetWindowTextLengthW(h); if (len == 0) return TRUE;
        std::wstring title(len + 1, 0); GetWindowTextW(h, &title[0], len + 1); title.resize(wcslen(title.c_str()));
        size_t a = title.find_first_not_of(L" \t\r\n"); if (a == std::wstring::npos) return TRUE;
        title = title.substr(a, title.find_last_not_of(L" \t\r\n") - a + 1);
        int cloaked = 0; DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof(cloaked)); if (cloaked) return TRUE;
        std::string line = std::to_string((long long)(intptr_t)h) + "\t" + wideToUtf8(title) + "\t" + iconBase64(h) + "\n";
        fwrite(line.data(), 1, line.size(), stdout);
        return TRUE;
    }, 0);
    Gdiplus::GdiplusShutdown(gtoken);
    return 0;
}
static int hdrCheck() {   // pro Monitor "1" (HDR/PQ aktiv) oder "0"
    winrt::com_ptr<IDXGIFactory1> fac; if (FAILED(CreateDXGIFactory1(winrt::guid_of<IDXGIFactory1>(), fac.put_void()))) return 1;
    for (UINT ai = 0; ; ++ai) {
        winrt::com_ptr<IDXGIAdapter1> a; if (fac->EnumAdapters1(ai, a.put()) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT oi = 0; ; ++oi) {
            winrt::com_ptr<IDXGIOutput> o; if (a->EnumOutputs(oi, o.put()) == DXGI_ERROR_NOT_FOUND) break;
            auto o6 = o.try_as<IDXGIOutput6>(); DXGI_OUTPUT_DESC1 d1{};
            printf("%d\n", (o6 && SUCCEEDED(o6->GetDesc1(&d1)) && d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ? 1 : 0);
        }
    }
    return 0;
}

// HMONITOR des idx-ten Bildschirms (EnumDisplayMonitors-Reihenfolge - GLEICH wie die
// list-sources-Auswahl in der Shell). Fallback Hauptmonitor, falls idx nicht existiert.
struct MonPick { int want; int cur; HMONITOR hm; };
static BOOL CALLBACK monPickProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    auto* p = (MonPick*)lp; if (p->cur++ == p->want) { p->hm = hm; return FALSE; } return TRUE;
}
static HMONITOR monitorByIndex(int idx) {
    MonPick p{ idx, 0, nullptr };
    EnumDisplayMonitors(nullptr, nullptr, monPickProc, (LPARAM)&p);
    return p.hm ? p.hm : MonitorFromPoint({ 0,0 }, MONITOR_DEFAULTTOPRIMARY);
}
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; ++i) { std::string a = argv[i]; if (a == "--list") return listWindows(); if (a == "--hdr-check") return hdrCheck(); }
    int fps = 60, mbit = 20, port = 9998, maxFrames = 0, scaleH = 0, monIdx = 0; std::string host = "127.0.0.1", tsout, encName = "auto", codecName = "auto";
    HWND targetHwnd = nullptr; bool findWindow = false, forceHdr = false; DWORD audioPid = 0;
    for (int i = 1; i < argc; ++i) { std::string a = argv[i];
        if (a == "--fps" && i + 1 < argc) fps = atoi(argv[++i]); else if (a == "--bitrate" && i + 1 < argc) mbit = atoi(argv[++i]);
        else if (a == "--mtx-host" && i + 1 < argc) host = argv[++i]; else if (a == "--mtx-port" && i + 1 < argc) port = atoi(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) maxFrames = atoi(argv[++i]); else if (a == "--tsout" && i + 1 < argc) tsout = argv[++i];
        else if (a == "--encoder" && i + 1 < argc) encName = argv[++i];
        else if (a == "--codec" && i + 1 < argc) codecName = argv[++i];   // auto|h264|av1: av1/auto = AV1 zusaetzlich zu H.264 (Doppel-Encode, Fallback pro Zuschauer)
        else if (a == "--hwnd" && i + 1 < argc) targetHwnd = (HWND)(uintptr_t)strtoull(argv[++i], nullptr, 0);
        else if (a == "--window") findWindow = true;
        else if (a == "--scale" && i + 1 < argc) scaleH = atoi(argv[++i]);
        else if (a == "--monitor" && i + 1 < argc) monIdx = atoi(argv[++i]);   // Bildschirm-Index (EnumDisplayMonitors-Reihenfolge wie die UI-Quellenliste)
        else if (a == "--hdr-force") forceHdr = true;   // Diagnose: HDR-Pfad (FP16+Tonemap) auch ohne aktives HDR erzwingen
        else if (a == "--audio-pid" && i + 1 < argc) { audioPid = (DWORD)strtoul(argv[++i], nullptr, 0); g_withAudio = true; }
        else if (a == "--audio") g_withAudio = true;
        else if (a == "--audio-nobytes") { g_withAudio = true; g_audioNoBytes = true; } }
    if (findWindow && !targetHwnd) { FindCtx fc; fc.self = GetConsoleWindow(); EnumWindows(enumProc, (LPARAM)&fc); targetHwnd = fc.best; }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (!wg::GraphicsCaptureSession::IsSupported()) { printf("WGC nicht unterstuetzt\n"); return 1; }

    // GPU/Encoder-Auswahl: passenden Adapter finden (NVENC->NVIDIA, AMF->AMD)
    winrt::com_ptr<IDXGIFactory1> fac; CreateDXGIFactory1(winrt::guid_of<IDXGIFactory1>(), fac.put_void());
    winrt::com_ptr<IDXGIAdapter1> nvA, amdA, intelA; for (UINT ai = 0;; ++ai) { winrt::com_ptr<IDXGIAdapter1> a; if (fac->EnumAdapters1(ai, a.put()) == DXGI_ERROR_NOT_FOUND) break; DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d); if (d.VendorId == 0x10DE && !nvA) nvA = a; else if (d.VendorId == 0x1002 && !amdA) amdA = a; else if (d.VendorId == 0x8086 && !intelA) intelA = a; }
    // Encoder-Wahl: explizit oder auto (Vorrang NVIDIA > AMD > Intel).
    bool useQsv = (encName == "qsv") || (encName == "auto" && !nvA && !amdA && intelA);
    bool useAmf = (encName == "amf") || (encName == "auto" && !nvA && amdA);
    winrt::com_ptr<IDXGIAdapter1> pick = useQsv ? intelA : (useAmf ? amdA : nvA);
    if (!pick) { printf("FEHLER: kein passender GPU-Adapter fuer '%s'\n", encName.c_str()); return 1; }

    winrt::com_ptr<ID3D11Device> dev; winrt::com_ptr<ID3D11DeviceContext> ctx; D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(pick.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, dev.put(), &fl, ctx.put()))) { printf("D3D11CreateDevice fehlgeschlagen\n"); return 1; }
    auto dxgiDev = dev.as<IDXGIDevice>(); wg::IDirect3DDevice d3dDevice{ nullptr };
    CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(), reinterpret_cast<::IInspectable**>(winrt::put_abi(d3dDevice)));
    auto interop = winrt::get_activation_factory<wg::GraphicsCaptureItem>().as<IGraphicsCaptureItemInterop>();
    auto vdev = dev.as<ID3D11VideoDevice>(); auto vctx = ctx.as<ID3D11VideoContext>();
    // HDR-Quelle -> per FP16 (scRGB linear) erfassen und im eigenen Shader tonemappen (VP kann kein FP16).
    bool hdr = forceHdr || detectHdr(pick.get());
    auto capFmt = hdr ? wg::DirectXPixelFormat::R16G16B16A16Float : wg::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    // Gelben WGC-Aufnahme-Rahmen abschalten (einmalig; gilt fuer alle folgenden Sessions).
    try { wg::GraphicsCaptureAccess::RequestAccessAsync(wg::GraphicsCaptureAccessKind::Borderless).get(); } catch (...) {}

    // ======= DYNAMISCHE QUELLE: Capture-Seite ist zur Laufzeit austauschbar =======
    // Encoder, Muxer, Zeitachse, Ton und UDP laufen beim Quellwechsel/Resize WEITER -
    // nur Capture-Item/FramePool/Scaler werden neu aufgebaut. Fuer den Zuschauer ist
    // ein Wechsel damit ein nahtloser Schnitt (kein Reconnect, kein Freeze).
    wg::GraphicsCaptureItem item{ nullptr };
    wg::Direct3D11CaptureFramePool framePool{ nullptr };
    wg::GraphicsCaptureSession session{ nullptr };
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> venum; winrt::com_ptr<ID3D11VideoProcessor> vproc;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> outView; winrt::com_ptr<ID3D11Texture2D> nv12, bgraIn;
    TonemapStage tonemap; bool tonemapReady = false;
    HWND curHwnd = nullptr; int W = 0, H = 0, outW = 0, outH = 0;

    // Scaler-Kette (VideoProcessor + Eingangstexturen) fuer die AKTUELLE Quellgroesse W x H
    // aufbauen; Ausgabe bleibt konstant outW x outH. Letterbox erhaelt das Seitenverhaeltnis.
    auto rebuildScaler = [&]() -> bool {
        int lw = outW, lh = W ? (int)(((long long)outW * H / W)) & ~1 : outH;
        if (lh > outH) { lh = outH; lw = H ? (int)(((long long)outH * W / H)) & ~1 : outW; }
        int lx = ((outW - lw) / 2) & ~1, ly = ((outH - lh) / 2) & ~1;
        venum = nullptr; vproc = nullptr; outView = nullptr; bgraIn = nullptr;
        // Bei HDR liefert der Tonemap-Shader BGRA-SDR bereits in Zielgroesse (inkl. Letterbox) -> VP nur BGRA->NV12 1:1.
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{}; cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE; cd.InputWidth = hdr ? outW : W; cd.InputHeight = hdr ? outH : H; cd.OutputWidth = outW; cd.OutputHeight = outH; cd.InputFrameRate = { (UINT)fps,1 }; cd.OutputFrameRate = { (UINT)fps,1 }; cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, venum.put()))) return false;
        if (FAILED(vdev->CreateVideoProcessor(venum.get(), 0, vproc.put()))) return false;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(vdev->CreateVideoProcessorOutputView(nv12.get(), venum.get(), &ovd, outView.put()))) return false;
        if (hdr) {
            if (!tonemap.reinitInput(dev.get(), W, H)) return false;
            tonemap.setViewport(lx, ly, lw, lh);
        } else {
            // Eigene BGRA-Textur EXAKT in FramePool-Groesse (CopyResource braucht identische Masse).
            D3D11_TEXTURE2D_DESC bd{}; bd.Width = W; bd.Height = H; bd.MipLevels = 1; bd.ArraySize = 1; bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bd.SampleDesc.Count = 1; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&bd, nullptr, bgraIn.put()))) return false;
            // Letterbox im VP: Quelle aspect-erhaltend in outW x outH, Raender schwarz.
            RECT dr{ lx, ly, lx + lw, ly + lh };
            vctx->VideoProcessorSetStreamDestRect(vproc.get(), 0, TRUE, &dr);
            D3D11_VIDEO_COLOR black{}; black.RGBA = { 0, 0, 0, 1 };
            vctx->VideoProcessorSetOutputBackgroundColor(vproc.get(), FALSE, &black);
        }
        printf("SOURCE: %dx%d -> %dx%d (Bild %dx%d @ %d,%d)\n", W, H, outW, outH, lw, lh, lx, ly);
        return true;
    };
    // Capture-Quelle (Monitor oder Fenster) neu aufsetzen. initial=true beim Programmstart.
    auto setupSource = [&](HWND hw, bool initial) -> bool {
        if (session) { try { session.Close(); } catch (...) {} session = nullptr; }
        if (framePool) { try { framePool.Close(); } catch (...) {} framePool = nullptr; }
        item = nullptr;
        if (hw) {
            if (FAILED(interop->CreateForWindow(hw, winrt::guid_of<wg::GraphicsCaptureItem>(), winrt::put_abi(item))) || !item) { printf("SOURCE: Fenster-Capture fehlgeschlagen (HWND ungueltig?)\n"); return false; }
            wchar_t t[128] = {}; GetWindowTextW(hw, t, 127); printf("Quelle: Fenster \"%ls\"\n", t);
        } else {
            HMONITOR hmon = monitorByIndex(monIdx);   // gewaehlter Bildschirm (nicht mehr fest Hauptmonitor)
            if (FAILED(interop->CreateForMonitor(hmon, winrt::guid_of<wg::GraphicsCaptureItem>(), winrt::put_abi(item))) || !item) { printf("SOURCE: Monitor-Capture fehlgeschlagen\n"); return false; }
            printf("Quelle: Bildschirm %d\n", monIdx);
        }
        curHwnd = hw;
        auto s2 = item.Size(); W = s2.Width; H = s2.Height;
        // War das Fenster MINIMIERT, liefert WGC hier eine falsche Platzhaltergroesse (real
        // 219x30 statt der echten Fensterinhaltsgroesse) - outW/outH werden aus GENAU DIESER
        // ersten Groesse fuer die ganze Session fest verdrahtet (rebuildScaler passt bei einem
        // spaeteren "Groessenwechsel" nur die Letterbox INNERHALB von outW/outH an, der Encoder
        // selbst bleibt auf der falschen Aufloesung stehen - real reproduziert: Firefox minimiert
        // vor Stream-Start -> Stream haengt dauerhaft in winzigem 1046x144-Letterbox fest).
        // Fix OHNE das Fenster anzufassen (kein ShowWindow/SetWindowPos - das Fenster bleibt
        // exakt wie der Nutzer es hingelegt hat): GetWindowPlacement liefert die "normale"
        // (wiederhergestellte) Groesse auch waehrend das Fenster minimiert bleibt - damit outW/
        // outH korrekt vorbelegen. WGC selbst captured den Inhalt eines minimierten Fensters auf
        // aktuellen Windows-Versionen weiterhin (eigene Faehigkeit, kein Workaround noetig);
        // nur die anfaengliche Groessenermittlung war die Fehlerquelle.
        if (hw && IsIconic(hw)) {
            WINDOWPLACEMENT wp{ sizeof(wp) };
            if (GetWindowPlacement(hw, &wp)) {
                int nw = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
                int nh = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
                if (nw > 0 && nh > 0) { W = nw; H = nh; s2.Width = nw; s2.Height = nh; }
            }
        }
        framePool = wg::Direct3D11CaptureFramePool::CreateFreeThreaded(d3dDevice, capFmt, 3, s2);
        session = framePool.CreateCaptureSession(item);
        try { session.IsBorderRequired(false); } catch (...) {}
        session.StartCapture();
        return initial ? true : rebuildScaler();   // beim Start folgt rebuildScaler nach outW/outH-Berechnung
    };

    // Zielaufloesung aus einer Quellgroesse ableiten (--scale N = Hoehe, gerade Kanten) +
    // Mindest-Encodergroesse (HW-Encoder scheitern an winzigen Quellen, s.u.). Eigene Funktion,
    // weil sowohl der Start als auch ein spaeterer Resize-Neuaufbau (s.u.) sie brauchen.
    auto computeOutSize = [&](int w, int h, int& ow, int& oh) {
        ow = w & ~1; oh = h & ~1;
        if (scaleH > 0 && scaleH < h) { oh = scaleH & ~1; ow = ((w * oh / h) + 1) & ~1; }
        // Mindest-Encodergroesse: HW-Encoder (AMF/QSV, teils NVENC) scheitern an winzigen Quellen -
        // ein 218x28-Fenster loeste auf AMD/Intel "Encoder-Init fehlgeschlagen" + Restart-Schleife
        // aus (mtx-Timeout). Ausgabe seitenverhaeltnis-erhaltend auf ein gueltiges Minimum ziehen
        // (kein Verzerren; bei normalem Seitenverhaeltnis fuellt die Quelle den Rahmen randlos).
        const int MIN_W = 256, MIN_H = 144;
        if (ow < MIN_W || oh < MIN_H) {
            double sc = (double)MIN_W / ow; double sy = (double)MIN_H / oh; if (sy > sc) sc = sy;
            ow = ((int)(ow * sc + 0.5)) & ~1; oh = ((int)(oh * sc + 0.5)) & ~1;
            if (ow < MIN_W) ow = MIN_W; if (oh < MIN_H) oh = MIN_H;   // Rundungs-Rest
            printf("HINWEIS: Quelle %dx%d zu klein -> Ausgabe %dx%d (Encoder-Mindestgroesse)\n", w, h, ow, oh);
        }
    };

    if (!setupSource(targetHwnd, true)) { printf("FEHLER: Capture-Start fehlgeschlagen\n"); return 1; }
    computeOutSize(W, H, outW, outH);
    D3D11_TEXTURE2D_DESC nd{}; nd.Width = outW; nd.Height = outH; nd.MipLevels = 1; nd.ArraySize = 1; nd.Format = DXGI_FORMAT_NV12; nd.SampleDesc.Count = 1; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_RENDER_TARGET;
    dev->CreateTexture2D(&nd, nullptr, nv12.put());
    if (hdr) { if (!tonemap.init(dev.get(), W, H, outW, outH)) { printf("FEHLER: HDR-Tonemap-Init fehlgeschlagen\n"); return 3; } tonemap.readControl(ctx.get(), true); tonemapReady = true; }
    if (!rebuildScaler()) { printf("FEHLER: Scaler-Init fehlgeschlagen\n"); return 3; }

    auto newEncoder = [&]() -> Encoder* { return useQsv ? (Encoder*)new QsvEncoder() : (useAmf ? (Encoder*)new AmfEncoder() : (Encoder*)new NvencEncoder()); };
    // AV1 bekommt die EINGESTELLTE Bitrate, nicht mehr die halbe. Die alte Halbierung
    // unterstellte die aus Software-Encodern bekannte ~50%-Ersparnis; beim Echtzeit-
    // Encoding auf der GPU sind es real eher 20-30%, bei schnellem Material (Rennspiel)
    // noch weniger. Sie deckelte AV1 zudem unbeabsichtigt bei 25 Mbit, weil die Auswahl
    // bei 50 endet. Wer sparen will, waehlt jetzt bewusst einen kleineren Wert.
    auto av1Kbit = [&](int kbit) { return kbit < 500 ? 500 : kbit; };
    Encoder* encoder = nullptr;      // H.264 (PID_VIDEO) - bedarfsgesteuert, aber Warm-Default
    Encoder* encoderAv1 = nullptr;   // AV1 (PID_VIDEO_AV1) - nur wenn ein AV1-Zuschauer verbunden ist
    // Bedarfsgesteuertes Encoding: Der Relay meldet der Shell, welche Codecs die aktuellen
    // Zuschauer brauchen; die Shell schreibt die Zielmenge nach %TEMP%\lumora-codec.txt.
    // Doppel-Encode (H.264 + AV1) laeuft nur im echten Mischfall. av1Capable = HW kann AV1
    // ueberhaupt (NVENC ab RTX 40, AMF ab RX 7000, QSV ab Arc/Xe) - sonst bleibt es bei H.264.
    bool av1Capable = false;
    { Encoder* probe = newEncoder();   // einmalige Faehigkeitspruefung: AV1-Init versuchen
      if (probe->init(dev.get(), outW, outH, fps, (mbit + 1) / 2 > 0 ? (mbit + 1) / 2 : 1, true)) { av1Capable = true; delete probe; }
      else { delete probe; if (codecName == "av1") printf("HINWEIS: AV1 angefordert, aber Encoder ohne AV1-Support -> nur H.264\n"); } }
    // Encoder-Menge auf einen Sollzustand bringen (zwischen zwei Frames aufgerufen -> sicher).
    // curKbit traegt die aktuelle Bitrate; wird weiter unten initialisiert - hier lokal gehalten.
    auto reconcile = [&](bool wantH264, bool wantAv1, int kbit) {
        if (!wantH264 && !wantAv1) wantH264 = true;   // Sicherung: nie beide aus
        if (!av1Capable) wantAv1 = false;
        if (wantH264 && !encoder) { Encoder* e = newEncoder(); if (e->init(dev.get(), outW, outH, fps, kbit / 1000 > 0 ? kbit / 1000 : 1)) { encoder = e; printf("codec: H.264 dazu\n"); } else { delete e; } }
        else if (!wantH264 && encoder) { delete encoder; encoder = nullptr; printf("codec: H.264 weg\n"); }
        if (wantAv1 && !encoderAv1) { Encoder* e = newEncoder(); int am = av1Kbit(kbit) / 1000; if (am < 1) am = 1; if (e->init(dev.get(), outW, outH, fps, am, true)) { encoderAv1 = e; printf("codec: AV1 dazu\n"); } else { delete e; } }
        else if (!wantAv1 && encoderAv1) { delete encoderAv1; encoderAv1 = nullptr; printf("codec: AV1 weg\n"); }
        // Flags aus der TATSAECHLICHEN Encoder-Praesenz ableiten (nicht nur bei Uebergaengen) -
        // sonst bleibt g_h264 auf dem Startwert true, obwohl gar kein H.264-Encoder existiert.
        g_h264 = (encoder != nullptr); g_av1 = (encoderAv1 != nullptr);
    };
    // Startmenge: aus lumora-codec.txt, falls die Shell sie schon geschrieben hat; sonst aus
    // --codec (Standalone-Test: auto/av1 -> beide, h264 -> nur H.264). H.264 immer warm.
    std::string codecPath = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "\\lumora-codec.txt";
    bool wantH264 = true, wantAv1 = (codecName == "av1" || codecName == "auto") && av1Capable;
    { FILE* cf = nullptr; fopen_s(&cf, codecPath.c_str(), "r"); if (cf) { char buf[32] = { 0 }; fread(buf, 1, 31, cf); fclose(cf); std::string cs = buf; wantH264 = cs.find("h264") != std::string::npos; wantAv1 = av1Capable && cs.find("av1") != std::string::npos; if (!wantH264 && !wantAv1) wantH264 = true; } }
    reconcile(wantH264, wantAv1, mbit * 1000);
    if (!encoder && !encoderAv1) { printf("FEHLER: Encoder-Init fehlgeschlagen\n"); return 2; }
    printf("codec=%s\n", av1Capable ? "av1" : "h264");   // Faehigkeit fuer die Shell (Status/OSD)
    uint64_t codecMtime = 0;

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons((u_short)port); inet_pton(AF_INET, host.c_str(), &dst.sin_addr);
    DXGI_ADAPTER_DESC1 gd; pick->GetDesc1(&gd);
    printf("lumora-capture: Aufnahme %dx%d -> Ausgabe %dx%d @ %dfps, %d Mbit | %s | Encoder %s auf %ls -> mediamtx %s:%d (ohne FFmpeg)\n", W, H, outW, outH, fps, mbit, hdr ? "HDR->SDR (Tonemap)" : "SDR", encoder ? encoder->name() : (encoderAv1 ? encoderAv1->name() : "-"), gd.Description, host.c_str(), port);

    static const uint8_t AUD[6] = { 0,0,0,1, 0x09, 0xF0 };
    FILE* tsf = nullptr; if (!tsout.empty()) fopen_s(&tsf, tsout.c_str(), "wb");
    const uint64_t basePts = 90000;                            // 1 s Vorlauf, gemeinsame Zeitbasis fuer Bild + Ton
    // Live-Bitrate: Lumora schreibt die Ziel-kbit in %TEMP%\lumora-bitrate.txt; der Helfer stellt
    // den Encoder ohne Neustart um (nahtlos, kein Stream-Abriss bei adaptiver Bitrate).
    std::string brPath = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "\\lumora-bitrate.txt";
    uint64_t brMtime = ~0ull; int curKbit = mbit * 1000;
    uint64_t inFrame = 0, outFrame = 0, frameTick = 0; int tries = 0; auto t0 = std::chrono::steady_clock::now();
    // Ton als Pointer: beim Quellwechsel (Monitor<->Fenster) wird die Aufnahme-Seite neu
    // gestartet (System- vs. Prozess-Loopback); die PTS laufen dank Epoch-Sync nahtlos weiter.
    AudioCapture* audio = new AudioCapture();
    auto audioPidFor = [&](HWND hw) -> DWORD { if (audioPid) return audioPid; DWORD p = 0; if (hw) GetWindowThreadProcessId(hw, &p); return p; };
    if (g_withAudio) {
        audio->targetPid = audioPidFor(targetHwnd);   // Fenster-Modus: nur der Ton dieses Prozesses
        audio->start(t0, basePts);
        // Auf die erste Ton-Probe warten, BEVOR wir Video-Pakete senden: sonst sieht mediamtx
        // anfangs nur Video, puffert den deklarierten (noch leeren) Ton-Track und sprengt sein
        // Limit ("max recorded size exceeded"). Timeout, falls kein Audiogeraet. Bild und Ton
        // teilen dieselbe Epoche t0 -> der Warte-Versatz steckt in beiden PTS, bleibt synchron.
        for (int w = 0; w < 400 && audio->empty(); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (audio->empty()) printf("AUDIO: keine Ton-Probe nach 4s - starte trotzdem (Stream evtl. ohne Ton)\n");
    }
    // Quellwechsel-Steuerdatei: "monitor" oder "hwnd <id>". Beim Start nur den Stand merken
    // (NICHT anwenden - ein alter Eintrag darf die per Argument gewaehlte Quelle nicht kippen).
    std::string srcPath = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "\\lumora-source.txt";
    uint64_t srcMtime = 0;
    { WIN32_FILE_ATTRIBUTE_DATA fa{}; if (GetFileAttributesExA(srcPath.c_str(), GetFileExInfoStandard, &fa)) srcMtime = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime; }
    // Quelle live wechseln: Capture+Scaler neu, Ton-Seite passend neu - Encoder/Muxer laufen durch.
    auto switchSource = [&](HWND hw) {
        if (!setupSource(hw, false)) { if (hw) { printf("SOURCE: Wechsel fehlgeschlagen -> Monitor\n"); setupSource(nullptr, false); } else return; }
        if (g_withAudio) { audio->join(); delete audio; audio = new AudioCapture(); audio->targetPid = audioPidFor(curHwnd); audio->start(t0, basePts); }
    };
    // FRAME-PACING (fester fps-Takt): WGC liefert nur bei BILDAENDERUNG einen Frame - bei
    // ruhigem Desktop kommen Frames unregelmaessig (30-60 schwankend) -> Ruckeln beim
    // Zuschauer (real gemeldet; beim 25fps-Video passt es zufaellig). Darum entkoppelt:
    // neue WGC-Frames werden uebernommen, WANN IMMER sie kommen; encodiert wird strikt im
    // Wall-Clock-Takt (fps) - bei Stillstand wird der letzte Frame wiederholt (wie ddagrab).
    bool haveFrame = false; uint64_t frameIdx = 0;
    // Deutliche Groessenaenderung (z.B. kleines Video -> Vollbild, oder ein minimiert
    // gestartetes Fenster wird nachtraeglich wiederhergestellt): rebuildScaler() passt
    // nur die Letterbox INNERHALB der bestehenden outW/outH an - der Encoder selbst bliebe
    // auf der ALTEN Aufloesung stehen (dauerhaft unscharf/falsch, s. Audit-Befund im
    // FFmpeg-Pfad, main.js bcStartWindowCapture). Dort loest man das durch einen kompletten
    // Prozess-Neustart bei Flaechenverhaeltnis >=1.5x/<=0.67x, 3s stabil; hier (ein einziger
    // Prozess, kein Neustart moeglich) stattdessen Encoder+D3D11-Pipeline in-process neu
    // aufsetzen, sobald diese Bedingung erfuellt ist.
    int srcBaseW = W, srcBaseH = H;   // Quellgroesse, aus der die AKTUELLEN outW/outH/der Encoder stammen
    bool resizePending = false; std::chrono::steady_clock::time_point resizePendingAt;
    while ((maxFrames == 0 || (int)frameTick < maxFrames) && (haveFrame || tries < 3000)) {
        // 1) alle bereitliegenden WGC-Frames abholen, den NEUESTEN uebernehmen.
        for (;;) {
            auto f = framePool.TryGetNextFrame(); if (!f) break;
            // RESIZE (Fenster geaendert / Spiel wechselt Aufloesung): FramePool + Scaler auf die
            // neue Quellgroesse umbauen; Encoder/Muxer laufen unveraendert weiter (Letterbox).
            auto cs = f.ContentSize();
            if (cs.Width != W || cs.Height != H) {
                if (cs.Width < 16 || cs.Height < 16) continue;   // minimiert/degeneriert -> letzter Frame laeuft weiter
                printf("SOURCE: Groessenwechsel %dx%d -> %dx%d\n", W, H, cs.Width, cs.Height);
                framePool.Recreate(d3dDevice, capFmt, 3, cs);
                W = cs.Width; H = cs.Height; rebuildScaler();
                double ratio = ((double)W * H) / ((double)srcBaseW * srcBaseH);
                if (ratio >= 1.5 || ratio <= 0.67) { resizePending = true; resizePendingAt = std::chrono::steady_clock::now(); }
                break;   // diesen (alten) Frame verwerfen; die naechsten kommen in neuer Groesse
            }
            auto access = f.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> ft; access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), ft.put_void());
            // sofort in eigene Textur (WGC recycelt den FramePool-Buffer): HDR -> FP16-Kopie
            // der Tonemap-Stufe, SDR -> bgraIn. Von dort wird beim Takt-Tick gerendert/encodiert.
            if (hdr) tonemap.update(ctx.get(), ft.get()); else ctx->CopyResource(bgraIn.get(), ft.get());
            haveFrame = true; inFrame++;
        }
        // Deutliche Groessenaenderung 3s stabil -> Encoder+D3D11-Pipeline in-process neu
        // aufsetzen (kein Prozess-Neustart wie im FFmpeg-Pfad noetig/moeglich). Ein frischer
        // Encoder liefert ab Frame 0 sofort einen IDR - direkt nutzbar fuer alle Zuschauer.
        if (resizePending && std::chrono::steady_clock::now() - resizePendingAt >= std::chrono::milliseconds(3000)) {
            resizePending = false;
            int newOutW, newOutH; computeOutSize(W, H, newOutW, newOutH);
            printf("SOURCE: Fenster deutlich veraendert (%dx%d) -> Encoder wird auf %dx%d neu aufgesetzt\n", W, H, newOutW, newOutH);
            // Aktuelle Encoder-Menge merken, alle abbauen, in neuer Groesse identisch neu aufsetzen.
            bool hadH264 = (encoder != nullptr), hadAv1 = (encoderAv1 != nullptr);
            if (encoder) { delete encoder; encoder = nullptr; g_h264 = false; }
            if (encoderAv1) { delete encoderAv1; encoderAv1 = nullptr; g_av1 = false; }
            outW = newOutW; outH = newOutH; srcBaseW = W; srcBaseH = H;
            nv12 = nullptr;
            D3D11_TEXTURE2D_DESC nd2{}; nd2.Width = outW; nd2.Height = outH; nd2.MipLevels = 1; nd2.ArraySize = 1; nd2.Format = DXGI_FORMAT_NV12; nd2.SampleDesc.Count = 1; nd2.Usage = D3D11_USAGE_DEFAULT; nd2.BindFlags = D3D11_BIND_RENDER_TARGET;
            dev->CreateTexture2D(&nd2, nullptr, nv12.put());
            if (hdr) { tonemap.init(dev.get(), W, H, outW, outH); tonemap.readControl(ctx.get(), true); }
            rebuildScaler();
            reconcile(hadH264, hadAv1, curKbit);   // gleiche Codec-Menge in neuer Aufloesung
        }
        if (!haveFrame) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); tries++; continue; }   // auf den ERSTEN Frame warten (max ~6s)
        // 2) faellige Takt-Ticks encodieren (letzter Frame zaehlt - echt oder wiederholt).
        uint64_t due = (uint64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * fps);
        if (frameIdx + fps < due) frameIdx = due;   // nach Pause/Stau nicht in einem Burst aufholen
        if (frameIdx >= due) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        frameIdx++;
        ID3D11Texture2D* vpIn;
        if (hdr) { tonemap.render(ctx.get()); vpIn = tonemap.sdrTex.get(); }
        else vpIn = bgraIn.get();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{}; ivd.FourCC = 0; ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; ivd.Texture2D.MipSlice = 0;
        winrt::com_ptr<ID3D11VideoProcessorInputView> iv; if (FAILED(vdev->CreateVideoProcessorInputView(vpIn, venum.get(), &ivd, iv.put()))) continue;
        D3D11_VIDEO_PROCESSOR_STREAM st{}; st.Enable = TRUE; st.pInputSurface = iv.get();
        if (FAILED(vctx->VideoProcessorBlt(vproc.get(), outView.get(), 0, 1, &st))) continue;
        ctx->Flush();
        std::vector<std::vector<uint8_t>> aus; if (encoder) encoder->encode(nv12.get(), aus);
        std::vector<std::vector<uint8_t>> ausAv1; if (encoderAv1) encoderAv1->encode(nv12.get(), ausAv1);
        frameTick++;
        // --- 1x/s Steuerdateien pollen (Bitrate, Quelle, Codec) - auf TICK-Ebene, nicht am
        //     H.264-AU-Zaehler (bei AV1-only kaeme sonst nie ein Tick durch). ---
        if (frameTick % fps == 0) {
            WIN32_FILE_ATTRIBUTE_DATA fa{}; uint64_t mt = 0;   // Bitrate
            if (GetFileAttributesExA(brPath.c_str(), GetFileExInfoStandard, &fa)) mt = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
            if (mt != brMtime) { brMtime = mt; FILE* bf = nullptr; fopen_s(&bf, brPath.c_str(), "r"); if (bf) { int k = 0; if (fscanf_s(bf, "%d", &k) == 1 && k >= 500 && k <= 200000 && k != curKbit) { curKbit = k; if (encoder) encoder->setBitrate(k); if (encoderAv1) encoderAv1->setBitrate(av1Kbit(k)); printf("  Bitrate live -> %d kbit\n", k); } fclose(bf); } }
            // Codec-Steuerdatei: gewuenschte Encoder-Menge (Shell schreibt "h264"/"av1"/"h264+av1")
            WIN32_FILE_ATTRIBUTE_DATA cfa{}; uint64_t cmt = 0;
            if (GetFileAttributesExA(codecPath.c_str(), GetFileExInfoStandard, &cfa)) cmt = ((uint64_t)cfa.ftLastWriteTime.dwHighDateTime << 32) | cfa.ftLastWriteTime.dwLowDateTime;
            if (cmt != codecMtime) { codecMtime = cmt; FILE* cf = nullptr; fopen_s(&cf, codecPath.c_str(), "r"); if (cf) { char buf[32] = { 0 }; fread(buf, 1, 31, cf); fclose(cf); std::string cs = buf; reconcile(cs.find("h264") != std::string::npos, cs.find("av1") != std::string::npos, curKbit); } }
            // Quellwechsel-Steuerdatei ("monitor" | "hwnd <id>")
            WIN32_FILE_ATTRIBUTE_DATA sfa{}; uint64_t smt = 0;
            if (GetFileAttributesExA(srcPath.c_str(), GetFileExInfoStandard, &sfa)) smt = ((uint64_t)sfa.ftLastWriteTime.dwHighDateTime << 32) | sfa.ftLastWriteTime.dwLowDateTime;
            if (smt != srcMtime) { srcMtime = smt;
                FILE* sf = nullptr; fopen_s(&sf, srcPath.c_str(), "r");
                if (sf) { char what[16] = { 0 }; unsigned long long id = 0; int n = fscanf_s(sf, "%15s %llu", what, (unsigned)_countof(what), &id); fclose(sf);
                    if (n >= 1 && _stricmp(what, "monitor") == 0) { int newIdx = (n >= 2) ? (int)id : 0; if (curHwnd != nullptr || newIdx != monIdx) { monIdx = newIdx; printf("SOURCE: Wechsel -> Bildschirm %d\n", monIdx); switchSource(nullptr); } }
                    else { HWND want = (n >= 2 && _stricmp(what, "hwnd") == 0) ? (HWND)(uintptr_t)id : nullptr; if (n >= 1 && want != curHwnd) { printf("SOURCE: Wechsel angefordert\n"); switchSource(want); } } }
            }
            if (curHwnd && !IsWindow(curHwnd)) { printf("SOURCE: Fenster geschlossen -> Monitor\n"); switchSource(nullptr); }
            printf("  %llu Frames live (%.1fs, %s%s%s)%s\n", (unsigned long long)frameTick, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), g_h264 ? "H264" : "", (g_h264 && g_av1) ? "+" : "", g_av1 ? "AV1" : "", g_withAudio ? " +Ton" : "");
        }
        // --- PSI (PAT/PMT) periodisch, codec-unabhaengig als eigene Pakete ---
        if (frameTick % (fps / 2 ? fps / 2 : 30) == 0) {
            std::vector<uint8_t> psi; buildPAT(psi); buildPMT(psi);
            sendto(s, (const char*)psi.data(), (int)psi.size(), 0, (sockaddr*)&dst, sizeof(dst));
            if (tsf) fwrite(psi.data(), 1, psi.size(), tsf);
        }
        // Bild-PTS aus der gemeinsamen Wall-Clock (nicht dem Frame-Zaehler): haelt Bild
        // und Ton synchron, auch wenn WGC mal einen Frame auslaesst. EINMAL pro Tick lesen:
        // AMF kann pro SubmitInput 0..N AUs liefern (Pipeline-Puffer, real auf AMD beobachtet
        // - 1 Tick 0 AUs, naechster Tick 2 AUs). Wuerde man now() je AU neu lesen, bekaemen
        // beide AUs praktisch denselben PTS (die Schleife braucht ~0ms) -> nicht-monotone
        // PTS, die den WHEP-Decoder nach ein paar Sekunden auf schwarz kippen lassen.
        // Darum: EIN Tick-PTS, AUs im Batch rueckwaerts im Frame-Abstand verteilt, dazu
        // Clamp gegen den zuletzt gesendeten PTS fuer garantierte Monotonie ueber Ticks hinweg.
        static uint64_t lastVideoPts = 0; const uint64_t frameDur = 90000 / (fps > 0 ? fps : 60);
        uint64_t tickPts = basePts + (uint64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 90000);
        uint64_t startPts = (aus.size() > 1) ? tickPts - (uint64_t)(aus.size() - 1) * frameDur : tickPts;
        for (size_t auIdx = 0; auIdx < aus.size(); ++auIdx) {
            auto& au = aus[auIdx];
            uint64_t pts = startPts + auIdx * frameDur;
            if (pts <= lastVideoPts) pts = lastVideoPts + frameDur;
            lastVideoPts = pts;
            std::vector<uint8_t> ts;
            std::vector<uint8_t> pes; pes.push_back(0); pes.push_back(0); pes.push_back(1); pes.push_back(0xE0); pes.push_back(0); pes.push_back(0); pes.push_back(0x80); pes.push_back(0x80); pes.push_back(5); writePTS(pes, 0x2, pts);
            pes.insert(pes.end(), AUD, AUD + 6); pes.insert(pes.end(), au.begin(), au.end());
            writeTS(ts, PID_VIDEO, true, g_ccVideo, pes.data(), (int)pes.size(), true, pts * 300);   // H.264 aktiv = PCR-Traeger
            for (size_t o = 0; o < ts.size(); o += 1316) { int len = (int)((ts.size() - o) < 1316 ? (ts.size() - o) : 1316); sendto(s, (const char*)ts.data() + o, len, 0, (sockaddr*)&dst, sizeof(dst)); }
            if (tsf) fwrite(ts.data(), 1, ts.size(), tsf);
            outFrame++;
        }
        // AV1-AUs (Temporal Units) auf eigenem PID einmuxen - ohne AUD (gibt es bei AV1 nicht).
        // PCR nur, wenn KEIN H.264 laeuft (dann ist AV1 der primaere Takttraeger).
        static uint64_t lastAv1Pts = 0;
        bool av1CarriesPcr = !g_h264;
        uint64_t startPtsAv1 = (ausAv1.size() > 1) ? tickPts - (uint64_t)(ausAv1.size() - 1) * frameDur : tickPts;
        for (size_t ai = 0; ai < ausAv1.size(); ++ai) {
            auto& au = ausAv1[ai];
            uint64_t pts = startPtsAv1 + ai * frameDur;
            if (pts <= lastAv1Pts) pts = lastAv1Pts + frameDur;
            lastAv1Pts = pts;
            std::vector<uint8_t> ts;
            std::vector<uint8_t> pes; pes.push_back(0); pes.push_back(0); pes.push_back(1); pes.push_back(0xE0); pes.push_back(0); pes.push_back(0); pes.push_back(0x80); pes.push_back(0x80); pes.push_back(5); writePTS(pes, 0x2, pts);
            pes.insert(pes.end(), au.begin(), au.end());
            writeTS(ts, PID_VIDEO_AV1, true, g_ccVideoAv1, pes.data(), (int)pes.size(), av1CarriesPcr, pts * 300);
            for (size_t o = 0; o < ts.size(); o += 1316) { int len = (int)((ts.size() - o) < 1316 ? (ts.size() - o) : 1316); sendto(s, (const char*)ts.data() + o, len, 0, (sockaddr*)&dst, sizeof(dst)); }
            if (tsf) fwrite(ts.data(), 1, ts.size(), tsf);
        }
        // Fertige Ton-Frames (Opus) einmuxen (PID 0x101, PES stream_id 0xBD = private_stream_1).
        if (g_withAudio && !g_audioNoBytes) { AudioFrame af;
            while (audio->pop(af)) { g_audioMuxed++;
                std::vector<uint8_t> ts; int plen = 8 + (int)af.data.size(); std::vector<uint8_t> pes;
                pes.push_back(0); pes.push_back(0); pes.push_back(1); pes.push_back(0xBD); pes.push_back((plen >> 8) & 0xFF); pes.push_back(plen & 0xFF);
                pes.push_back(0x80); pes.push_back(0x80); pes.push_back(5); writePTS(pes, 0x2, af.pts);
                pes.insert(pes.end(), af.data.begin(), af.data.end());
                writeTS(ts, PID_AUDIO, true, g_ccAudio, pes.data(), (int)pes.size(), false, 0);
                for (size_t o = 0; o < ts.size(); o += 1316) { int len = (int)((ts.size() - o) < 1316 ? (ts.size() - o) : 1316); sendto(s, (const char*)ts.data() + o, len, 0, (sockaddr*)&dst, sizeof(dst)); }
                if (tsf) fwrite(ts.data(), 1, ts.size(), tsf);
            }
        }
    }
    if (g_withAudio) { audio->join(); printf("AUDIO-MUX: %llu Ton-Frames eingemuxt\n", (unsigned long long)g_audioMuxed); }
    if (tsf) fclose(tsf);
    printf("Ende: %llu in / %llu out Frames (%s).\n", (unsigned long long)inFrame, (unsigned long long)outFrame, encoder ? encoder->name() : (encoderAv1 ? encoderAv1->name() : "-"));
    session.Close(); framePool.Close(); closesocket(s); WSACleanup(); return 0;
}
