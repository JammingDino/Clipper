#include "clipper.h"

// ---------- misc ----------
std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    *wcsrchr(p, L'\\') = 0;
    return p;
}

void logf(const wchar_t* fmt, ...) {
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

// ---------- config ----------
Config cfg;

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
    "; Max frame rate. Only frames the screen actually shows are recorded (a game at 40 fps records 40 fps).\r\n"
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

std::wstring iniPath() { return exeDir() + L"\\clipper.ini"; }

void loadConfig() {
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

bool hasAudio(const wchar_t* src) { return cfg.audio.find(src) != std::wstring::npos; }
bool anyAudio() { return hasAudio(L"game") || hasAudio(L"desktop") || hasAudio(L"mic") || hasAudio(L"discord"); }

// video bitrate from the size cap: 5% headroom for container + rate-control wobble, minus the audio track
UINT32 videoBitrate(double seconds, double maxmb, UINT32 audioBps) {
    return (UINT32)std::max(300000.0, maxmb * 1e6 * 8 * 0.95 / seconds - audioBps);
}

// largest standard height with >= 3.6 bits per pixel per second (clean-looking H.264 for fast games).
// Frame rate is deliberately ignored: the bitrate is what the file size buys.
UINT pickHeight(UINT32 budget, UINT bufW, UINT bufH) {
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
Estimate estimate(int secs, double maxmb, int bitrateKbps, UINT32 audioBps, int height, UINT srcW, UINT srcH) {
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
std::wstring formatHotkey(UINT mods, UINT vk) {
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

UINT swapAltShift(UINT m) { return (m & 2) | (m & 1 ? 4 : 0) | (m & 4 ? 1 : 0); }  // MOD_* <-> HOTKEYF_*
bool parseHotkey(std::wstring s, UINT& mods, UINT& vk) {
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

