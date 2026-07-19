// Audio-PoC: FFmpeg-Ersatz fuer den Ton.
//  Teil A (immer): PCM f32/48k/stereo (Sinus 440 Hz) -> s16 -> Media-Foundation-
//                  AAC-Encoder (MFT) -> ADTS-Frames -> poc_audio.aac. Beweist das
//                  AAC-Encoding OHNE FFmpeg (bislang machte das FFmpeg pipe:3->AAC).
//  Teil B (best effort): WASAPI-System-Loopback 3 s oeffnen und zeigen, dass echte
//                  Ton-Frames im f32/48k/stereo-Format ankommen (der reale Capture-Weg).
// Validierung: ffmpeg -i poc_audio.aac  ->  "Audio: aac (LC), 48000 Hz, stereo".
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <winrt/base.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstdint>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#define HROK(call, msg) do { HRESULT _h=(call); if(FAILED(_h)){ printf("FEHLER %s 0x%08lX\n", msg, _h); return 1; } } while(0)

// ADTS-Header (7 Byte, ohne CRC) vor jeden AAC-LC-Frame. 48 kHz -> freqIdx 3, 2 ch.
static void adtsHeader(uint8_t* h, int payloadLen, int freqIdx, int chan) {
    int frameLen = 7 + payloadLen;
    h[0] = 0xFF;
    h[1] = 0xF1;                                  // MPEG-4, Layer 0, kein CRC
    h[2] = (uint8_t)((1 << 6) | (freqIdx << 2) | ((chan >> 2) & 1)); // profile=AAC-LC(objType2 -> 1)
    h[3] = (uint8_t)(((chan & 3) << 6) | ((frameLen >> 11) & 3));
    h[4] = (uint8_t)((frameLen >> 3) & 0xFF);
    h[5] = (uint8_t)(((frameLen & 7) << 5) | 0x1F);
    h[6] = 0xFC;
}

static int freqIndexFor(int sr) {
    static const int t[] = { 96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350 };
    for (int i = 0; i < 13; ++i) if (t[i] == sr) return i;
    return 3;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    HROK(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");

    const int SR = 48000, CH = 2, BITS = 16;

    // ---------- Teil B: WASAPI-System-Loopback (best effort) ----------
    int loopFrames = 0; double loopPeak = 0.0;
    {
        winrt::com_ptr<IMMDeviceEnumerator> en;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), en.put_void()))) {
            winrt::com_ptr<IMMDevice> dev;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()))) {
                winrt::com_ptr<IAudioClient> ac;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, ac.put_void()))) {
                    WAVEFORMATEX* mix = nullptr; ac->GetMixFormat(&mix);
                    if (mix && SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                        AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, mix, nullptr))) {
                        winrt::com_ptr<IAudioCaptureClient> cap;
                        if (SUCCEEDED(ac->GetService(__uuidof(IAudioCaptureClient), cap.put_void()))) {
                            printf("WASAPI-Loopback Mix-Format: %d Hz, %d ch, %d bit\n",
                                (int)mix->nSamplesPerSec, (int)mix->nChannels, (int)mix->wBitsPerSample);
                            ac->Start();
                            for (int t = 0; t < 30; ++t) {          // ~3 s (30 x 100 ms)
                                Sleep(100);
                                UINT32 pkt = 0;
                                while (SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt > 0) {
                                    BYTE* d = nullptr; UINT32 n = 0; DWORD fl = 0;
                                    if (FAILED(cap->GetBuffer(&d, &n, &fl, nullptr, nullptr))) break;
                                    if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT) && mix->wBitsPerSample == 32) {
                                        const float* fp = (const float*)d;
                                        for (UINT32 i = 0; i < n * mix->nChannels; ++i)
                                            if (fabs(fp[i]) > loopPeak) loopPeak = fabs(fp[i]);
                                    }
                                    loopFrames += n;
                                    cap->ReleaseBuffer(n);
                                    cap->GetNextPacketSize(&pkt);
                                }
                            }
                            ac->Stop();
                        }
                    }
                    if (mix) CoTaskMemFree(mix);
                }
            }
        }
        printf("WASAPI-Loopback: %d Frames erfasst, Spitzenpegel %.3f (%s)\n",
            loopFrames, loopPeak, loopPeak > 0.0001 ? "echter Ton lief" : "still/kein Ton");
    }

    // ---------- Teil A: Sinus f32 -> s16 -> MF-AAC -> ADTS ----------
    // 2 s Sinus 440 Hz, f32 stereo -> direkt als s16 PCM (MF-AAC will 16 bit).
    const int seconds = 2, total = SR * seconds;
    std::vector<int16_t> pcm(total * CH);
    for (int i = 0; i < total; ++i) {
        double s = sin(2.0 * 3.14159265358979 * 440.0 * i / SR) * 0.3;
        int16_t v = (int16_t)(s * 32767);
        pcm[i * CH + 0] = v; pcm[i * CH + 1] = v;
    }

    // AAC-Encoder-MFT finden (MFT_CATEGORY_AUDIO_ENCODER, Output MFAudioFormat_AAC).
    MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Audio, MFAudioFormat_AAC };
    IMFActivate** acts = nullptr; UINT32 nActs = 0;
    HROK(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &outInfo, &acts, &nActs), "MFTEnumEx");
    if (nActs == 0) { printf("FEHLER: kein AAC-Encoder-MFT\n"); return 1; }
    winrt::com_ptr<IMFTransform> mft;
    HROK(acts[0]->ActivateObject(__uuidof(IMFTransform), mft.put_void()), "ActivateObject");
    for (UINT32 i = 0; i < nActs; ++i) acts[i]->Release();
    CoTaskMemFree(acts);

    // Output-Typ: AAC 48k/2/16, 128 kbps (16000 B/s), Payload 0 (raw).
    winrt::com_ptr<IMFMediaType> ot;
    HROK(MFCreateMediaType(ot.put()), "CreateMediaType(out)");
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    ot->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    ot->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, SR);
    ot->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, CH);
    ot->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, BITS);
    ot->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
    ot->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    HROK(mft->SetOutputType(0, ot.get(), 0), "SetOutputType");

    // Input-Typ: PCM 48k/2/16.
    winrt::com_ptr<IMFMediaType> it;
    HROK(MFCreateMediaType(it.put()), "CreateMediaType(in)");
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    it->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    it->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, SR);
    it->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, CH);
    it->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, BITS);
    it->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, CH * BITS / 8);
    it->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, SR * CH * BITS / 8);
    HROK(mft->SetInputType(0, it.get(), 0), "SetInputType");

    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    const int freqIdx = freqIndexFor(SR);
    FILE* f = nullptr; fopen_s(&f, "poc_audio.aac", "wb");
    size_t aacFrames = 0, aacBytes = 0;

    auto drainOutput = [&]() -> HRESULT {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO si{}; mft->GetOutputStreamInfo(0, &si);
            winrt::com_ptr<IMFMediaBuffer> buf;
            if (FAILED(MFCreateMemoryBuffer(si.cbSize ? si.cbSize : 8192, buf.put()))) return E_FAIL;
            MFT_OUTPUT_DATA_BUFFER odb{}; winrt::com_ptr<IMFSample> os;
            MFCreateSample(os.put()); os->AddBuffer(buf.get()); odb.pSample = os.get();
            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &odb, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
            if (FAILED(hr)) return hr;
            winrt::com_ptr<IMFMediaBuffer> ob; os->ConvertToContiguousBuffer(ob.put());
            BYTE* p = nullptr; DWORD cur = 0; ob->Lock(&p, nullptr, &cur);
            if (cur > 0) {
                uint8_t hdr[7]; adtsHeader(hdr, cur, freqIdx, CH);
                fwrite(hdr, 1, 7, f); fwrite(p, 1, cur, f);
                aacFrames++; aacBytes += cur + 7;
            }
            ob->Unlock();
        }
    };

    // In 1024-Sample-Bloecken einspeisen (AAC-Frame = 1024 Samples).
    const int blk = 1024;
    LONGLONG hns = 0; const LONGLONG dur = (LONGLONG)blk * 10000000 / SR;
    for (int off = 0; off + blk <= total; off += blk) {
        winrt::com_ptr<IMFMediaBuffer> buf;
        const DWORD bytes = blk * CH * BITS / 8;
        MFCreateMemoryBuffer(bytes, buf.put());
        BYTE* p = nullptr; buf->Lock(&p, nullptr, nullptr);
        memcpy(p, &pcm[off * CH], bytes); buf->Unlock(); buf->SetCurrentLength(bytes);
        winrt::com_ptr<IMFSample> smp; MFCreateSample(smp.put());
        smp->AddBuffer(buf.get()); smp->SetSampleTime(hns); smp->SetSampleDuration(dur); hns += dur;
        HRESULT hr = mft->ProcessInput(0, smp.get(), 0);
        if (hr == MF_E_NOTACCEPTING) { drainOutput(); mft->ProcessInput(0, smp.get(), 0); }
        drainOutput();
    }
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    drainOutput();
    fclose(f);

    printf("OK: %zu AAC-Frames (%zu Bytes ADTS) -> poc_audio.aac\n", aacFrames, aacBytes);
    MFShutdown();
    return (aacFrames > 0 && aacBytes > 0) ? 0 : 3;
}
