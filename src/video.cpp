#include "clipper.h"

// ---------- video ----------
// HDR -> SDR. Input is scRGB (linear, 1.0 = 80 nits). Windows' SDR white maps to 1.0 so desktop/HUD colours stay
// exact; brighter game highlights roll off smoothly (hue-preserving) instead of clipping per channel.
static const char kToneMapHlsl[] = R"(
Texture2D<float4> src : register(t0);
cbuffer C : register(b0) { float sdrWhite; float3 pad; }
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    float3 c = max(src.Load(int3(pos.xy, 0)).rgb, 0) / sdrWhite;
    float m = max(c.r, max(c.g, c.b));
    const float k = 0.8;  // knee: below this, untouched
    if (m > k) c *= (k + (1 - k) * (1 - exp(-(m - k) / (1 - k)))) / m;
    c = saturate(c);
    return float4(c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055, 1);
}
)";

// scRGB value of Windows' "SDR content brightness" white on this monitor (1.0 = 80 nits)
static float sdrWhiteScale(HMONITOR mon) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm)) return 1;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr)) return 1;
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(sn), paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&sn.header) || wcscmp(sn.viewGdiDeviceName, mi.szDevice)) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
        wl.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(wl), paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
        if (!DisplayConfigGetDeviceInfo(&wl.header)) return wl.SDRWhiteLevel / 1000.f;
    }
    return 1;
}

std::atomic<bool> resized;  // capture area changed size: the watcher restarts recording at the new size

// Game window's client area in monitor pixels, clipped to the monitor (centre 16:9 if crop is on). Leaves r alone
// while the window is minimised or gone, so alt-tab doesn't count as a resize. Caller must be per-monitor DPI aware.
// ponytail: assumes an unrotated monitor
void captureRect(HWND w, const RECT& mon, RECT& r) {
    RECT full{0, 0, mon.right - mon.left, mon.bottom - mon.top}, c, s;
    POINT o{0, 0};
    if (!w) s = full;
    else if (IsIconic(w) || !GetClientRect(w, &c) || !ClientToScreen(w, &o)) return;
    else s = {o.x - mon.left, o.y - mon.top, o.x - mon.left + c.right, o.y - mon.top + c.bottom};
    if (!IntersectRect(&s, &s, &full)) return;  // window is on another monitor: the watcher will follow it
    LONG sw = s.right - s.left, sh = s.bottom - s.top;
    if (cfg.crop && sw * 9 > sh * 16 + 16) s.left += (sw - sh * 16 / 9) / 2, s.right = s.left + sh * 16 / 9;
    r = s;
}

bool videoLoop(HMONITOR mon, HWND win) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // DuplicateOutput1 requires it
    ComPtr<IDXGIFactory1> fac;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&fac)));
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput6> output;
    for (UINT i = 0; !output && fac->EnumAdapters1(i, &adapter) == S_OK; i++) {
        ComPtr<IDXGIOutput> o;
        for (UINT j = 0; adapter->EnumOutputs(j, &o) == S_OK; j++) {
            DXGI_OUTPUT_DESC od;
            o->GetDesc(&od);
            if (od.Monitor == mon) { o.As(&output); break; }
        }
    }
    if (!output) { logf(L"monitor not found on any GPU"); return false; }
    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    CHECK(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                            D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    // A game maxing the GPU/CPU must not starve our one copy + encode per frame (that's what drops frames):
    // high GPU scheduling priority like OBS, and an MMCSS "Capture" boost for this thread.
    ComPtr<IDXGIDevice> dxdev;
    HRESULT gpuPrio = SUCCEEDED(dev.As(&dxdev)) ? dxdev->SetGPUThreadPriority(7) : E_FAIL;
    auto setGpuClass = (LONG(WINAPI*)(HANDLE, int))GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass");
    LONG gpuClass = setGpuClass ? setGpuClass(GetCurrentProcess(), 4 /* D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH */) : -1;
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &task);
    logf(L"priority: gpu thread %ls, gpu class %ls, mmcss %ls", SUCCEEDED(gpuPrio) ? L"ok" : L"failed",
         gpuClass == 0 ? L"high" : L"failed", mmcss ? L"ok" : L"failed");
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    CHECK(dev.As(&vdev));
    CHECK(ctx.As(&vctx));

    // HDR desktops are grabbed as FP16 scRGB and tone-mapped; asking for 8-bit makes Windows clip (oversaturated look)
    ComPtr<IDXGIOutputDuplication> dup;
    auto duplicate = [&] {
        DXGI_OUTPUT_DESC1 od;
        output->GetDesc1(&od);
        bool hdr = od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        DXGI_FORMAT fmt = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        HRESULT hr = output->DuplicateOutput1(dev.Get(), 0, 1, &fmt, &dup);
        if (SUCCEEDED(hr)) logf(L"duplicating %ls (HDR %ls)", od.DeviceName, hdr ? L"on" : L"off");
        return hr;
    };
    CHECK(duplicate());
    DXGI_OUTDUPL_DESC dd;
    dup->GetDesc(&dd);
    DXGI_OUTPUT_DESC mdesc;
    output->GetDesc(&mdesc);
    const RECT monRc = mdesc.DesktopCoordinates;
    RECT cap{0, 0, monRc.right - monRc.left, monRc.bottom - monRc.top};
    captureRect(win, monRc, cap);
    const UINT srcW = cap.right - cap.left, srcH = cap.bottom - cap.top;
    // auto bitrate: buffer at 2x the size cap's average bitrate so the save step re-encodes from a clean source.
    // fixed bitrate: the buffer is the clip.
    UINT32 abps = anyAudio() ? cfg.audiokbps * 1000 : 0;
    UINT32 vbr = cfg.bitrate ? cfg.bitrate * 1000 : (UINT32)std::clamp(2.0 * videoBitrate(cfg.seconds, cfg.maxmb, abps), 4e6, 60e6);
    // buffer resolution: the fixed choice; auto = 1080p (the save step picks the final size), or what a fixed bitrate suits
    UINT outH = std::min<UINT>(cfg.height ? cfg.height : cfg.bitrate ? pickHeight(vbr, srcW, srcH) : 1080, srcH) & ~1u;
    UINT outW = (UINT)((UINT64)outH * srcW / srcH + 1) & ~1u;
    const UINT fps = cfg.fps;
    logf(L"%ls: %ux%u, capturing %ux%u at (%ld,%ld) -> %ux%u @%u, buffer %u kbps", ad.Description, dd.ModeDesc.Width,
         dd.ModeDesc.Height, srcW, srcH, cap.left, cap.top, outW, outH, fps, vbr / 1000);

    ComPtr<ID3D11VertexShader> tmVs;
    ComPtr<ID3D11PixelShader> tmPs;
    ComPtr<ID3D11Buffer> tmCb;
    {
        ComPtr<ID3DBlob> vsb, psb, err;
        CHECK(D3DCompile(kToneMapHlsl, sizeof(kToneMapHlsl) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err));
        CHECK(D3DCompile(kToneMapHlsl, sizeof(kToneMapHlsl) - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err));
        CHECK(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &tmVs));
        CHECK(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &tmPs));
        float cb[4] = {sdrWhiteScale(mon), 0, 0, 0};
        logf(L"SDR white = %.0f nits", cb[0] * 80);
        D3D11_BUFFER_DESC bd{sizeof(cb), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA sd{cb, 0, 0};
        CHECK(dev->CreateBuffer(&bd, &sd, &tmCb));
    }

    // hardware H.264 encoder on the same GPU
    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12}, outInfo{MFMediaType_Video, MFVideoFormat_H264};
    ComPtr<IMFAttributes> enumAttrs;
    MFCreateAttributes(&enumAttrs, 1);
    enumAttrs->SetBlob(MFT_ENUM_ADAPTER_LUID, (BYTE*)&ad.AdapterLuid, sizeof(ad.AdapterLuid));
    IMFActivate** acts = nullptr;
    UINT32 nActs = 0;
    CHECK(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &inInfo, &outInfo,
                   enumAttrs.Get(), &acts, &nActs));
    // ponytail: hardware encoder only; add a software-MFT fallback if someone runs this without a GPU encoder
    if (!nActs) { logf(L"no hardware H.264 encoder"); return false; }
    ComPtr<IMFActivate> act = acts[0];
    for (UINT32 i = 0; i < nActs; i++) acts[i]->Release();
    CoTaskMemFree(acts);
    ComPtr<IMFTransform> enc;
    CHECK(act->ActivateObject(IID_PPV_ARGS(&enc)));
    ComPtr<IMFAttributes> ea;
    CHECK(enc->GetAttributes(&ea));
    ea->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    ea->SetUINT32(MF_LOW_LATENCY, TRUE);

    UINT token;
    ComPtr<IMFDXGIDeviceManager> mgr;
    CHECK(MFCreateDXGIDeviceManager(&token, &mgr));
    CHECK(mgr->ResetDevice(dev.Get(), token));
    CHECK(enc->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)mgr.Get()));

    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(enc.As(&api))) {
        VARIANT v;
        v.vt = VT_UI4;
        // VBR: busy moments borrow bits from calm ones (peaks up to 2x the average); CBR if the encoder won't
        v.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
        bool isVbr = SUCCEEDED(api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v));
        if (!isVbr) {
            v.ulVal = eAVEncCommonRateControlMode_CBR;
            api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        }
        v.ulVal = vbr;
        api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
        v.ulVal = (ULONG)std::min<UINT64>(2ull * vbr, 200000000);
        if (isVbr) api->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &v);
        logf(L"buffer rate control: %ls", isVbr ? L"peak-constrained VBR" : L"CBR (VBR not supported)");
        v.ulVal = fps * 2;  // backstop only: keyframes are forced every second of real time (clip start granularity)
        api->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
    }
    auto videoType = [&](const GUID& sub) {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, sub);
        MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, outW, outH);
        MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        return t;
    };
    ComPtr<IMFMediaType> ot = videoType(MFVideoFormat_H264);
    ot->SetUINT32(MF_MT_AVG_BITRATE, vbr);
    ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    CHECK(enc->SetOutputType(0, ot.Get(), 0));
    CHECK(enc->SetInputType(0, videoType(MFVideoFormat_NV12).Get(), 0));
    auto publishType = [&] {
        ComPtr<IMFMediaType> t;
        enc->GetOutputCurrentType(0, &t);
        std::lock_guard<std::mutex> l(bufMx);
        vType = t;
    };
    publishType();
    ComPtr<IMFMediaEventGenerator> gen;
    CHECK(enc.As(&gen));
    CHECK(enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    CHECK(enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

    // NV12 output pool. ponytail: fixed 8-deep ring assumes encoder holds < 8 frames; true for low-latency HW encoders
    const int POOL = 8;
    ComPtr<ID3D11Texture2D> pool[POOL];
    ComPtr<ID3D11VideoProcessorOutputView> outView[POOL];
    D3D11_TEXTURE2D_DESC pd{outW, outH, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    for (auto& p : pool) CHECK(dev->CreateTexture2D(&pd, nullptr, &p));

    ComPtr<ID3D11Texture2D> srcTex, hdrTex;  // srcTex: 8-bit sRGB desktop; hdrTex: FP16 copy when HDR
    ComPtr<ID3D11ShaderResourceView> hdrSrv;
    ComPtr<ID3D11RenderTargetView> srcRtv;
    DXGI_FORMAT frameFmt = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11VideoProcessorEnumerator> ven;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<ID3D11VideoProcessorInputView> inView;
    auto buildInput = [&](D3D11_TEXTURE2D_DESC td) -> bool {  // on first frame and whenever the game changes resolution
        frameFmt = td.Format;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = td.MiscFlags = 0;
        td.MipLevels = td.ArraySize = 1;
        hdrTex.Reset();
        if (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            CHECK(dev->CreateTexture2D(&td, nullptr, &hdrTex));
            CHECK(dev->CreateShaderResourceView(hdrTex.Get(), nullptr, &hdrSrv));
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        CHECK(dev->CreateTexture2D(&td, nullptr, &srcTex));
        CHECK(dev->CreateRenderTargetView(srcTex.Get(), nullptr, &srcRtv));
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = cd.OutputFrameRate = {fps, 1};
        cd.InputWidth = td.Width;
        cd.InputHeight = td.Height;
        cd.OutputWidth = outW;
        cd.OutputHeight = outH;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        CHECK(vdev->CreateVideoProcessorEnumerator(&cd, &ven));
        CHECK(vdev->CreateVideoProcessor(ven.Get(), 0, &vp));
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{0, D3D11_VPIV_DIMENSION_TEXTURE2D, {0, 0}};
        CHECK(vdev->CreateVideoProcessorInputView(srcTex.Get(), ven.Get(), &iv, &inView));
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{D3D11_VPOV_DIMENSION_TEXTURE2D};
        for (int i = 0; i < POOL; i++) CHECK(vdev->CreateVideoProcessorOutputView(pool[i].Get(), ven.Get(), &ov, &outView[i]));
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
        out.YCbCr_Matrix = 1;  // BT.709
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &in);
        vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &out);
        vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
        return true;
    };

    auto drainOutput = [&] {
        MFT_OUTPUT_STREAM_INFO si{};
        enc->GetOutputStreamInfo(0, &si);
        MFT_OUTPUT_DATA_BUFFER ob{};
        ComPtr<IMFSample> own;
        if (!(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            ComPtr<IMFMediaBuffer> b;
            MFCreateSample(&own);
            MFCreateMemoryBuffer(std::max<DWORD>(si.cbSize, outW * outH * 2), &b);
            own->AddBuffer(b.Get());
            ob.pSample = own.Get();
        }
        DWORD status;
        HRESULT hr = enc->ProcessOutput(0, 1, &ob, &status);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(enc->GetOutputAvailableType(0, 0, &t))) enc->SetOutputType(0, t.Get(), 0);
            publishType();
        } else if (SUCCEEDED(hr) && ob.pSample) {
            storeSample(ob.pSample, vBuf);
        }
        if (!own && ob.pSample) ob.pSample->Release();
        if (ob.pEvents) ob.pEvents->Release();
    };

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const LONGLONG frameHns = 10000000 / fps;
    LONGLONG next = nowHns();
    int credits = 0, cur = 0;
    bool haveFrame = false, fresh = false;  // fresh: the frame waiting to be sent is a new screen image
    LONGLONG lastSent = LLONG_MIN / 2, lastKey = LLONG_MIN / 2;
    while (running) {
        next += frameHns;
        LONGLONG wait = next - nowHns();
        if (wait > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -wait;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else if (wait < -4 * frameHns) {
            droppedFrames += (int)(-wait / frameHns);
            next = nowHns();  // fell behind (e.g. system stall): skip ahead instead of bursting
        }
        captureRect(win, monRc, cap);  // follow the window as it moves
        bool gone = win && (!IsWindow(win) || (!IsIconic(win) && MonitorFromWindow(win, MONITOR_DEFAULTTONEAREST) != mon));
        if (gone || (UINT)(cap.right - cap.left) != srcW || (UINT)(cap.bottom - cap.top) != srcH) {
            logf(L"game window now %ldx%ld%ls, restarting capture", cap.right - cap.left, cap.bottom - cap.top,
                 gone ? L" (closed or moved monitor)" : L"");
            resized = true;
            break;
        }

        // Variable frame rate: encode only new desktop images, so a game at 5 fps records 5 fps and costs
        // 5 fps worth of bits. While the screen is frozen, re-send the last frame once a second (keeps keyframes
        // and the clip start fresh).
        HRESULT hr = DXGI_ERROR_ACCESS_LOST;
        if (!dup) duplicate();  // lost on mode switch / HDR toggle / UAC; retry each tick
        if (dup) {
            DXGI_OUTDUPL_FRAME_INFO fi;
            ComPtr<IDXGIResource> res;
            hr = dup->AcquireNextFrame(0, &fi, &res);
            if (SUCCEEDED(hr)) {
                ComPtr<ID3D11Texture2D> tex;
                if (fi.LastPresentTime.QuadPart && SUCCEEDED(res.As(&tex))) {
                    D3D11_TEXTURE2D_DESC td;
                    tex->GetDesc(&td);
                    D3D11_TEXTURE2D_DESC cd{};
                    if (srcTex) srcTex->GetDesc(&cd);
                    if (!srcTex || cd.Width != td.Width || cd.Height != td.Height || frameFmt != td.Format) {
                        if (!buildInput(td)) { dup->ReleaseFrame(); return false; }
                    }
                    if (hdrTex) {
                        ctx->CopyResource(hdrTex.Get(), tex.Get());
                        D3D11_VIEWPORT vpt{0, 0, (float)td.Width, (float)td.Height, 0, 1};
                        ctx->OMSetRenderTargets(1, srcRtv.GetAddressOf(), nullptr);
                        ctx->RSSetViewports(1, &vpt);
                        ctx->IASetInputLayout(nullptr);
                        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        ctx->VSSetShader(tmVs.Get(), nullptr, 0);
                        ctx->PSSetShader(tmPs.Get(), nullptr, 0);
                        ctx->PSSetShaderResources(0, 1, hdrSrv.GetAddressOf());
                        ctx->PSSetConstantBuffers(0, 1, tmCb.GetAddressOf());
                        ctx->Draw(3, 0);
                        ID3D11ShaderResourceView* none = nullptr;
                        ctx->PSSetShaderResources(0, 1, &none);
                        ctx->OMSetRenderTargets(0, nullptr, nullptr);
                    } else {
                        ctx->CopyResource(srcTex.Get(), tex.Get());
                    }
                    cur = (cur + 1) % POOL;
                    D3D11_VIDEO_PROCESSOR_STREAM vs{};
                    vs.Enable = TRUE;
                    vs.pInputSurface = inView.Get();
                    vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &cap);
                    vctx->VideoProcessorBlt(vp.Get(), outView[cur].Get(), 0, 1, &vs);
                    haveFrame = fresh = true;
                }
                dup->ReleaseFrame();
            } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
                dup.Reset();
            }
        }

        ComPtr<IMFMediaEvent> ev;
        while (gen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev) == S_OK) {
            MediaEventType t;
            ev->GetType(&t);
            if (t == METransformNeedInput) credits++;
            else if (t == METransformHaveOutput) drainOutput();
            ev.Reset();
        }
        bool due = haveFrame && (fresh || next - lastSent >= 10000000);
        if (due && credits > 0) {
            ComPtr<IMFSample> s;
            ComPtr<IMFMediaBuffer> b;
            MFCreateSample(&s);
            if (SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pool[cur].Get(), 0, FALSE, &b))) {
                ComPtr<IMF2DBuffer> b2;
                DWORD len = 0;
                if (SUCCEEDED(b.As(&b2))) b2->GetContiguousLength(&len);
                b->SetCurrentLength(len);
                s->AddBuffer(b.Get());
                s->SetSampleTime(next);
                s->SetSampleDuration(frameHns);  // provisional; storeSample stretches it to the next frame
                VARIANT k;
                k.vt = VT_UI4;
                k.ulVal = 1;
                bool key = next - lastKey >= 10000000 && api && SUCCEEDED(api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &k));
                if (SUCCEEDED(enc->ProcessInput(0, s.Get(), 0))) {
                    credits--;
                    fresh = false;
                    lastSent = next;
                    if (key) lastKey = next;
                } else {
                    droppedFrames++;
                }
            }
        } else if (due) {
            droppedFrames++;  // encoder still busy with earlier frames
        }
    }
    CloseHandle(timer);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    enc.Reset();
    act->ShutdownObject();
    return true;
}

