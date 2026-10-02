#include "clipper.h"

// ---------- clock: everything is 100ns units since recording start ----------
LONGLONG qpcFreq, startHns;
std::atomic<int> droppedFrames;  // capture ticks that produced no frame (encoder busy / thread stalled)
static LONGLONG qpcToHns(LONGLONG q) { return q / qpcFreq * 10000000 + q % qpcFreq * 10000000 / qpcFreq; }
LONGLONG nowHns() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return qpcToHns(q.QuadPart) - startHns;
}

// ---------- replay buffer ----------
std::mutex bufMx;
std::deque<Packet> vBuf, aBuf;
ComPtr<IMFMediaType> vType, aType;
std::atomic<bool> running;

void storeSample(IMFSample* s, std::deque<Packet>& q) {
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
    if (&q == &vBuf && !q.empty() && p.t > q.back().t) q.back().dur = p.t - q.back().t;
    q.push_back(std::move(p));
    LONGLONG keep = (cfg.seconds + 5) * 10000000LL;
    while (q.size() > 1 && q.back().t - q.front().t > keep) q.pop_front();
}


// ---------- recording lifecycle ----------
static std::vector<std::thread> workers;
static DWORD gamePid;
std::atomic<bool> workerError;
bool discordMissing;

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

void startRecording(HMONITOR mon, DWORD pid, HWND win) {
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
    resetAudio();
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

void stopRecording() {
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
std::vector<std::wstring> windowedApps() {
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

void watchGame() {
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
