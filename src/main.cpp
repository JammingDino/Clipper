#include "clipper.h"

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

    createUi(hi, !wcsstr(cmd, L"--tray"));

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
