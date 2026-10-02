#pragma once
// Clipper: tiny replay-buffer game clipper. A hotkey saves the last N seconds as an MP4 under a size cap.
// Pipeline: Desktop Duplication -> GPU scale/convert (D3D11 video processor) -> hardware H.264 (Media Foundation)
//           WASAPI (game-only process loopback / desktop / mic) -> mixer -> AAC
// Encoded packets sit in RAM; saving re-encodes to fit the size cap. Build: build.bat
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

#define CHECK(x)                                                                 \
    do {                                                                         \
        HRESULT hr_ = (x);                                                       \
        if (FAILED(hr_)) {                                                       \
            logf(L"%hs:%d: %hs failed 0x%08X", __FILE__, __LINE__, #x, (unsigned)hr_);\
            return false;                                                        \
        }                                                                        \
    } while (0)

// ---------- config.cpp: settings, logging, sizing math ----------
struct Config {
    std::wstring hotkey, game, audio, folder;
    int seconds, height, fps;  // height 0 = auto
    int bitrate, audiokbps;    // video kbps while recording, 0 = auto from maxmb
    bool crop, mono;
    double maxmb, gamevol, micvol, discordvol;
};
extern Config cfg;
std::wstring exeDir();
void logf(const wchar_t* fmt, ...);
std::wstring iniPath();
void loadConfig();
bool hasAudio(const wchar_t* src);
bool anyAudio();
UINT32 videoBitrate(double seconds, double maxmb, UINT32 audioBps);
UINT pickHeight(UINT32 budget, UINT bufW, UINT bufH);
struct Estimate {
    double mb, secsFit;
    UINT32 vbps;
    UINT h;
};
Estimate estimate(int secs, double maxmb, int bitrateKbps, UINT32 audioBps, int height, UINT srcW, UINT srcH);
std::wstring formatHotkey(UINT mods, UINT vk);
bool parseHotkey(std::wstring s, UINT& mods, UINT& vk);
UINT swapAltShift(UINT m);  // MOD_* <-> HOTKEYF_* (the hotkey control swaps Alt and Shift)

// ---------- recorder.cpp: clock (100ns units since recording start), replay buffer, lifecycle ----------
extern LONGLONG qpcFreq, startHns;
LONGLONG nowHns();
extern std::atomic<int> droppedFrames;  // capture ticks that produced no frame (encoder busy / thread stalled)
struct Packet {
    LONGLONG t = 0, dur = 0;
    bool key = false;
    std::vector<BYTE> data;
};
extern std::mutex bufMx;
extern std::deque<Packet> vBuf, aBuf;
extern ComPtr<IMFMediaType> vType, aType;
extern std::atomic<bool> running, workerError;
extern bool discordMissing;
void storeSample(IMFSample* s, std::deque<Packet>& q);
void startRecording(HMONITOR mon, DWORD pid, HWND win);
void stopRecording();
void watchGame();
std::vector<std::wstring> windowedApps();

// ---------- video.cpp: desktop duplication -> GPU convert -> hardware H.264 ----------
extern std::atomic<bool> resized;  // capture area changed size: the watcher restarts recording at the new size
void captureRect(HWND w, const RECT& mon, RECT& r);
bool videoLoop(HMONITOR mon, HWND win);

// ---------- audio.cpp: WASAPI sources -> mixer -> AAC ----------
bool captureLoop(int si, DWORD pid);
bool audioLoop();
void setGains();
void resetAudio();

// ---------- save.cpp: MP4 writing and fitting clips under the size cap ----------
extern std::wstring lastClip;
extern std::atomic<bool> saving;
extern std::thread saveThread;
std::vector<BYTE> seqHeader(const std::vector<BYTE>& d);  // SPS+PPS from an Annex-B keyframe
void saveClip();

// ---------- ui.cpp: settings window, tray icon, notifications ----------
enum { TRAY_MSG = WM_APP, SHOW_MSG = WM_APP + 1, STATUS_MSG = WM_APP + 2 };
extern HWND ui;
void createUi(HINSTANCE hi, bool show);
void updateStatus();
void notify(const std::wstring& title, const std::wstring& text, DWORD icon);
void fail(const std::wstring& title, const std::wstring& text);
