#include "clipper.h"

std::wstring lastClip;
std::atomic<bool> saving;
std::thread saveThread;

// ---------- saving ----------
// SPS+PPS from an Annex-B keyframe, needed by the MP4 muxer in passthrough mode
std::vector<BYTE> seqHeader(const std::vector<BYTE>& d) {
    std::vector<BYTE> out;
    size_t n = d.size();
    auto next = [&](size_t from) {
        for (size_t k = from; k + 3 <= n; k++)
            if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1) return k;
        return n;
    };
    for (size_t s = next(0); s < n;) {
        size_t b = s + 3, e = next(b), end = e;
        while (end > b && d[end - 1] == 0) end--;  // strip leading zero of a following 4-byte start code
        int type = b < n ? d[b] & 0x1F : 0;
        if (type == 7 || type == 8) {
            out.insert(out.end(), {0, 0, 0, 1});
            out.insert(out.end(), d.begin() + b, d.begin() + end);
        }
        s = e;
    }
    return out;
}

static bool writeMp4(const std::wstring& path, const std::vector<Packet>& v, size_t v0, const std::vector<Packet>& a,
                     IMFMediaType* vt, IMFMediaType* at) {
    ComPtr<IMFMediaType> vtype;
    CHECK(MFCreateMediaType(&vtype));
    CHECK(vt->CopyAllItems(vtype.Get()));
    std::vector<BYTE> sh = seqHeader(v[v0].data);
    if (!sh.empty()) vtype->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sh.data(), (UINT32)sh.size());
    ComPtr<IMFSinkWriter> w;
    CHECK(MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &w));
    DWORD vs, as = 0;
    CHECK(w->AddStream(vtype.Get(), &vs));
    CHECK(w->SetInputMediaType(vs, vtype.Get(), nullptr));
    if (at) {
        CHECK(w->AddStream(at, &as));
        CHECK(w->SetInputMediaType(as, at, nullptr));
    }
    CHECK(w->BeginWriting());
    LONGLONG t0 = v[v0].t;
    auto put = [&](DWORD stream, const Packet& p) -> bool {
        ComPtr<IMFMediaBuffer> b;
        ComPtr<IMFSample> s;
        BYTE* d;
        CHECK(MFCreateMemoryBuffer((DWORD)p.data.size(), &b));
        b->Lock(&d, nullptr, nullptr);
        memcpy(d, p.data.data(), p.data.size());
        b->Unlock();
        b->SetCurrentLength((DWORD)p.data.size());
        MFCreateSample(&s);
        s->AddBuffer(b.Get());
        s->SetSampleTime(p.t - t0);
        s->SetSampleDuration(p.dur);
        if (p.key) s->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        CHECK(w->WriteSample(stream, s.Get()));
        return true;
    };
    size_t i = v0, j = 0;
    while (j < a.size() && a[j].t < t0) j++;
    while (i < v.size() || (at && j < a.size())) {
        bool video = !at || j >= a.size() || (i < v.size() && v[i].t <= a[j].t);
        if (!(video ? put(vs, v[i++]) : put(as, a[j++]))) return false;
    }
    CHECK(w->Finalize());
    return true;
}

// ---------- smart compression ----------
// The buffer is recorded at 2x the cap's bitrate. On save the clip is re-encoded at constant quality: every frame
// looks equally good, so fights and high-fps stretches take the bits while calm or low-fps stretches cost little.
// The quality is searched until the file lands just under the cap. Resolution is picked from the average bitrate:
// fewer clean pixels beat many blocky ones.
static ULONGLONG fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

// The buffer is variable frame rate; the saved clip is constant `fps` (the reader's video processor repeats frames).
// At constant quality a repeat has nothing left to refine and codes as a near-free skip, so low-fps stretches still
// cost almost nothing while the file stays a plain CFR MP4 every player and editor handles.
// quality 1-100 = constant quality; 0 = VBR at `bitrate` (fallback for encoders without a quality mode).
static bool transcode(const std::wstring& src, const std::wstring& dst, UINT w, UINT h, UINT fps, UINT32 bitrate,
                      UINT32 quality, IMFDXGIDeviceManager* mgr) {
    ComPtr<IMFAttributes> ra, wa, encAttrs;
    MFCreateAttributes(&ra, 3);
    ra->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);  // GPU decode + resize
    ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    ComPtr<IMFSourceReader> r;
    CHECK(MFCreateSourceReaderFromURL(src.c_str(), ra.Get(), &r));
    ComPtr<IMFMediaType> native, raw, cur, audio, out;
    CHECK(r->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native));
    const UINT32 fn = fps, fd = 1;
    MFCreateMediaType(&raw);
    raw->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    raw->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(raw.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(raw.Get(), MF_MT_FRAME_RATE, fn, fd);
    CHECK(r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, raw.Get()));
    CHECK(r->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur));
    bool withAudio = SUCCEEDED(r->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &audio)) &&
                     SUCCEEDED(r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, audio.Get()));
    DWORD videoIdx = 0;  // real stream index, to route samples from ReadSample(ANY_STREAM)
    for (DWORD i = 0;; i++) {
        ComPtr<IMFMediaType> t;
        GUID major;
        if (FAILED(r->GetCurrentMediaType(i, &t))) break;
        if (SUCCEEDED(t->GetMajorType(&major)) && major == MFMediaType_Video) videoIdx = i;
    }

    MFCreateAttributes(&wa, 2);
    wa->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, mgr);
    wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    ComPtr<IMFSinkWriter> wr;
    CHECK(MFCreateSinkWriterFromURL(dst.c_str(), nullptr, wa.Get(), &wr));
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fn, fd);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    DWORD vs, as = 0;
    CHECK(wr->AddStream(out.Get(), &vs));
    MFCreateAttributes(&encAttrs, 5);
    if (quality) {
        encAttrs->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality);
        encAttrs->SetUINT32(CODECAPI_AVEncCommonQuality, quality);
    } else {
        encAttrs->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
        encAttrs->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate);
    }
    encAttrs->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 2);
    // sparse keyframes (CamStudio-style): full frames are the expensive ones; a short clip barely needs seek points
    encAttrs->SetUINT32(CODECAPI_AVEncMPVGOPSize, 10 * fn / std::max(fd, 1u));
    encAttrs->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, 100);
    CHECK(wr->SetInputMediaType(vs, cur.Get(), encAttrs.Get()));
    if (withAudio) {  // audio is copied as-is
        CHECK(wr->AddStream(audio.Get(), &as));
        CHECK(wr->SetInputMediaType(as, audio.Get(), nullptr));
    }
    CHECK(wr->BeginWriting());
    for (int ended = 0; ended < (withAudio ? 2 : 1);) {
        DWORD idx, flags;
        LONGLONG t;
        ComPtr<IMFSample> smp;
        CHECK(r->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &idx, &flags, &t, &smp));
        if (flags & MF_SOURCE_READERF_ERROR) return false;
        if (smp) CHECK(wr->WriteSample(idx == videoIdx ? vs : as, smp.Get()));
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) ended++;
    }
    CHECK(wr->Finalize());
    return true;
}

// Work back from the cap: aim for 98.5% of it and rescale the bitrate by each miss (size ~ bitrate), up or down.
// Keeps the largest encode that fits; stops once within 3% of the cap. Returns the size written to dst, 0 if none fit.
static ULONGLONG fitBitrate(const std::wstring& src, const std::wstring& tmp, const std::wstring& dst, UINT w, UINT h, UINT fps,
                           ULONGLONG limit, UINT32 bitrate, IMFDXGIDeviceManager* mgr) {
    ULONGLONG best = 0;
    for (int pass = 0; pass < 5 && best < limit * 0.97; pass++) {
        if (!transcode(src, tmp, w, h, fps, bitrate, 0, mgr)) break;
        ULONGLONG size = fileSize(tmp);
        logf(L"  pass %d: %ux%u %u kbps -> %.2f MB", pass + 1, w, h, bitrate / 1000, size / 1e6);
        if (!size) break;
        if (size <= limit && size > best && MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) best = size;
        bitrate = (UINT32)std::clamp(bitrate * (limit * 0.985 / size), 1e5, 250e6);
    }
    return best;
}

// Constant-quality search: size grows with quality, so bracket the cap and interpolate on log(size). Starts from the
// last save's quality (clips from the same game land close). Keeps the largest encode that fits; stops within 3% of
// the cap, at quality 100, or when the bracket closes. Falls back to the bitrate walk if quality mode isn't available.
static ULONGLONG fitToSize(const std::wstring& src, const std::wstring& tmp, const std::wstring& dst, UINT w, UINT h, UINT fps,
                           ULONGLONG limit, UINT32 bitrate, IMFDXGIDeviceManager* mgr) {
    static int lastQ = 70;
    int lo = 0, hi = 101, q = lastQ;  // lo fits (or 0 = none yet), hi is too big (101 = none yet)
    double loSize = 0, hiSize = 0;
    ULONGLONG best = 0;
    for (int pass = 0; pass < 6; pass++) {
        if (!transcode(src, tmp, w, h, fps, 0, q, mgr)) break;
        ULONGLONG size = fileSize(tmp);
        logf(L"  pass %d: %ux%u quality %d -> %.2f MB", pass + 1, w, h, q, size / 1e6);
        if (!size) break;
        if (size <= limit) {
            if (size > best && MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) best = size, lastQ = q;
            lo = q, loSize = (double)size;
        } else {
            hi = q, hiSize = (double)size;
        }
        if (best >= limit * 0.97 || lo == 100 || hi - lo <= 1) break;
        if (lo && hi <= 100) {
            double f = (std::log(limit * 0.985) - std::log(loSize)) / (std::log(hiSize) - std::log(loSize));
            q = std::clamp(lo + (int)std::lround((hi - lo) * f), lo + 1, hi - 1);
        } else {
            q = lo ? std::min(100, lo + 15) : std::max(1, hi - 25);
        }
    }
    if (best) return best;
    logf(L"  quality search found no fit, falling back to bitrate");
    return fitBitrate(src, tmp, dst, w, h, fps, limit, bitrate, mgr);
}

static void compressAndSave(std::vector<Packet> v, size_t s, std::vector<Packet> a, ComPtr<IMFMediaType> vt,
                            ComPtr<IMFMediaType> at, std::wstring path, double maxmb, bool keepIfFits) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    std::wstring src = std::wstring(tmpDir) + L"clipper_src.mp4", tmp = std::wstring(tmpDir) + L"clipper_try.mp4";
    ULONGLONG limit = (ULONGLONG)(maxmb * 1e6), size = 0;
    UINT bufW = 0, bufH = 0, outH = 0;
    LONGLONG t0 = GetTickCount64();
    if (writeMp4(src, v, s, a, vt.Get(), at.Get())) {
        MFGetAttributeSize(vt.Get(), MF_MT_FRAME_SIZE, &bufW, &bufH);
        double secs = (v.back().t + v.back().dur - v[s].t) / 1e7;
        size = fileSize(src);
        logf(L"saving %.1fs clip (%.1f fps avg), buffer %ux%u %.1f MB, %d frames dropped since the last save", secs,
             (v.size() - s) / secs, bufW, bufH, size / 1e6, droppedFrames.exchange(0));
        bool fits = size <= limit;
        size = 0;
        if (keepIfFits && fits) {  // fixed bitrate and under the cap: the buffer is the clip
            outH = bufH;
            if (MoveFileExW(src.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) size = fileSize(path);
        }
        // auto bitrate always re-encodes to fill the cap; a calm clip that already fits keeps the full buffer resolution
        UINT32 budget = videoBitrate(secs, maxmb, at ? MFGetAttributeUINT32(at.Get(), MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000) * 8 : 0);
        outH = size ? outH : cfg.height || fits ? bufH : pickHeight(budget, bufW, bufH);
        ComPtr<ID3D11Device> dev;
        ComPtr<IMFDXGIDeviceManager> mgr;
        UINT token;
        if (!size && SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                        nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr)) &&
            SUCCEEDED(MFCreateDXGIDeviceManager(&token, &mgr)) && SUCCEEDED(mgr->ResetDevice(dev.Get(), token))) {
            ComPtr<ID3D11Multithread> mt;
            if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
            UINT outW = (UINT)((UINT64)outH * bufW / bufH + 1) & ~1u;
            UINT32 fn = 60, fd = 1;  // capture rate, from the buffer encoder's type
            MFGetAttributeRatio(vt.Get(), MF_MT_FRAME_RATE, &fn, &fd);
            size = fitToSize(src, tmp, path, outW, outH, fn / std::max(fd, 1u), limit, budget, mgr.Get());
        }
        // re-encode failed but the buffer itself fits: better an under-cap clip than none
        if (!size && fits && MoveFileExW(src.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
            size = fileSize(path);
    }
    DeleteFileW(src.c_str());
    DeleteFileW(tmp.c_str());
    if (size) {
        lastClip = path;
        wchar_t info[64];
        swprintf(info, 64, L" (%.1f MB, %up, %.0fs to compress)", size / 1e6, outH, (GetTickCount64() - t0) / 1000.0);
        logf(L"saved %ls%ls", path.c_str(), info);
        notify(L"Clip saved", path.substr(path.rfind(L'\\') + 1) + info + L"\nClick to show it.", NIIF_INFO);
    } else {
        fail(L"Couldn't save the clip", L"Details in clipper.log.");
    }
    saving = false;
    PostMessageW(ui, STATUS_MSG, 0, 0);
    CoUninitialize();
}

void saveClip() {
    if (!running) {
        fail(L"Not recording", L"Waiting for " + cfg.game + L". Start the game, or pick a different one in Clipper's settings.");
        return;
    }
    if (saving) { fail(L"Still saving the last clip", L"Try again in a few seconds."); return; }
    std::vector<Packet> v, a;
    ComPtr<IMFMediaType> vt, at;
    {
        std::lock_guard<std::mutex> l(bufMx);
        v.assign(vBuf.begin(), vBuf.end());
        a.assign(aBuf.begin(), aBuf.end());
        vt = vType;
        at = aType;
    }
    if (v.empty() || !vt) { fail(L"Nothing recorded yet", L"No video captured. Details in clipper.log."); return; }
    while (!a.empty() && a.back().t > v.back().t) a.pop_back();
    if (a.empty()) at.Reset();

    // start at the first keyframe inside the window, so the clip is never longer than `seconds`
    LONGLONG from = v.back().t - cfg.seconds * 10000000LL;
    size_t s = 0;
    while (s < v.size() && !(v[s].key && v[s].t >= from)) s++;
    if (s == v.size()) { fail(L"Nothing recorded yet", L"Try again in a second."); return; }

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring name = cfg.game.empty() ? L"clip" : cfg.game.substr(0, cfg.game.rfind(L'.'));
    wchar_t file[64];
    swprintf(file, 64, L"_%04d-%02d-%02d_%02d-%02d-%02d.mp4", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    MessageBeep(MB_OK);  // "got it" — the moment is captured; compression continues in the background
    saving = true;
    if (saveThread.joinable()) saveThread.join();
    saveThread = std::thread(compressAndSave, std::move(v), s, std::move(a), vt, at, cfg.folder + L"\\" + name + file, cfg.maxmb,
                             cfg.bitrate != 0);
    PostMessageW(ui, STATUS_MSG, 0, 0);
}

