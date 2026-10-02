#include "clipper.h"

// ---------- audio ----------
// Each source writes into a ring at the sample position given by its capture timestamp; the mixer sums
// the rings ~100ms behind real time. Gaps (loopback sends nothing during silence) become zeros, and
// all sources stay in sync with the video clock.
static constexpr int SR = 48000, RING = SR * 4;
struct AudioSrc {
    std::vector<int16_t> ring = std::vector<int16_t>(RING * 2);
    LONGLONG wpos = LLONG_MIN;
    float gain = 1;
};
static AudioSrc aSrc[4];  // game, desktop, mic, discord
static LONGLONG mixPos;
static std::mutex aMx;

static void audioWrite(int si, const int16_t* d, UINT32 n, LONGLONG t, bool silent) {
    LONGLONG pos = t * SR / 10000000;
    std::lock_guard<std::mutex> l(aMx);
    AudioSrc& s = aSrc[si];
    if (s.wpos == LLONG_MIN || std::llabs(pos - s.wpos) > SR / 50) s.wpos = pos;  // resync on >20ms drift
    for (UINT32 i = 0; i < n; i++, s.wpos++) {
        if (s.wpos < mixPos) continue;  // arrived too late / before start
        int16_t* o = &s.ring[(s.wpos % RING) * 2];
        for (int c = 0; c < 2; c++) o[c] = silent ? 0 : (int16_t)std::clamp(d[i * 2 + c] * s.gain, -32768.f, 32767.f);
    }
}

struct ActivateHandler : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                      Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ActivateHandler() { CloseHandle(done); }
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation*) override { SetEvent(done); return S_OK; }
};

bool captureLoop(int si, DWORD pid) {
    WAVEFORMATEX wf{WAVE_FORMAT_PCM, 2, SR, SR * 4, 4, 16, 0};
    ComPtr<IAudioClient> ac;
    if (si == 0 || si == 3) {  // game / discord only: per-process loopback (Win10 2004+)
        AUDIOCLIENT_ACTIVATION_PARAMS p{};
        p.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        p.ProcessLoopbackParams.TargetProcessId = pid;
        p.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(p);
        pv.blob.pBlobData = (BYTE*)&p;
        auto h = Microsoft::WRL::Make<ActivateHandler>();
        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        CHECK(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, h.Get(), &op));
        WaitForSingleObject(h->done, 5000);
        HRESULT hr = E_FAIL;
        ComPtr<IUnknown> u;
        op->GetActivateResult(&hr, &u);
        CHECK(hr);
        CHECK(u.As(&ac));
        CHECK(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, &wf, nullptr));
    } else {
        ComPtr<IMMDeviceEnumerator> de;
        ComPtr<IMMDevice> dev;
        CHECK(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&de)));
        CHECK(de->GetDefaultAudioEndpoint(si == 1 ? eRender : eCapture, eConsole, &dev));
        CHECK(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &ac));
        DWORD fl = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                   (si == 1 ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        CHECK(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, fl, 2000000, 0, &wf, nullptr));
    }
    ComPtr<IAudioCaptureClient> cc;
    CHECK(ac->GetService(IID_PPV_ARGS(&cc)));
    CHECK(ac->Start());
    // ponytail: default device picked at start; a device switch mid-session needs Reload settings
    while (running) {
        Sleep(10);
        UINT32 n;
        while (SUCCEEDED(cc->GetNextPacketSize(&n)) && n) {
            BYTE* d;
            UINT32 frames;
            DWORD flags;
            UINT64 qpc = 0;
            if (FAILED(cc->GetBuffer(&d, &frames, &flags, nullptr, &qpc))) break;
            LONGLONG t = qpc ? (LONGLONG)qpc - startHns : nowHns() - frames * 10000000LL / SR;
            audioWrite(si, (const int16_t*)d, frames, t, flags & AUDCLNT_BUFFERFLAGS_SILENT);
            cc->ReleaseBuffer(frames);
        }
    }
    ac->Stop();
    return true;
}

bool audioLoop() {
    ComPtr<IMFTransform> enc;
    CHECK(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enc)));
    const UINT32 ch = cfg.mono ? 1 : 2;
    auto audioType = [&](const GUID& sub) {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, sub);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, SR);
        t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        return t;
    };
    ComPtr<IMFMediaType> in = audioType(MFAudioFormat_PCM), out = audioType(MFAudioFormat_AAC);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * ch);
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, SR * 2 * ch);
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, cfg.audiokbps * 125);
    CHECK(enc->SetInputType(0, in.Get(), 0));
    CHECK(enc->SetOutputType(0, out.Get(), 0));
    {
        ComPtr<IMFMediaType> t;
        CHECK(enc->GetOutputCurrentType(0, &t));
        std::lock_guard<std::mutex> l(bufMx);
        aType = t;
    }
    MFT_OUTPUT_STREAM_INFO si{};
    CHECK(enc->GetOutputStreamInfo(0, &si));
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    while (running) {
        Sleep(20);
        LONGLONG target = nowHns() * SR / 10000000 - SR / 10;
        LONGLONG from;
        UINT32 n;
        ComPtr<IMFMediaBuffer> b;
        {
            std::lock_guard<std::mutex> l(aMx);
            if (target <= mixPos) continue;
            from = mixPos;
            n = (UINT32)(target - mixPos);
            CHECK(MFCreateMemoryBuffer(n * 2 * ch, &b));
            BYTE* raw;
            b->Lock(&raw, nullptr, nullptr);
            int16_t* o = (int16_t*)raw;
            for (UINT32 i = 0; i < n * ch; i++) {
                int sum = 0;
                for (UINT32 c = 0; c < 3 - ch; c++) {  // mono: average both ring channels into one
                    size_t idx = (size_t)((from + i / ch) % RING) * 2 + (ch == 2 ? i % 2 : c);
                    for (auto& s : aSrc) { sum += s.ring[idx]; s.ring[idx] = 0; }
                }
                o[i] = (int16_t)std::clamp(sum / (int)(3 - ch), -32768, 32767);
            }
            b->Unlock();
            b->SetCurrentLength(n * 2 * ch);
            mixPos = target;
        }
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(b.Get());
        s->SetSampleTime(from * 10000000 / SR);
        s->SetSampleDuration(n * 10000000LL / SR);
        if (FAILED(enc->ProcessInput(0, s.Get(), 0))) continue;
        for (;;) {
            ComPtr<IMFSample> os;
            ComPtr<IMFMediaBuffer> obuf;
            MFCreateSample(&os);
            MFCreateMemoryBuffer(std::max<DWORD>(si.cbSize, 8192), &obuf);
            os->AddBuffer(obuf.Get());
            MFT_OUTPUT_DATA_BUFFER ob{0, os.Get()};
            DWORD st;
            HRESULT hr = enc->ProcessOutput(0, 1, &ob, &st);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr != S_OK) break;
            storeSample(os.Get(), aBuf);
        }
    }
    return true;
}

void setGains() {
    std::lock_guard<std::mutex> l(aMx);
    aSrc[0].gain = aSrc[1].gain = (float)cfg.gamevol;
    aSrc[2].gain = (float)cfg.micvol;
    aSrc[3].gain = (float)cfg.discordvol;
}

void resetAudio() {
    {
        std::lock_guard<std::mutex> l(aMx);
        mixPos = 0;
        for (auto& s : aSrc) { std::fill(s.ring.begin(), s.ring.end(), (int16_t)0); s.wpos = LLONG_MIN; }
    }
    setGains();
}
