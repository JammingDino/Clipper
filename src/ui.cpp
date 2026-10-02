#include "clipper.h"

HWND ui;
static NOTIFYICONDATAW nid{sizeof(nid)};

void notify(const std::wstring& title, const std::wstring& text, DWORD icon) {
    NOTIFYICONDATAW n = nid;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    n.dwInfoFlags = icon | NIIF_NOSOUND;  // our own beep is the audible cue (balloons are muted in fullscreen games)
    Shell_NotifyIconW(NIM_MODIFY, &n);
}
void fail(const std::wstring& title, const std::wstring& text) {
    MessageBeep(MB_ICONHAND);
    notify(title, text, NIIF_WARNING);
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

void updateStatus() {
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

void createUi(HINSTANCE hi, bool show) {
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
    if (show) showUi();
}
