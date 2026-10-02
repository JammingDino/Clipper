// Clipper: tiny replay-buffer game clipper. A hotkey saves the last N seconds as an MP4 under a size cap.
// Pipeline: Desktop Duplication -> GPU scale/convert (D3D11 video processor) -> hardware H.264 (Media Foundation)
//           WASAPI (game-only process loopback / desktop / mic) -> mixer -> AAC
// Encoded packets sit in RAM; saving is a remux, no re-encode. Build: build.bat
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <avrt.h>
#include <tlhelp32.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>
#include <wmcodecdsp.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "ole32")
#pragma comment(lib, "user32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "mmdevapi")
#pragma comment(lib, "wmcodecdspuuid")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "uxtheme")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "avrt")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "d3dcompiler")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df'\"")

using Microsoft::WRL::ComPtr;

// Set by the release build (build.bat passes VERSION from the git tag); local builds show "dev".
#ifndef CLIPPER_VERSION
#define CLIPPER_VERSION dev
#endif
#define WSTR2(x) L## #x
#define WSTR(x) WSTR2(x)
#define CLIPPER_VERSION_W WSTR(CLIPPER_VERSION)

// ---------- misc ----------
static std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    *wcsrchr(p, L'\\') = 0;
    return p;
}

static void logf(const wchar_t* fmt, ...) {
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    FILE* f = _wfopen((exeDir() + L"\\clipper.log").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_list a;
    va_start(a, fmt);
    vfwprintf(f, fmt, a);
    va_end(a);
    fputwc(L'\n', f);
    fclose(f);
}

#define CHECK(x)                                                                 \
    do {                                                                         \
        HRESULT hr_ = (x);                                                       \
        if (FAILED(hr_)) {                                                       \
            logf(L"line %d: %hs failed 0x%08X", __LINE__, #x, (unsigned)hr_);    \
            return false;                                                        \
        }                                                                        \
    } while (0)

// ---------- config ----------
struct Config {
    std::wstring hotkey, game, audio, folder;
    int seconds, height, fps;  // height 0 = auto
    int bitrate, audiokbps;    // video kbps while recording, 0 = auto from maxmb
    bool crop, mono;
    double maxmb, gamevol, micvol, discordvol;
} cfg;

static const char kDefaultIni[] =
    "; Clipper settings. After editing: tray icon > Reload settings.\r\n"
    "[clipper]\r\n"
    "; Save hotkey, e.g. F9, Alt+F10, Ctrl+Shift+S\r\n"
    "hotkey=F9\r\n"
    "; Game exe to watch. Records only while it runs, on the monitor it is on. THE FINALS = Discovery.exe\r\n"
    "; Leave empty to always record the primary monitor.\r\n"
    "game=Discovery.exe\r\n"
    "; Audio: game, discord, mic, desktop joined with + (e.g. game+discord+mic), or none\r\n"
    ";   game    = only the game's sound\r\n"
    ";   discord = only Discord (voice chat)\r\n"
    ";   desktop = everything you hear (don't combine with game/discord or they double up)\r\n"
    "audio=game+discord+mic\r\n"
    "; Volume multipliers (gamevol also applies to desktop)\r\n"
    "gamevol=1.0\r\n"
    "discordvol=1.0\r\n"
    "micvol=1.0\r\n"
    "; Clip length in seconds\r\n"
    "seconds=30\r\n"
    "; Output height: 0 = auto (best resolution for the size cap), or e.g. 1080 / 720 / 540\r\n"
    "height=0\r\n"
    "fps=60\r\n"
    "; 1 = crop ultrawide monitors to the centre 16:9\r\n"
    "crop=0\r\n"
    "; Hard file size cap in MB. Clips are compressed on save to land just under it.\r\n"
    "maxmb=19\r\n"
    "; Video bitrate in kbps while recording. 0 = auto (as much as maxmb allows for the clip length).\r\n"
    "; With a fixed bitrate, clips under maxmb save instantly as recorded; bigger ones are re-compressed to fit.\r\n"
    "bitrate=0\r\n"
    "; AAC audio bitrate: 96, 128, 160 or 192 kbps. mono=1 folds the mix to one channel.\r\n"
    "audiokbps=128\r\n"
    "mono=0\r\n"
    "; Clip folder (empty = Videos\\Clips)\r\n"
    "folder=\r\n";

static std::wstring iniPath() { return exeDir() + L"\\clipper.ini"; }

static void loadConfig() {
    std::wstring ini = iniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(ini.c_str(), L"wb")) { fputs(kDefaultIni, f); fclose(f); }
    }
    auto str = [&](const wchar_t* k, const wchar_t* def) {
        wchar_t b[512];
        GetPrivateProfileStringW(L"clipper", k, def, b, 512, ini.c_str());
        return std::wstring(b);
    };
    auto num = [&](const wchar_t* k, double def) {
        std::wstring s = str(k, L"");
        return s.empty() ? def : _wtof(s.c_str());
    };
    cfg.hotkey = str(L"hotkey", L"F9");
    cfg.game = str(L"game", L"");
    cfg.audio = str(L"audio", L"game+discord+mic");
    for (auto& c : cfg.audio) c = towlower(c);
    cfg.folder = str(L"folder", L"");
    cfg.seconds = std::clamp((int)num(L"seconds", 30), 5, 300);
    int h = (int)num(L"height", 0);
    cfg.height = h <= 0 ? 0 : std::clamp(h, 240, 2160);
    cfg.crop = num(L"crop", 0) != 0;
    cfg.fps = std::clamp((int)num(L"fps", 60), 10, 120);
    cfg.maxmb = std::clamp(num(L"maxmb", 19), 1.0, 4000.0);
    int br = (int)num(L"bitrate", 0);
    cfg.bitrate = br <= 0 ? 0 : std::clamp(br, 1000, 100000);
    cfg.audiokbps = std::clamp((int)std::lround(num(L"audiokbps", 128) / 32) * 32, 96, 192);  // the AAC encoder's rates
    cfg.mono = num(L"mono", 0) != 0;
    cfg.gamevol = num(L"gamevol", 1);
    cfg.micvol = num(L"micvol", 1);
    cfg.discordvol = num(L"discordvol", 1);
    if (cfg.folder.empty()) {
        PWSTR v;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &v))) {
            cfg.folder = std::wstring(v) + L"\\Clips";
            CoTaskMemFree(v);
        }
    }
    CreateDirectoryW(cfg.folder.c_str(), nullptr);
}

static bool hasAudio(const wchar_t* src) { return cfg.audio.find(src) != std::wstring::npos; }
static bool anyAudio() { return hasAudio(L"game") || hasAudio(L"desktop") || hasAudio(L"mic") || hasAudio(L"discord"); }

// video bitrate from the size cap: 5% headroom for container + rate-control wobble, minus the audio track
static UINT32 videoBitrate(double seconds, double maxmb, UINT32 audioBps) {
    return (UINT32)std::max(300000.0, maxmb * 1e6 * 8 * 0.95 / seconds - audioBps);
}

// largest standard height with >= 3.6 bits per pixel per second (clean-looking H.264 for fast games).
// Frame rate is deliberately ignored: the bitrate is what the file size buys.
static UINT pickHeight(UINT32 budget, UINT bufW, UINT bufH) {
    const UINT heights[] = {1440, 1080, 900, 720, 540, 480, 360};
    UINT pick = 360;
    for (UINT h : heights) {
        if (h > bufH) continue;
        double w = (double)h * bufW / bufH;
        if (budget / (w * h) >= 3.6) return h;
        pick = h;
    }
    return std::min(pick, bufH);
}

// What a clip will come out as, for the settings window. Auto bitrate: always ~maxmb. Fixed bitrate: the CBR
// buffer's size, plus how long a clip can be and still save as-is under the cap.
struct Estimate {
    double mb, secsFit;
    UINT32 vbps;
    UINT h;
};
static Estimate estimate(int secs, double maxmb, int bitrateKbps, UINT32 audioBps, int height, UINT srcW, UINT srcH) {
    Estimate e{maxmb, 0, videoBitrate(secs, maxmb, audioBps), 0};
    if (bitrateKbps) {
        e.vbps = bitrateKbps * 1000;
        e.mb = (e.vbps + audioBps) * secs / 8e6 * 1.01;  // ~1% MP4 overhead
        e.secsFit = maxmb * 8e6 * 0.95 / (e.vbps + audioBps);
    }
    e.h = height ? std::min<UINT>(height, srcH) : pickHeight(e.vbps, srcW, srcH);
    return e;
}

static const struct { const wchar_t* name; UINT vk; } keyNames[] = {
    {L"INSERT", VK_INSERT}, {L"DELETE", VK_DELETE}, {L"HOME", VK_HOME}, {L"END", VK_END},
    {L"PAGEUP", VK_PRIOR}, {L"PAGEDOWN", VK_NEXT}, {L"PAUSE", VK_PAUSE},
    {L"PRINTSCREEN", VK_SNAPSHOT}, {L"SCROLLLOCK", VK_SCROLL}};

// inverse of parseHotkey, for the hotkey picker. Empty = key not supported.
static std::wstring formatHotkey(UINT mods, UINT vk) {
    std::wstring key;
    if (vk >= VK_F1 && vk <= VK_F24) key = L"F" + std::to_wstring(vk - VK_F1 + 1);
    else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) key = std::wstring(1, (wchar_t)vk);
    else
        for (auto& n : keyNames)
            if (n.vk == vk) key = n.name;
    if (key.empty()) return key;
    return std::wstring(mods & MOD_CONTROL ? L"Ctrl+" : L"") + (mods & MOD_ALT ? L"Alt+" : L"") +
           (mods & MOD_SHIFT ? L"Shift+" : L"") + (mods & MOD_WIN ? L"Win+" : L"") + key;
}

static bool parseHotkey(std::wstring s, UINT& mods, UINT& vk) {
    mods = MOD_NOREPEAT;
    vk = 0;
    for (auto& c : s) c = towupper(c);
    size_t p = 0;
    while (p <= s.size()) {
        size_t e = s.find(L'+', p);
        if (e == std::wstring::npos) e = s.size();
        std::wstring k = s.substr(p, e - p);
        p = e + 1;
        k.erase(0, k.find_first_not_of(L' '));
        k.erase(k.find_last_not_of(L' ') + 1);
        if (k == L"CTRL" || k == L"CONTROL") mods |= MOD_CONTROL;
        else if (k == L"ALT") mods |= MOD_ALT;
        else if (k == L"SHIFT") mods |= MOD_SHIFT;
        else if (k == L"WIN") mods |= MOD_WIN;
        else if (k.size() > 1 && k[0] == L'F' && iswdigit(k[1])) {
            int n = _wtoi(k.c_str() + 1);
            if (n < 1 || n > 24) return false;
            vk = VK_F1 + n - 1;
        } else if (k.size() == 1 && iswalnum(k[0])) vk = k[0];
        else {
            auto it = std::find_if(std::begin(keyNames), std::end(keyNames), [&](auto& n) { return k == n.name; });
            if (it == std::end(keyNames)) return false;
            vk = it->vk;
        }
    }
    return vk != 0;
}

// ---------- clock: everything is 100ns units since recording start ----------
static LONGLONG qpcFreq, startHns;
static std::atomic<int> droppedFrames;  // capture ticks that produced no frame (encoder busy / thread stalled)
static LONGLONG qpcToHns(LONGLONG q) { return q / qpcFreq * 10000000 + q % qpcFreq * 10000000 / qpcFreq; }
static LONGLONG nowHns() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return qpcToHns(q.QuadPart) - startHns;
}

// ---------- replay buffer ----------
struct Packet {
    LONGLONG t = 0, dur = 0;
    bool key = false;
    std::vector<BYTE> data;
};
static std::mutex bufMx;
static std::deque<Packet> vBuf, aBuf;
static ComPtr<IMFMediaType> vType, aType;
static std::atomic<bool> running;

static void storeSample(IMFSample* s, std::deque<Packet>& q) {
    Packet p;
    s->GetSampleTime(&p.t);
    s->GetSampleDuration(&p.dur);
    p.key = MFGetAttributeUINT32(s, MFSampleExtension_CleanPoint, 0) != 0;
    ComPtr<IMFMediaBuffer> b;
    if (FAILED(s->ConvertToContiguousBuffer(&b))) return;
    BYTE* d;
    DWORD n;
    if (FAILED(b->Lock(&d, nullptr, &n))) return;
    p.data.assign(d, d + n);
    b->Unlock();
    std::lock_guard<std::mutex> l(bufMx);
    q.push_back(std::move(p));
    LONGLONG keep = (cfg.seconds + 5) * 10000000LL;
    while (q.size() > 1 && q.back().t - q.front().t > keep) q.pop_front();
}

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

static std::atomic<bool> resized;  // capture area changed size: the watcher restarts recording at the new size

// Game window's client area in monitor pixels, clipped to the monitor (centre 16:9 if crop is on). Leaves r alone
// while the window is minimised or gone, so alt-tab doesn't count as a resize. Caller must be per-monitor DPI aware.
// ponytail: assumes an unrotated monitor
static void captureRect(HWND w, const RECT& mon, RECT& r) {
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

static bool videoLoop(HMONITOR mon, HWND win) {
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
        v.ulVal = fps;  // keyframe every second = clip start granularity
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
    bool haveFrame = false;
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

        // grab the newest desktop image if it changed; otherwise re-send the last converted frame (constant fps)
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
                    haveFrame = true;
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
        if (credits > 0 && haveFrame) {
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
                s->SetSampleDuration(frameHns);
                if (SUCCEEDED(enc->ProcessInput(0, s.Get(), 0))) credits--;
                else droppedFrames++;
            }
        } else if (haveFrame) {
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

static bool captureLoop(int si, DWORD pid) {
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

static bool audioLoop() {
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

// ---------- saving ----------
// SPS+PPS from an Annex-B keyframe, needed by the MP4 muxer in passthrough mode
static std::vector<BYTE> seqHeader(const std::vector<BYTE>& d) {
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
// The buffer is recorded at 2x the cap's bitrate. On save the clip is always re-encoded with VBR (calm moments get
// few bits, fights get many), and the bitrate is walked until the file lands just under the cap. Resolution is
// picked from the bitrate: fewer clean pixels beat many blocky ones.
static ULONGLONG fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

static bool transcode(const std::wstring& src, const std::wstring& dst, UINT w, UINT h, UINT32 bitrate,
                      IMFDXGIDeviceManager* mgr) {
    ComPtr<IMFAttributes> ra, wa, encAttrs;
    MFCreateAttributes(&ra, 3);
    ra->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);  // GPU decode + resize
    ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    ComPtr<IMFSourceReader> r;
    CHECK(MFCreateSourceReaderFromURL(src.c_str(), ra.Get(), &r));
    ComPtr<IMFMediaType> native, raw, cur, audio, out;
    CHECK(r->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native));
    UINT32 fn = 60, fd = 1;
    MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &fn, &fd);
    MFCreateMediaType(&raw);
    raw->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    raw->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(raw.Get(), MF_MT_FRAME_SIZE, w, h);
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
    encAttrs->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
    encAttrs->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate);
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
static ULONGLONG fitToSize(const std::wstring& src, const std::wstring& tmp, const std::wstring& dst, UINT w, UINT h,
                           ULONGLONG limit, UINT32 bitrate, IMFDXGIDeviceManager* mgr) {
    ULONGLONG best = 0;
    for (int pass = 0; pass < 5 && best < limit * 0.97; pass++) {
        if (!transcode(src, tmp, w, h, bitrate, mgr)) break;
        ULONGLONG size = fileSize(tmp);
        logf(L"  pass %d: %ux%u %u kbps -> %.2f MB", pass + 1, w, h, bitrate / 1000, size / 1e6);
        if (!size) break;
        if (size <= limit && size > best && MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) best = size;
        bitrate = (UINT32)std::clamp(bitrate * (limit * 0.985 / size), 1e5, 250e6);
    }
    return best;
}

static NOTIFYICONDATAW nid{sizeof(nid)};
static std::wstring lastClip;
static HWND ui;
static std::atomic<bool> saving;
static std::thread saveThread;
enum { TRAY_MSG = WM_APP, SHOW_MSG = WM_APP + 1, STATUS_MSG = WM_APP + 2 };

static void notify(const std::wstring& title, const std::wstring& text, DWORD icon) {
    NOTIFYICONDATAW n = nid;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    n.dwInfoFlags = icon | NIIF_NOSOUND;  // our own beep is the audible cue (balloons are muted in fullscreen games)
    Shell_NotifyIconW(NIM_MODIFY, &n);
}
static void fail(const std::wstring& title, const std::wstring& text) {
    MessageBeep(MB_ICONHAND);
    notify(title, text, NIIF_WARNING);
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
        logf(L"saving %.1fs clip, buffer %ux%u %.1f MB, %d frames dropped since the last save", secs, bufW, bufH, size / 1e6,
             droppedFrames.exchange(0));
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
            size = fitToSize(src, tmp, path, outW, outH, limit, budget, mgr.Get());
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

static void saveClip() {
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

// ---------- recording lifecycle ----------
static std::vector<std::thread> workers;
static DWORD gamePid;
static std::atomic<bool> workerError;
static bool discordMissing;

template <class F>
static void spawn(const wchar_t* what, F f) {
    workers.emplace_back([what, f] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (!f()) { logf(L"%ls stopped with an error", what); workerError = true; }
        CoUninitialize();
    });
}

// Returns the root process of that name (Discord and many games spawn helper children with the same exe).
static DWORD findProcess(const std::wstring& exe) {
    std::vector<std::pair<DWORD, DWORD>> m;  // pid, parent
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, exe.c_str())) m.push_back({pe.th32ProcessID, pe.th32ParentProcessID});
    CloseHandle(snap);
    for (auto& [pid, parent] : m)
        if (std::none_of(m.begin(), m.end(), [&](auto& o) { return o.first == parent; })) return pid;
    return m.empty() ? 0 : m[0].first;
}

static void setGains() {
    std::lock_guard<std::mutex> l(aMx);
    aSrc[0].gain = aSrc[1].gain = (float)cfg.gamevol;
    aSrc[2].gain = (float)cfg.micvol;
    aSrc[3].gain = (float)cfg.discordvol;
}

static void startRecording(HMONITOR mon, DWORD pid, HWND win) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    startHns = qpcToHns(q.QuadPart);
    {
        std::lock_guard<std::mutex> l(bufMx);
        vBuf.clear();
        aBuf.clear();
        vType.Reset();
        aType.Reset();
    }
    {
        std::lock_guard<std::mutex> l(aMx);
        mixPos = 0;
        for (auto& s : aSrc) { std::fill(s.ring.begin(), s.ring.end(), (int16_t)0); s.wpos = LLONG_MIN; }
    }
    setGains();
    running = true;
    workerError = resized = false;
    spawn(L"video", [mon, win] { return videoLoop(mon, win); });
    if (hasAudio(L"game") && pid) spawn(L"game audio", [pid] { return captureLoop(0, pid); });
    if (hasAudio(L"desktop") || (hasAudio(L"game") && !pid)) spawn(L"desktop audio", [] { return captureLoop(1, 0); });
    if (hasAudio(L"mic")) spawn(L"mic", [] { return captureLoop(2, 0); });
    // ponytail: Discord looked up once at start; opening it later needs an audio setting toggle to pick it up
    DWORD d = hasAudio(L"discord") ? findProcess(L"Discord.exe") : 0;
    discordMissing = hasAudio(L"discord") && !d;
    if (d) spawn(L"discord audio", [d] { return captureLoop(3, d); });
    if (anyAudio()) spawn(L"audio encoder", [] { return audioLoop(); });
    logf(L"recording started (game pid %lu, discord pid %lu)", pid, d);
}

static void stopRecording() {
    running = false;
    for (auto& t : workers) t.join();
    workers.clear();
    gamePid = 0;
}

static HWND findMainWindow(DWORD pid) {
    struct Find { DWORD pid; HWND best; LONG area; } f{pid, nullptr, 0};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto& f = *(Find*)lp;
        DWORD p;
        GetWindowThreadProcessId(h, &p);
        RECT r;
        if (p == f.pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && GetWindowRect(h, &r)) {
            LONG area = (r.right - r.left) * (r.bottom - r.top);
            if (area > f.area) { f.area = area; f.best = h; }
        }
        return TRUE;
    }, (LPARAM)&f);
    return f.best;
}

// exe names of programs that currently have a visible window, for the game picker
static std::vector<std::wstring> windowedApps() {
    struct Ctx { std::vector<std::pair<DWORD, std::wstring>> procs; std::vector<std::wstring> out; } c;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID != GetCurrentProcessId()) c.procs.push_back({pe.th32ProcessID, pe.szExeFile});
    CloseHandle(snap);
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto& c = *(Ctx*)lp;
        if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || !GetWindowTextLengthW(h)) return TRUE;
        DWORD pid;
        GetWindowThreadProcessId(h, &pid);
        for (auto& [p, exe] : c.procs)
            if (p == pid && std::find(c.out.begin(), c.out.end(), exe) == c.out.end()) c.out.push_back(exe);
        return TRUE;
    }, (LPARAM)&c);
    std::sort(c.out.begin(), c.out.end(), [](auto& a, auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return c.out;
}

// ---------- settings window (also the hidden owner of the hotkey + tray icon) ----------
// Every change is applied ~0.6s after the last edit; recording only restarts if a capture setting changed.
enum {
    ID_STATUS = 100, ID_SAVECLIP, ID_OPENFOLDER, ID_GAME, ID_REFRESH, ID_HINT, ID_HOTKEY,
    ID_A_GAME, ID_A_DISCORD, ID_A_MIC, ID_A_DESKTOP, ID_V_GAME, ID_V_DISCORD, ID_V_MIC,
    ID_SECONDS, ID_SECONDS_UD, ID_HEIGHT, ID_FPS, ID_CROP, ID_MAXMB, ID_FOLDER, ID_BROWSE, ID_HKHINT,
    ID_AQUALITY, ID_MONO, ID_BITRATE, ID_ESTIMATE
};
enum { TIMER_WATCH = 1, TIMER_APPLY = 2 };
static HFONT uiFont, uiBold;
static HWND statusTip;
static bool loadingUi;

// dark theme to match the icon: near-black window, dark grey cards, red accent, square corners
static const COLORREF C_BG = RGB(16, 16, 20), C_CARD = RGB(28, 29, 35), C_CARDLINE = RGB(40, 41, 49),
                      C_CTRL = RGB(42, 43, 51), C_CTRLHOT = RGB(54, 55, 64), C_LINE = RGB(70, 72, 82),
                      C_TEXT = RGB(236, 236, 240), C_MUTED = RGB(150, 152, 162), C_RED = RGB(222, 22, 42),
                      C_REDHOT = RGB(255, 64, 80), C_REDDEEP = RGB(150, 6, 24), C_AMBER = RGB(255, 170, 60);
static HBRUSH brBg, brCard, brCtrl;
static const struct { RECT r; const wchar_t* title; } cards[] = {
    {{12, 50, 480, 174}, L"RECORDING"}, {{12, 182, 480, 342}, L"AUDIO"}, {{12, 350, 480, 558}, L"CLIPS"}};

static void box(HDC dc, RECT r, COLORREF fill, COLORREF line) {
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, line);
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
}
static void chevron(HDC dc, int x, int y, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 2, c);
    HGDIOBJ old = SelectObject(dc, p);
    POINT v[] = {{x - 4, y - 2}, {x, y + 2}, {x + 4, y - 2}};
    Polyline(dc, v, 3);
    SelectObject(dc, old);
    DeleteObject(p);
}

// themed combo frames don't take our colours: paint the closed combo as a flat box with a chevron
static LRESULT CALLBACK comboProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_ERASEBKGND) return 1;
    if (m != WM_PAINT) return DefSubclassProc(h, m, w, l);
    COMBOBOXINFO ci{sizeof(ci)};
    GetComboBoxInfo(h, &ci);
    bool editable = (GetWindowLongW(h, GWL_STYLE) & 3) == CBS_DROPDOWN;
    bool active = GetFocus() == h || (editable && GetFocus() == ci.hwndItem) || SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0);
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT r;
    GetClientRect(h, &r);
    if (editable) {  // the edit child paints itself
        RECT e;
        GetWindowRect(ci.hwndItem, &e);
        MapWindowPoints(nullptr, h, (POINT*)&e, 2);
        ExcludeClipRect(dc, e.left, e.top, e.right, e.bottom);
    }
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    box(dc, r, C_CTRL, active ? C_REDHOT : C_LINE);
    if (!editable) {
        wchar_t t[128] = L"";
        LRESULT i = SendMessageW(h, CB_GETCURSEL, 0, 0);
        if (i >= 0 && SendMessageW(h, CB_GETLBTEXTLEN, i, 0) < 128) SendMessageW(h, CB_GETLBTEXT, i, (LPARAM)t);
        RECT tr{r.left + 6, r.top, ci.rcButton.left, r.bottom};
        SelectObject(dc, uiFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, t, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    chevron(dc, (ci.rcButton.left + ci.rcButton.right) / 2, (r.top + r.bottom) / 2, active ? C_TEXT : C_MUTED);
    EndPaint(h, &ps);
    return 0;
}

static HWND ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, ui, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)uiFont, TRUE);
    SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
    COMBOBOXINFO ci{sizeof(ci)};
    if (GetComboBoxInfo(c, &ci)) {
        SetWindowTheme(ci.hwndList, L"DarkMode_Explorer", nullptr);  // dark dropdown scrollbar
        SetWindowSubclass(c, comboProc, 0, 0);
    }
    return c;
}
static bool onCard(HWND c) {
    int id = GetDlgCtrlID(c);
    return id != ID_STATUS && id != ID_SAVECLIP && id != ID_OPENFOLDER;
}
// buttons and checkboxes are painted here (NM_CUSTOMDRAW) so they keep their native behaviour
static LRESULT drawButton(NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    HWND b = cd->hdr.hwndFrom;
    HDC dc = cd->hdc;
    RECT r = cd->rc;
    bool hot = cd->uItemState & CDIS_HOT, down = cd->uItemState & CDIS_SELECTED;
    bool focus = (cd->uItemState & CDIS_FOCUS) && !(SendMessageW(b, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    wchar_t t[128];
    GetWindowTextW(b, t, 128);
    SaveDC(dc);
    FillRect(dc, &r, onCard(b) ? brCard : brBg);
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SelectObject(dc, uiFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_TEXT);
    if ((GetWindowLongW(b, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX) {
        bool on = SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED;
        int y = (r.top + r.bottom) / 2;
        RECT k{r.left, y - 8, r.left + 16, y + 8};
        COLORREF red = hot ? C_REDHOT : C_RED;
        box(dc, k, on ? red : C_CTRL, on ? red : hot || focus ? C_REDHOT : C_LINE);
        if (on) {
            HPEN p = CreatePen(PS_SOLID, 2, C_TEXT);
            SelectObject(dc, p);
            POINT v[] = {{k.left + 4, y}, {k.left + 7, y + 3}, {k.left + 12, y - 3}};
            Polyline(dc, v, 3);
            SelectObject(dc, GetStockObject(DC_PEN));
            DeleteObject(p);
        }
        r.left += 24;
        DrawTextW(dc, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    } else {
        bool primary = GetDlgCtrlID(b) == ID_SAVECLIP;
        COLORREF fill = primary ? (down ? C_REDDEEP : hot ? C_REDHOT : C_RED) : (down ? C_LINE : hot ? C_CTRLHOT : C_CTRL);
        box(dc, r, fill, focus ? (primary ? C_TEXT : C_REDHOT) : primary ? fill : C_LINE);
        DrawTextW(dc, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }
    RestoreDC(dc, -1);
    return CDRF_SKIPDEFAULT;
}

// volume sliders: thin track filled red up to a square thumb, small mark at 100%
static LRESULT drawSlider(NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT || cd->dwItemSpec == TBCD_TICS) return CDRF_SKIPDEFAULT;
    HWND tb = cd->hdr.hwndFrom;
    HDC dc = cd->hdc;
    RECT th, r;
    SendMessageW(tb, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
    int x = (th.left + th.right) / 2, y = (th.top + th.bottom) / 2;
    SaveDC(dc);
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    if (cd->dwItemSpec == TBCD_CHANNEL) {
        int tic = (int)SendMessageW(tb, TBM_GETTICPOS, 0, 0);
        box(dc, {tic - 1, y + 5, tic + 1, y + 10}, C_MUTED, C_MUTED);
        box(dc, {cd->rc.left, y - 2, cd->rc.right, y + 2}, C_LINE, C_LINE);
        box(dc, {cd->rc.left, y - 2, x, y + 2}, C_RED, C_RED);
    } else {
        bool hot = cd->uItemState & (CDIS_HOT | CDIS_SELECTED);
        r = {x - 5, y - 9, x + 5, y + 9};
        box(dc, r, hot ? C_REDHOT : C_TEXT, hot ? C_REDHOT : C_TEXT);
    }
    RestoreDC(dc, -1);
    return CDRF_SKIPDEFAULT;
}
static HWND item(int id) { return GetDlgItem(ui, id); }
static std::wstring text(int id) {
    wchar_t b[MAX_PATH];
    GetDlgItemTextW(ui, id, b, MAX_PATH);
    return b;
}
static UINT swapAltShift(UINT m) { return (m & 2) | (m & 1 ? 4 : 0) | (m & 4 ? 1 : 0); }  // MOD_* <-> HOTKEYF_*

// the hotkey control ignores WM_CTLCOLOR*, so paint it ourselves; a red border replaces its caret
static LRESULT CALLBACK hotkeyProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_SETFOCUS || m == WM_KILLFOCUS) {
        LRESULT r = DefSubclassProc(h, m, w, l);
        HideCaret(h);
        InvalidateRect(h, nullptr, TRUE);
        return r;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m != WM_PAINT) return DefSubclassProc(h, m, w, l);
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT r;
    GetClientRect(h, &r);
    WORD k = (WORD)SendMessageW(h, HKM_GETHOTKEY, 0, 0);
    std::wstring t = formatHotkey(swapAltShift(HIBYTE(k) & 7), LOBYTE(k));
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SelectObject(dc, uiFont);
    box(dc, r, C_CTRL, GetFocus() == h ? C_REDHOT : C_LINE);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, t.empty() ? C_MUTED : C_TEXT);
    r.left += 6;
    DrawTextW(dc, t.empty() ? L"Press a key" : t.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    EndPaint(h, &ps);
    return 0;
}

static const wchar_t kHotkeyHint[] = L"Click the box, then press the key combo you want.";

static void buildUi() {
    const int L = 24, X = 140, W = 330;
    ctl(L"BUTTON", L"Save clip now", WS_TABSTOP, 12, 12, 140, 28, ID_SAVECLIP);
    ctl(L"BUTTON", L"Open clips folder", WS_TABSTOP, 160, 12, 140, 28, ID_OPENFOLDER);
    // status is an icon; the detail text lives in its tooltip
    HWND st = ctl(L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY, 448, 12, 32, 28, ID_STATUS);
    statusTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                                ui, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowTheme(statusTip, L"DarkMode_Explorer", nullptr);
    TOOLINFOW ti{sizeof(ti), TTF_IDISHWND | TTF_SUBCLASS, ui, (UINT_PTR)st};
    ti.lpszText = (LPWSTR)L"";
    SendMessageW(statusTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);

    ctl(L"STATIC", L"Game", 0, L, 78, 110, 20, 0);
    ctl(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, X, 74, 250, 300, ID_GAME);
    ctl(L"BUTTON", L"Refresh", WS_TABSTOP, X + 256, 73, 74, 25, ID_REFRESH);
    ctl(L"STATIC", L"Only records while this is running. Blank = always record.", 0, X, 101, W, 18, ID_HINT);
    ctl(L"STATIC", L"Save hotkey", 0, L, 126, 110, 20, 0);
    HWND hk = ctl(HOTKEY_CLASSW, L"", WS_TABSTOP, X, 123, 160, 24, ID_HOTKEY);
    SetWindowSubclass(hk, hotkeyProc, 0, 0);
    SetWindowLongPtrW(hk, GWL_EXSTYLE, 0);  // drop the light client edge the control adds itself
    SetWindowPos(hk, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    SendMessageW(hk, HKM_SETRULES, 0, 0);
    ctl(L"STATIC", kHotkeyHint, SS_NOPREFIX, X, 150, W, 18, ID_HKHINT);

    const wchar_t* names[] = {L"Game sound", L"Discord (voice chat)", L"Microphone",
                              L"Everything you hear (desktop), already includes game + Discord"};
    for (int i = 0; i < 4; i++) {
        int y = 206 + i * 26;
        ctl(L"BUTTON", names[i], BS_AUTOCHECKBOX | WS_TABSTOP, L, y, i == 3 ? 440 : 190, 22, ID_A_GAME + i);
        if (i == 3) break;
        HWND tb = ctl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_TOOLTIPS | WS_TABSTOP, 230, y - 2, 240, 26, ID_V_GAME + i);
        SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
        SendMessageW(tb, TBM_SETTIC, 0, 100);
        SetWindowTheme((HWND)SendMessageW(tb, TBM_GETTOOLTIPS, 0, 0), L"DarkMode_Explorer", nullptr);
    }
    ctl(L"STATIC", L"Quality", 0, L, 313, 110, 20, 0);
    ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, X, 309, 160, 200, ID_AQUALITY);
    ctl(L"BUTTON", L"Mono", BS_AUTOCHECKBOX | WS_TABSTOP, X + 180, 310, 120, 22, ID_MONO);

    ctl(L"STATIC", L"Clip length", 0, L, 376, 110, 20, 0);
    ctl(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, X, 373, 60, 23, ID_SECONDS);
    HWND ud = ctl(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_AUTOBUDDY, 0, 0, 0, 0, ID_SECONDS_UD);
    SendMessageW(ud, UDM_SETRANGE32, 5, 300);
    ctl(L"STATIC", L"seconds", 0, X + 68, 376, 80, 20, 0);
    ctl(L"STATIC", L"Resolution", 0, L, 406, 110, 20, 0);
    ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, X, 403, 160, 200, ID_HEIGHT);
    ctl(L"STATIC", L"FPS", 0, X + 180, 406, 30, 20, 0);
    ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, X + 214, 403, 70, 200, ID_FPS);
    ctl(L"BUTTON", L"Crop ultrawide to 16:9 (centre)", BS_AUTOCHECKBOX | WS_TABSTOP, X, 432, W, 22, ID_CROP);
    ctl(L"STATIC", L"Video bitrate", 0, L, 464, 110, 20, 0);
    ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, X, 461, 160, 300, ID_BITRATE);
    ctl(L"STATIC", L"Max", 0, X + 180, 464, 30, 20, 0);
    ctl(L"EDIT", L"", WS_BORDER | WS_TABSTOP, X + 214, 461, 50, 23, ID_MAXMB);
    ctl(L"STATIC", L"MB", 0, X + 270, 464, 30, 20, 0);
    ctl(L"STATIC", L"", SS_NOPREFIX, X, 489, W, 32, ID_ESTIMATE);
    ctl(L"STATIC", L"Save to", 0, L, 528, 110, 20, 0);
    ctl(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X, 525, 250, 23, ID_FOLDER);
    ctl(L"BUTTON", L"Browse...", WS_TABSTOP, X + 256, 524, 74, 25, ID_BROWSE);
}

static void fillCombo(int id, const std::vector<std::pair<std::wstring, int>>& opts, int cur) {
    HWND c = item(id);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    auto all = opts;
    if (std::none_of(all.begin(), all.end(), [&](auto& o) { return o.second == cur; }))
        all.push_back({std::to_wstring(cur), cur});
    for (auto& [label, val] : all) {
        LRESULT i = SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        SendMessageW(c, CB_SETITEMDATA, i, val);
        if (val == cur) SendMessageW(c, CB_SETCURSEL, i, 0);
    }
}
static int comboValue(int id) {
    HWND c = item(id);
    return (int)SendMessageW(c, CB_GETITEMDATA, SendMessageW(c, CB_GETCURSEL, 0, 0), 0);
}

static void refreshGames() {
    std::wstring cur = text(ID_GAME);
    HWND c = item(ID_GAME);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (auto& exe : windowedApps()) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)exe.c_str());
    SetWindowTextW(c, cur.c_str());
}

// live size/bitrate estimate from what's in the window right now (before it's applied)
static void updateEstimate() {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);  // physical pixels of the primary monitor
    UINT w = dm.dmPelsWidth ? dm.dmPelsWidth : 1920, h = dm.dmPelsHeight ? dm.dmPelsHeight : 1080;
    if (IsDlgButtonChecked(ui, ID_CROP) == BST_CHECKED) w = std::min(w, h * 16 / 9);
    bool audio = false;
    for (int i = 0; i < 4; i++) audio |= IsDlgButtonChecked(ui, ID_A_GAME + i) == BST_CHECKED;
    int secs = std::clamp((int)GetDlgItemInt(ui, ID_SECONDS, nullptr, FALSE), 5, 300), kbps = comboValue(ID_BITRATE);
    double maxmb = std::clamp(_wtof(text(ID_MAXMB).c_str()), 1.0, 4000.0);
    Estimate e = estimate(secs, maxmb, kbps, audio ? comboValue(ID_AQUALITY) * 1000 : 0, comboValue(ID_HEIGHT), w, h);
    wchar_t b[256];
    if (!kbps)
        swprintf(b, 256, L"Each %ds clip is compressed to just under %g MB: about %.1f Mbps video at %up.", secs, maxmb, e.vbps / 1e6, e.h);
    else if (e.mb <= maxmb)
        swprintf(b, 256, L"Up to about %.1f MB per %ds clip at %up, saved instantly. Up to %ds always fits %g MB.", e.mb, secs, e.h, (int)e.secsFit, maxmb);
    else
        swprintf(b, 256, L"Up to about %.1f MB per %ds clip: busy clips over %g MB are re-compressed to fit. Up to %ds always saves as-is.",
                 e.mb, secs, maxmb, (int)e.secsFit);
    SetDlgItemTextW(ui, ID_ESTIMATE, b);
}

static void loadUi() {
    loadingUi = true;
    SetDlgItemTextW(ui, ID_GAME, cfg.game.c_str());
    refreshGames();
    UINT mods = 0, vk = 0;
    parseHotkey(cfg.hotkey, mods, vk);
    SendMessageW(item(ID_HOTKEY), HKM_SETHOTKEY, MAKEWORD(vk, swapAltShift(mods & 7)), 0);
    const wchar_t* src[] = {L"game", L"discord", L"mic", L"desktop"};
    for (int i = 0; i < 4; i++) CheckDlgButton(ui, ID_A_GAME + i, hasAudio(src[i]) ? BST_CHECKED : BST_UNCHECKED);
    double vols[] = {cfg.gamevol, cfg.discordvol, cfg.micvol};
    for (int i = 0; i < 3; i++) SendMessageW(item(ID_V_GAME + i), TBM_SETPOS, TRUE, (LPARAM)(vols[i] * 100 + 0.5));
    SetDlgItemInt(ui, ID_SECONDS, cfg.seconds, FALSE);
    fillCombo(ID_HEIGHT, {{L"Auto (recommended)", 0}, {L"1080p", 1080}, {L"720p", 720}, {L"540p", 540}, {L"480p", 480}}, cfg.height);
    fillCombo(ID_FPS, {{L"30", 30}, {L"60", 60}}, cfg.fps);
    CheckDlgButton(ui, ID_CROP, cfg.crop ? BST_CHECKED : BST_UNCHECKED);
    wchar_t b[32];
    swprintf(b, 32, L"%g", cfg.maxmb);
    SetDlgItemTextW(ui, ID_MAXMB, b);
    SetDlgItemTextW(ui, ID_FOLDER, cfg.folder.c_str());
    fillCombo(ID_AQUALITY, {{L"High (192 kbps)", 192}, {L"Good (160 kbps)", 160}, {L"Standard (128 kbps)", 128}, {L"Small (96 kbps)", 96}},
              cfg.audiokbps);
    CheckDlgButton(ui, ID_MONO, cfg.mono ? BST_CHECKED : BST_UNCHECKED);
    fillCombo(ID_BITRATE, {{L"Auto (fit max size)", 0}, {L"4 Mbps", 4000}, {L"6 Mbps", 6000}, {L"8 Mbps", 8000}, {L"12 Mbps", 12000},
                           {L"16 Mbps", 16000}, {L"20 Mbps", 20000}, {L"30 Mbps", 30000}, {L"50 Mbps", 50000}}, cfg.bitrate);
    SetDlgItemTextW(ui, ID_HKHINT, kHotkeyHint);
    loadingUi = false;
    updateEstimate();
}

static void updateStatus() {
    std::wstring s;
    if (running) {
        s = L"Recording. " + cfg.hotkey + L" saves a clip";
        if (discordMissing) s += L" (Discord not running)";
        if (workerError) s += L" ⚠ capture error, see clipper.log";
    } else {
        s = L"Waiting for " + cfg.game + L" to start";
    }
    if (saving) s += L"  ·  compressing clip…";
    SetDlgItemTextW(ui, ID_STATUS, s.c_str());  // read by screen readers; the icon is drawn in WM_DRAWITEM
    InvalidateRect(item(ID_STATUS), nullptr, TRUE);
    TOOLINFOW ti{sizeof(ti), 0, ui, (UINT_PTR)item(ID_STATUS)};
    ti.lpszText = s.data();
    SendMessageW(statusTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    NOTIFYICONDATAW n = nid;
    n.uFlags = NIF_TIP;
    wcsncpy_s(n.szTip, (L"Clipper: " + s).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void watchGame() {
    if (cfg.game.empty()) {
        if (!running) startRecording(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), 0, nullptr);
    } else {
        DWORD pid = findProcess(cfg.game);
        if (running && (pid != gamePid || resized)) stopRecording();  // game closed/restarted, or window resized/moved
        if (!running && pid) {
            HWND w = findMainWindow(pid);
            if (w) {
                gamePid = pid;
                startRecording(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), pid, w);
            }
        }
    }
    updateStatus();
}

static bool registerHotkey() {
    UINT mods, vk;
    UnregisterHotKey(ui, 1);
    if (parseHotkey(cfg.hotkey, mods, vk) && RegisterHotKey(ui, 1, mods, vk)) return true;
    SetDlgItemTextW(ui, ID_HKHINT, (L"⚠ " + cfg.hotkey + L" is taken by another program. Pick another.").c_str());
    return false;
}

static void applyUi() {
    std::wstring ini = iniPath(), note;
    auto put = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"clipper", k, v.c_str(), ini.c_str()); };
    WORD hk = (WORD)SendMessageW(item(ID_HOTKEY), HKM_GETHOTKEY, 0, 0);
    std::wstring hotkey = formatHotkey(swapAltShift(HIBYTE(hk) & 7), LOBYTE(hk));
    if (hotkey.empty()) note = L"⚠ Use F1-F24, A-Z, 0-9 or Ins/Home/End/PgUp/PgDn/Pause.";
    else put(L"hotkey", hotkey);
    std::wstring audio;
    const wchar_t* src[] = {L"game", L"discord", L"mic", L"desktop"};
    for (int i = 0; i < 4; i++)
        if (IsDlgButtonChecked(ui, ID_A_GAME + i) == BST_CHECKED) audio += (audio.empty() ? L"" : L"+") + std::wstring(src[i]);
    auto vol = [](int id) {
        wchar_t b[16];
        swprintf(b, 16, L"%.2f", SendMessageW(item(id), TBM_GETPOS, 0, 0) / 100.0);
        return std::wstring(b);
    };
    put(L"game", text(ID_GAME));
    put(L"audio", audio.empty() ? L"none" : audio);
    put(L"gamevol", vol(ID_V_GAME));
    put(L"discordvol", vol(ID_V_DISCORD));
    put(L"micvol", vol(ID_V_MIC));
    put(L"seconds", text(ID_SECONDS));
    put(L"height", std::to_wstring(comboValue(ID_HEIGHT)));
    put(L"fps", std::to_wstring(comboValue(ID_FPS)));
    put(L"crop", IsDlgButtonChecked(ui, ID_CROP) == BST_CHECKED ? L"1" : L"0");
    put(L"maxmb", text(ID_MAXMB));
    put(L"bitrate", std::to_wstring(comboValue(ID_BITRATE)));
    put(L"audiokbps", std::to_wstring(comboValue(ID_AQUALITY)));
    put(L"mono", IsDlgButtonChecked(ui, ID_MONO) == BST_CHECKED ? L"1" : L"0");
    put(L"folder", text(ID_FOLDER));

    Config old = cfg;
    loadConfig();
    setGains();
    bool restart = old.game != cfg.game || old.audio != cfg.audio || old.height != cfg.height || old.fps != cfg.fps ||
                   old.crop != cfg.crop || old.seconds != cfg.seconds || old.maxmb != cfg.maxmb ||  // these two set auto bitrate
                   old.bitrate != cfg.bitrate || old.audiokbps != cfg.audiokbps || old.mono != cfg.mono;
    if (restart && running) stopRecording();
    SetDlgItemTextW(ui, ID_HKHINT, note.empty() ? kHotkeyHint : note.c_str());
    if (old.hotkey != cfg.hotkey) registerHotkey();
    watchGame();
}

static void scheduleApply() {
    if (loadingUi) return;
    SetTimer(ui, TIMER_APPLY, 600, nullptr);
}

static void browseFolder() {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS);
    ComPtr<IShellItem> it;
    PWSTR p;
    if (d->Show(ui) == S_OK && SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
        SetDlgItemTextW(ui, ID_FOLDER, p);
        CoTaskMemFree(p);
    }
}

static void showUi() {
    ShowWindow(ui, SW_SHOWNORMAL);
    SetForegroundWindow(ui);
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_HOTKEY:
        if (GetFocus() == item(ID_HOTKEY)) {  // user is re-binding: our own hotkey would swallow the keypress
            SendMessageW(item(ID_HOTKEY), HKM_SETHOTKEY, MAKEWORD(HIWORD(l), swapAltShift(LOWORD(l) & 7)), 0);
            scheduleApply();
            return 0;
        }
        saveClip();
        return 0;
    case WM_TIMER:
        if (w == TIMER_APPLY) {
            KillTimer(h, TIMER_APPLY);
            applyUi();
        } else {
            watchGame();
        }
        return 0;
    case WM_HSCROLL:  // volume sliders: live
        cfg.gamevol = SendMessageW(item(ID_V_GAME), TBM_GETPOS, 0, 0) / 100.0;
        cfg.discordvol = SendMessageW(item(ID_V_DISCORD), TBM_GETPOS, 0, 0) / 100.0;
        cfg.micvol = SendMessageW(item(ID_V_MIC), TBM_GETPOS, 0, 0) / 100.0;
        setGains();
        scheduleApply();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        switch (id) {
        case ID_SAVECLIP: saveClip(); return 0;
        case ID_OPENFOLDER: ShellExecuteW(nullptr, L"open", cfg.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0;
        case ID_REFRESH: refreshGames(); return 0;
        case ID_BROWSE: browseFolder(); return 0;
        }
        if (code == EN_CHANGE || code == BN_CLICKED || code == CBN_SELCHANGE || code == CBN_EDITCHANGE) {
            if (!loadingUi) updateEstimate();
            scheduleApply();
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        int id = GetDlgCtrlID((HWND)l);
        SetTextColor((HDC)w, C_TEXT);
        if (id == ID_HINT || id == ID_ESTIMATE) SetTextColor((HDC)w, C_MUTED);
        if (id == ID_HKHINT) SetTextColor((HDC)w, text(ID_HKHINT).rfind(L"⚠", 0) == 0 ? C_AMBER : C_MUTED);
        bool card = onCard((HWND)l);
        SetBkColor((HDC)w, card ? C_CARD : C_BG);
        return (LRESULT)(card ? brCard : brBg);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)w, C_TEXT);
        SetBkColor((HDC)w, C_CTRL);
        return (LRESULT)brCtrl;
    case WM_NOTIFY: {
        auto* cd = (NMCUSTOMDRAW*)l;
        if (cd->hdr.code != NM_CUSTOMDRAW) break;
        wchar_t cls[32];
        GetClassNameW(cd->hdr.hwndFrom, cls, 32);
        if (!_wcsicmp(cls, L"Button")) return drawButton(cd);
        if (!_wcsicmp(cls, TRACKBAR_CLASSW)) return drawSlider(cd);
        break;
    }
    case WM_MEASUREITEM:  // owner-drawn combos: closed field and list rows
        ((MEASUREITEMSTRUCT*)l)->itemHeight = ((MEASUREITEMSTRUCT*)l)->itemID == (UINT)-1 ? 18 : 22;
        return TRUE;
    case WM_DRAWITEM: {
        auto* di = (DRAWITEMSTRUCT*)l;
        HDC dc = di->hDC;
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        if (di->CtlType == ODT_STATIC) {  // status icon: red dot recording, amber dot on a problem, grey ring waiting
            // drawn 4x and scaled down with HALFTONE: GDI has no antialiasing
            const int k = 4, w = (di->rcItem.right - di->rcItem.left) * k, h = (di->rcItem.bottom - di->rcItem.top) * k, d = 16 * k;
            COLORREF c = !running ? C_MUTED : workerError || discordMissing ? C_AMBER : saving ? C_TEXT : C_RED;
            HDC m = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
            HGDIOBJ oldBmp = SelectObject(m, bmp);
            RECT all{0, 0, w, h};
            FillRect(m, &all, brBg);
            HPEN pen = CreatePen(PS_SOLID, 2 * k, c);
            SelectObject(m, pen);
            SelectObject(m, GetStockObject(DC_BRUSH));
            SetDCBrushColor(m, running ? c : C_BG);
            Ellipse(m, (w - d) / 2 + k, (h - d) / 2 + k, (w + d) / 2 - k, (h + d) / 2 - k);
            SetStretchBltMode(dc, HALFTONE);
            StretchBlt(dc, di->rcItem.left, di->rcItem.top, w / k, h / k, m, 0, 0, w, h, SRCCOPY);
            SelectObject(m, oldBmp);
            DeleteDC(m);
            DeleteObject(bmp);
            DeleteObject(pen);
            return TRUE;
        }
        SetDCBrushColor(dc, di->itemState & ODS_SELECTED ? C_RED : C_CTRL);
        FillRect(dc, &di->rcItem, (HBRUSH)GetStockObject(DC_BRUSH));
        wchar_t t[MAX_PATH] = L"";
        if ((int)di->itemID >= 0 && SendMessageW(di->hwndItem, CB_GETLBTEXTLEN, di->itemID, 0) < MAX_PATH)
            SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)t);
        RECT r = di->rcItem;
        r.left += 6;
        SelectObject(dc, uiFont);
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        return TRUE;
    }
    case WM_PAINT: {  // section cards behind the controls
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(DC_PEN));
        SelectObject(dc, uiBold);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_REDHOT);
        for (auto& c : cards) {
            box(dc, c.r, C_CARD, C_CARDLINE);
            box(dc, {c.r.left, c.r.top, c.r.left + 3, c.r.top + 24}, C_RED, C_RED);
            RECT t{c.r.left + 12, c.r.top + 5, c.r.right, c.r.top + 22};
            DrawTextW(dc, c.title, -1, &t, DT_SINGLELINE | DT_NOPREFIX);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case TRAY_MSG:
        if (LOWORD(l) == NIN_BALLOONUSERCLICK && !lastClip.empty())
            ShellExecuteW(nullptr, nullptr, L"explorer.exe", (L"/select,\"" + lastClip + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        if (LOWORD(l) == WM_LBUTTONUP) showUi();
        if (LOWORD(l) == WM_RBUTTONUP) {
            POINT p;
            GetCursorPos(&p);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, 0, 1, L"Settings...");
            AppendMenuW(menu, 0, 2, L"Save clip now");
            AppendMenuW(menu, 0, 3, L"Open clips folder");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, 0, 4, L"Exit");
            SetForegroundWindow(h);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) showUi();
            if (cmd == 2) saveClip();
            if (cmd == 3) ShellExecuteW(nullptr, L"open", cfg.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (cmd == 4) DestroyWindow(h);
        }
        return 0;
    case SHOW_MSG: showUi(); return 0;
    case STATUS_MSG: updateStatus(); return 0;
    case WM_CLOSE:  // keep running in the tray; flush a pending settings change first
        if (KillTimer(h, TIMER_APPLY)) applyUi();
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        if (KillTimer(h, TIMER_APPLY)) applyUi();
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int selftest() {
    UINT m, vk;
    assert(parseHotkey(L"F9", m, vk) && vk == VK_F9 && m == MOD_NOREPEAT);
    assert(parseHotkey(L"ctrl + Shift+s", m, vk) && vk == 'S' && m == (MOD_NOREPEAT | MOD_CONTROL | MOD_SHIFT));
    assert(parseHotkey(L"Alt+PageUp", m, vk) && vk == VK_PRIOR);
    assert(!parseHotkey(L"Ctrl", m, vk) && !parseHotkey(L"F25", m, vk) && !parseHotkey(L"Mouse4", m, vk));
    for (const wchar_t* s : {L"F9", L"Ctrl+Shift+S", L"Alt+PageUp", L"Ctrl+Alt+Shift+F12", L"7"}) {
        assert(parseHotkey(s, m, vk));
        std::wstring f = formatHotkey(m, vk);
        assert(!_wcsicmp(f.c_str(), s));
    }
    assert(formatHotkey(0, VK_LBUTTON).empty());
    assert(swapAltShift(MOD_ALT) == HOTKEYF_ALT && swapAltShift(HOTKEYF_SHIFT | HOTKEYF_CONTROL) == (MOD_SHIFT | MOD_CONTROL));
    std::vector<BYTE> au = {0, 0, 0, 1, 0x67, 0xAA, 0, 0, 0, 1, 0x68, 0xBB, 0, 0, 1, 0x65, 0xCC};
    std::vector<BYTE> want = {0, 0, 0, 1, 0x67, 0xAA, 0, 0, 0, 1, 0x68, 0xBB};
    assert(seqHeader(au) == want);
    // 30s at 19MB with audio on a 32:9 buffer -> 540p; 16:9 -> 720p; tiny budget floors at 360p
    UINT32 b = videoBitrate(30, 19, 128000);
    assert(pickHeight(b, 3840, 1080) == 540 && pickHeight(b, 1920, 1080) == 720);
    assert(pickHeight(300000, 1920, 1080) == 360 && pickHeight(b, 1280, 720) == 720);
    // estimates: auto squeezes to the cap; 8 Mbps + 128k audio for 20s is ~20.5 MB and 17s fits 19 MB
    Estimate e = estimate(30, 19, 0, 128000, 0, 1920, 1080);
    assert(e.mb == 19 && e.vbps == b && e.h == 720);
    e = estimate(20, 19, 8000, 128000, 0, 1920, 1080);
    assert(e.mb > 20.4 && e.mb < 20.7 && (int)e.secsFit == 17 && e.h == 1080);
    // no game window: whole monitor, or its centre 16:9 with crop on (monitor at a non-zero desktop origin)
    RECT mon{-5120, 0, 0, 1440}, r{};
    cfg.crop = false;
    captureRect(nullptr, mon, r);
    assert(r.left == 0 && r.right == 5120 && r.bottom == 1440);
    cfg.crop = true;
    captureRect(nullptr, mon, r);
    assert(r.left == 1280 && r.right == 3840 && r.top == 0 && r.bottom == 1440);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int) {
    if (wcsstr(cmd, L"--selftest")) return selftest();
    CreateMutexW(nullptr, TRUE, L"ClipperSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {  // already running: just bring up its window
        if (HWND w = FindWindowW(L"Clipper", nullptr)) PostMessageW(w, SHOW_MSG, 0, 0);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq = f.QuadPart;
    loadConfig();

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_HOTKEY_CLASS | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    uiBold = CreateFontIndirectW(&ncm.lfMessageFont);

    // undocumented uxtheme #135 SetPreferredAppMode(ForceDark): makes the tray menu dark; harmless if missing
    if (auto setAppMode = (int(WINAPI*)(int))GetProcAddress(LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32), MAKEINTRESOURCEA(135)))
        setAppMode(2);
    brBg = CreateSolidBrush(C_BG);
    brCard = CreateSolidBrush(C_CARD);
    brCtrl = CreateSolidBrush(C_CTRL);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"Clipper";
    wc.hbrBackground = brBg;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r{0, 0, 492, 566};
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    ui = CreateWindowW(L"Clipper", L"Clipper " CLIPPER_VERSION_W, style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2,
                       (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, nullptr, nullptr, hi, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(ui, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(ui, DWMWA_CAPTION_COLOR, &C_BG, sizeof(C_BG));  // Windows 11 only, ignored elsewhere
    buildUi();

    nid.hWnd = ui;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = TRAY_MSG;
    nid.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(nid.szTip, L"Clipper");
    Shell_NotifyIconW(NIM_ADD, &nid);
    loadUi();
    registerHotkey();
    watchGame();
    SetTimer(ui, TIMER_WATCH, 2000, nullptr);
    if (!wcsstr(cmd, L"--tray")) showUi();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(ui, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    stopRecording();
    if (saveThread.joinable()) saveThread.join();
    MFShutdown();
    return 0;
}
