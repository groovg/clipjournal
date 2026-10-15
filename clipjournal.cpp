// clipjournal: lightweight clipboard history for Windows (text, links, images, files).
// Storage is a plain folder, one file per entry: <unix-ms>_<hash>[_pin].<txt|png|files>.

#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "uxtheme")
#pragma comment(lib, "shcore")
#pragma comment(lib, "gdiplus")
#pragma comment(lib, "ole32")
#pragma comment(lib, "advapi32")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

enum Kind { Text, Link, Image, Files };  // order mirrors kFilters[2..]
struct Item {
    fs::path path;
    std::wstring ext;
    long long ts = 0;
    uint64_t hash = 0;
    bool pinned = false;
    Kind kind = Text;
    std::wstring preview;                    // head of the content, for drawing and search
    std::unique_ptr<Gdiplus::Bitmap> thumb;  // images only, decoded on first draw
    int thumbH = 0;
};

const size_t kPreviewBytes = 4096;  // ponytail: search sees only the first 4 KB of each entry
const int kDrawChars = 300;
const int kW = 400, kH = 480;
const wchar_t* kFilters[] = {L"All", L"Pinned", L"Text", L"Links", L"Images", L"Files"};
const CLSID kPngClsid = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};  // GDI+ PNG encoder
const UINT WM_APP_SHOW = WM_APP + 1, WM_APP_TRAY = WM_APP + 2;
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const char kDefaultConfig[] =
    "; clipjournal settings. After editing: tray icon > Reload config.\r\n"
    "[clipjournal]\r\n"
    "; Hotkey: Ctrl, Shift, Alt, Win + a key (A-Z, 0-9, F1-F24, Space, Insert or a symbol).\r\n"
    "; Win+V takes over the built-in Windows clipboard history.\r\n"
    "hotkey=Win+V\r\n"
    "; How long unpinned items live: 30m, 12h, 7d, 4w. 0 = forever. Pinned items never expire.\r\n"
    "keep=30d\r\n"
    "; Separate lifetime for images (they are the heavy ones). Empty = same as keep.\r\n"
    "keep_images=7d\r\n"
    "; Max number of unpinned items. 0 = no limit.\r\n"
    "max_items=1000\r\n"
    "; Never record clipboard from these programs (exe names, comma-separated).\r\n"
    "ignore_apps=KeePass.exe,KeePassXC.exe,1Password.exe,Bitwarden.exe\r\n";

std::vector<Item> items;  // newest first
fs::path dataDir, itemsDir, cfgPath;
std::wstring exePath, hotkey;
long long keepMs, keepImagesMs;
int maxItems;
std::vector<std::wstring> ignoreApps;

HWND popup, edit, list, prevFg;
HFONT font;
HDC measureDC;
int lineH, filter;
UINT dpi = 96;
HBRUSH bgBrush, selBrush;
struct Colors { COLORREF bg, fg, sub, sel, accent; } col;
DWORD ownSeq;
UINT cfPng, cfExclude, cfIgnore, cfHistory, msgTaskbarCreated;
UINT hkMods, hkVk;
bool hkHeld;
HANDLE hookThread;
DWORD hookTid;

// ---------- helpers ----------

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
int S(int x) { return MulDiv(x, dpi, 96); }
int SearchH() { return S(44); }

std::string ToUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring FromUtf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}
std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, s.find_last_not_of(L" \t\r\n") - a + 1);
}
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t a = 0;
    for (size_t b; (b = s.find(sep, a)) != std::wstring::npos; a = b + 1) out.push_back(Trim(s.substr(a, b - a)));
    out.push_back(Trim(s.substr(a)));
    return out;
}

std::string ReadBytes(const fs::path& p, size_t max = SIZE_MAX) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::string s(std::min((size_t)f.tellg(), max), '\0');
    f.seekg(0);
    f.read(s.data(), (std::streamsize)s.size());
    return s;
}
bool WriteBytes(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f.write(s.data(), (std::streamsize)s.size());
    f.close();
    if (f) return true;
    std::error_code ec;
    fs::remove(p, ec);
    return false;
}

uint64_t Fnv1a(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

bool ParseHotkey(const std::wstring& s, UINT& mods, UINT& vk) {
    mods = vk = 0;
    for (const auto& t : Split(Lower(s), L'+')) {
        if (vk) return false;  // the key must come last
        if (t == L"ctrl" || t == L"control") mods |= MOD_CONTROL;
        else if (t == L"shift") mods |= MOD_SHIFT;
        else if (t == L"alt") mods |= MOD_ALT;
        else if (t == L"win") mods |= MOD_WIN;
        else if (t == L"space") vk = VK_SPACE;
        else if (t == L"insert" || t == L"ins") vk = VK_INSERT;
        else if (t.size() >= 2 && t.size() <= 3 && t[0] == L'f' && iswdigit(t[1])) {
            int n = _wtoi(t.c_str() + 1);
            if (n < 1 || n > 24) return false;
            vk = VK_F1 + n - 1;
        } else if (t.size() == 1 && LOBYTE(VkKeyScanW(t[0])) != 0xFF) vk = LOBYTE(VkKeyScanW(t[0]));
        else return false;
    }
    return vk != 0;
}

long long ParseDuration(const std::wstring& s) {
    wchar_t* end;
    double n = wcstod(s.c_str(), &end);
    while (*end == L' ') end++;
    long long unit = 86400000;  // bare number = days
    switch (towlower(*end)) {
        case L'm': unit = 60000; break;
        case L'h': unit = 3600000; break;
        case L'w': unit = 7 * 86400000LL; break;
    }
    return (long long)(n * unit);
}

bool IsLink(const std::wstring& s) {
    std::wstring t = Trim(s);
    return (StrCmpNIW(t.c_str(), L"http://", 7) == 0 || StrCmpNIW(t.c_str(), L"https://", 8) == 0) &&
           t.find_first_of(L" \t\r\n") == std::wstring::npos;
}

std::wstring ImageLabel(const std::string& png) {  // width/height sit in the PNG IHDR chunk
    if (png.size() < 24) return L"Image";
    auto be32 = [&](size_t o) {
        return (unsigned)(uint8_t)png[o] << 24 | (unsigned)(uint8_t)png[o + 1] << 16 |
               (unsigned)(uint8_t)png[o + 2] << 8 | (unsigned)(uint8_t)png[o + 3];
    };
    return L"Image " + std::to_wstring(be32(16)) + L"×" + std::to_wstring(be32(20));
}

std::wstring Ago(long long ts) {
    long long s = (NowMs() - ts) / 1000;
    if (s < 60) return L"now";
    if (s < 3600) return std::to_wstring(s / 60) + L"m";
    if (s < 86400) return std::to_wstring(s / 3600) + L"h";
    return std::to_wstring(s / 86400) + L"d";
}

// ---------- storage ----------

fs::path NameFor(const Item& it) {
    wchar_t b[64];
    swprintf_s(b, L"%lld_%016llx%s", it.ts, it.hash, it.pinned ? L"_pin" : L"");
    return itemsDir / (b + it.ext);
}

bool Rename(Item& it) {
    fs::path p = NameFor(it);
    std::error_code ec;
    fs::rename(it.path, p, ec);
    if (ec) return false;
    it.path = p;
    return true;
}

void FillMeta(Item& it, const std::string& head) {
    if (it.ext == L".png") {
        it.kind = Image;
        it.preview = ImageLabel(head);
        return;
    }
    std::wstring w = FromUtf8(head);
    std::erase(w, L'\r');
    std::replace(w.begin(), w.end(), L'\t', L' ');
    w.erase(0, w.find_first_not_of(L" \n"));
    it.preview = std::move(w);
    it.kind = it.ext == L".files" ? Files : IsLink(it.preview) ? Link : Text;
}

void Load() {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(itemsDir, ec)) {
        Item it;
        it.path = e.path();
        it.ext = it.path.extension().wstring();
        std::wstring stem = it.path.stem().wstring();
        if ((it.ext != L".txt" && it.ext != L".png" && it.ext != L".files") ||
            swscanf_s(stem.c_str(), L"%lld_%llx", &it.ts, &it.hash) != 2)
            continue;
        it.pinned = stem.ends_with(L"_pin");
        FillMeta(it, ReadBytes(it.path, kPreviewBytes));
        items.push_back(std::move(it));
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.ts > b.ts; });
}

void Prune() {
    long long now = NowMs();
    int kept = 0;
    std::error_code ec;
    std::erase_if(items, [&](const Item& it) {
        if (it.pinned) return false;
        long long keep = it.kind == Image ? keepImagesMs : keepMs;
        if ((keep > 0 && now - it.ts > keep) || (maxItems > 0 && kept >= maxItems)) {
            fs::remove(it.path, ec);
            return true;
        }
        kept++;
        return false;
    });
}

void MoveToTop(size_t i) {
    items[i].ts = NowMs();
    Rename(items[i]);
    std::rotate(items.begin(), items.begin() + i, items.begin() + i + 1);
}

void Refill(int sel);
void Changed() {
    if (IsWindowVisible(popup)) Refill(std::max(0, (int)SendMessageW(list, LB_GETCURSEL, 0, 0)));
}

void Add(std::string data, const wchar_t* ext) {
    uint64_t h = Fnv1a(data);
    for (size_t i = 0; i < items.size(); i++)
        if (items[i].hash == h && items[i].ext == ext) {  // copied again: just bump it
            MoveToTop(i);
            Changed();
            return;
        }
    Item it;
    it.ts = NowMs();
    it.hash = h;
    it.ext = ext;
    it.path = NameFor(it);
    if (!WriteBytes(it.path, data)) return;
    FillMeta(it, data.substr(0, kPreviewBytes));
    items.insert(items.begin(), std::move(it));
    Prune();
    Changed();
}

// ---------- clipboard ----------

bool OpenClip() {
    for (int i = 0; i < 10; i++) {  // another app may hold it for a moment
        if (OpenClipboard(popup)) return true;
        Sleep(20);
    }
    return false;
}

std::string EncodePng(HBITMAP hb) {
    std::string out;
    Gdiplus::Bitmap bmp(hb, nullptr);
    IStream* s = nullptr;
    if (bmp.GetLastStatus() != Gdiplus::Ok || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &s))) return out;
    STATSTG st;
    HGLOBAL g;
    if (bmp.Save(s, &kPngClsid, nullptr) == Gdiplus::Ok && SUCCEEDED(s->Stat(&st, STATFLAG_NONAME)) &&
        SUCCEEDED(GetHGlobalFromStream(s, &g))) {
        out.assign((const char*)GlobalLock(g), (size_t)st.cbSize.QuadPart);
        GlobalUnlock(g);
    }
    s->Release();
    return out;
}

bool FromIgnoredApp() {
    if (ignoreApps.empty()) return false;
    HWND w = GetClipboardOwner();
    if (!w) w = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(p, 0, buf, &n);
    CloseHandle(p);
    std::wstring exe = Lower(fs::path(buf).filename().wstring());
    return ok && std::find(ignoreApps.begin(), ignoreApps.end(), exe) != ignoreApps.end();
}

void Capture() {
    if (GetClipboardSequenceNumber() == ownSeq) return;  // our own paste
    // Password managers mark secrets with these formats.
    if (IsClipboardFormatAvailable(cfExclude) || IsClipboardFormatAvailable(cfIgnore) || FromIgnoredApp()) return;
    if (!OpenClip()) return;
    std::string data;
    const wchar_t* ext = L".txt";
    bool allowed = true;
    if (HANDLE g = GetClipboardData(cfHistory); g && GlobalSize(g) >= sizeof(DWORD)) {
        if (auto* p = (const DWORD*)GlobalLock(g)) {
            allowed = *p != 0;
            GlobalUnlock(g);
        }
    }
    if (!allowed) {
    } else if (HDROP h = (HDROP)GetClipboardData(CF_HDROP)) {
        std::wstring w;
        UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            std::wstring p(DragQueryFileW(h, i, nullptr, 0), L'\0');
            DragQueryFileW(h, i, p.data(), (UINT)p.size() + 1);
            w += (i ? L"\n" : L"") + p;
        }
        data = ToUtf8(w);
        ext = L".files";
    } else if (HANDLE t = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = (const wchar_t*)GlobalLock(t)) {
            std::wstring w(p, wcsnlen(p, GlobalSize(t) / sizeof(wchar_t)));
            GlobalUnlock(t);
            if (!Trim(w).empty()) data = ToUtf8(w);
        }
    } else if (HANDLE png = GetClipboardData(cfPng)) {  // browsers etc. give lossless PNG with alpha
        if (auto* p = (const char*)GlobalLock(png)) {
            data.assign(p, GlobalSize(png));
            GlobalUnlock(png);
        }
        ext = L".png";
    } else if (HBITMAP hb = (HBITMAP)GetClipboardData(CF_BITMAP)) {
        data = EncodePng(hb);  // ponytail: runs on the UI thread, a huge screenshot stalls the popup for ~0.5 s
        ext = L".png";
    }
    CloseClipboard();
    if (!data.empty()) Add(std::move(data), ext);
}

bool SetGlobal(UINT fmt, const void* p, size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return false;
    memcpy(GlobalLock(g), p, n);
    GlobalUnlock(g);
    if (SetClipboardData(fmt, g)) return true;
    GlobalFree(g);
    return false;
}

bool ToClipboard(const Item& it) {
    std::string data = ReadBytes(it.path);
    if (data.empty()) return false;
    HBITMAP hb = nullptr;
    if (it.kind == Image) {
        Gdiplus::Bitmap b(it.path.c_str());
        if (b.GetLastStatus() == Gdiplus::Ok) b.GetHBITMAP(Gdiplus::Color(255, 255, 255), &hb);
    }
    if (!OpenClip()) {
        if (hb) DeleteObject(hb);
        return false;
    }
    EmptyClipboard();
    bool ok;
    if (it.kind == Image) {
        ok = SetGlobal(cfPng, data.data(), data.size());
        if (hb) {
            if (SetClipboardData(CF_BITMAP, hb)) ok = true;  // Windows derives CF_DIB from it
            else DeleteObject(hb);
        }
    } else if (it.kind == Files) {
        std::wstring w = FromUtf8(data);
        std::replace(w.begin(), w.end(), L'\n', L'\0');
        w.append(2, L'\0');  // each path null-terminated, list ends with an extra null
        std::string buf(sizeof(DROPFILES), '\0');
        auto* df = (DROPFILES*)buf.data();
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        buf.append((const char*)w.data(), w.size() * sizeof(wchar_t));
        ok = SetGlobal(CF_HDROP, buf.data(), buf.size());
    } else {
        std::wstring w = FromUtf8(data);
        ok = SetGlobal(CF_UNICODETEXT, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    }
    CloseClipboard();
    ownSeq = GetClipboardSequenceNumber();
    return ok;
}

void SendCtrlV() {
    INPUT in[4] = {};
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[3].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = in[2].ki.wVk = 'V';
    in[2].ki.dwFlags = in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

// ---------- popup ----------

void ApplyTheme() {
    DWORD light = 1, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    BOOL dark = !light;
    col = dark ? Colors{RGB(32, 32, 32), RGB(230, 230, 230), RGB(150, 150, 150), RGB(58, 58, 58), RGB(96, 205, 255)}
               : Colors{RGB(249, 249, 249), RGB(26, 26, 26), RGB(110, 110, 110), RGB(224, 224, 224), RGB(0, 103, 192)};
    DeleteObject(bgBrush);
    DeleteObject(selBrush);
    bgBrush = CreateSolidBrush(col.bg);
    selBrush = CreateSolidBrush(col.sel);
    DwmSetWindowAttribute(popup, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    SetWindowTheme(list, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);  // dark scrollbar
}

void ApplyDpi(UINT d) {
    if (font && d == dpi) return;
    dpi = d;
    HFONT old = font;
    font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                       L"Segoe UI");
    SelectObject(measureDC, font);
    TEXTMETRICW tm;
    GetTextMetricsW(measureDC, &tm);
    lineH = tm.tmHeight;
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, FALSE);
    DeleteObject(old);
}

int TextWidth() { return S(kW) - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) - S(24) - S(56); }

int RowHeight(const Item& it) {
    int h = S(64);
    if (it.kind != Image) {
        RECT r{0, 0, TextWidth(), 0};
        DrawTextW(measureDC, it.preview.c_str(), (int)std::min<size_t>(it.preview.size(), kDrawChars), &r,
                  DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        h = std::clamp((int)r.bottom, lineH, 3 * lineH);
    }
    return std::min(255, h + S(16));  // listbox rows max out at 255 px
}

Gdiplus::Bitmap* Thumb(Item& it, int maxW) {
    int maxH = S(64);
    if (it.thumb && it.thumbH == maxH) return it.thumb.get();
    Gdiplus::Bitmap src(it.path.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok || !src.GetWidth() || !src.GetHeight()) return nullptr;
    double k = std::min({1.0, (double)maxH / src.GetHeight(), (double)maxW / src.GetWidth()});
    int w = std::max(1, (int)(src.GetWidth() * k)), h = std::max(1, (int)(src.GetHeight() * k));
    it.thumb = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(it.thumb.get());
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.DrawImage(&src, 0, 0, w, h);
    it.thumbH = maxH;
    return it.thumb.get();
}

void DrawRow(const DRAWITEMSTRUCT* d) {
    if (d->itemID == (UINT)-1) return;
    Item& it = items[d->itemData];
    HDC dc = d->hDC;
    FillRect(dc, &d->rcItem, (d->itemState & ODS_SELECTED) ? selBrush : bgBrush);
    RECT r = d->rcItem;
    InflateRect(&r, -S(12), -S(8));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font);
    std::wstring meta = (it.pinned ? L"★ " : L"") + Ago(it.ts);
    SetTextColor(dc, col.sub);
    DrawTextW(dc, meta.c_str(), -1, &r, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
    r.right -= S(56);
    if (it.kind == Image) {
        if (auto* t = Thumb(it, S(200))) {
            Gdiplus::Graphics(dc).DrawImage(t, (int)r.left, (int)r.top, (int)t->GetWidth(), (int)t->GetHeight());
            r.left += t->GetWidth() + S(10);
        }
        DrawTextW(dc, it.preview.c_str(), -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    } else {
        SetTextColor(dc, it.kind == Link ? col.accent : col.fg);
        DrawTextW(dc, it.preview.c_str(), (int)std::min<size_t>(it.preview.size(), kDrawChars), &r,
                  DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
}

bool Matches(const Item& it, const wchar_t* q) {
    if (filter == 1 && !it.pinned) return false;
    if (filter >= 2 && it.kind != filter - 2) return false;
    return !*q || StrStrIW(it.preview.c_str(), q);
}

void Refill(int sel) {
    wchar_t q[256];
    GetWindowTextW(edit, q, 256);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < items.size(); i++)
        if (Matches(items[i], q)) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)i);
    int n = (int)SendMessageW(list, LB_GETCOUNT, 0, 0);
    SendMessageW(list, LB_SETCURSEL, std::min(sel, n - 1), 0);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

void HidePopup() { ShowWindow(popup, SW_HIDE); }

void ShowPopup() {
    if (IsWindowVisible(popup)) {
        HidePopup();
        return;
    }
    prevFg = GetForegroundWindow();
    POINT pt;
    GUITHREADINFO gi{sizeof gi};
    if (GetGUIThreadInfo(GetWindowThreadProcessId(prevFg, nullptr), &gi) && gi.hwndCaret) {
        pt = {gi.rcCaret.left, gi.rcCaret.bottom};
        ClientToScreen(gi.hwndCaret, &pt);
    } else {
        GetCursorPos(&pt);
    }
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    UINT dx, dy;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    ApplyDpi(dx);
    ApplyTheme();

    int w = S(kW), h = S(kH), sh = SearchH();
    RECT wa = mi.rcWork;
    int x = std::max((int)wa.left, std::min((int)pt.x, (int)wa.right - w));
    int y = std::max((int)wa.top, std::min((int)pt.y, (int)wa.bottom - h));
    SetWindowPos(popup, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    SetWindowPos(edit, nullptr, S(12), (sh - lineH) / 2, w - S(112), lineH, SWP_NOZORDER);
    SetWindowPos(list, nullptr, 0, sh + 1, w, h - sh - 1, SWP_NOZORDER);
    filter = 0;
    SetWindowTextW(edit, L"");
    Refill(0);

    INPUT in{};
    in.type = INPUT_MOUSE;
    SendInput(1, &in, sizeof in);  // zero-length input lets a background process take focus
    ShowWindow(popup, SW_SHOW);
    SetForegroundWindow(popup);
    SetFocus(edit);
}

int SelectedItem(int* sel = nullptr) {  // index into items, or -1
    int s = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel) *sel = s;
    return s < 0 ? -1 : (int)SendMessageW(list, LB_GETITEMDATA, s, 0);
}

void PasteSelected(bool paste) {
    int i = SelectedItem();
    if (i < 0) return;
    if (!ToClipboard(items[i])) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    MoveToTop(i);
    HidePopup();
    if (paste && prevFg) {
        SetForegroundWindow(prevFg);
        SendCtrlV();  // ponytail: blocked by UIPI for elevated targets; the item is on the clipboard anyway
    }
}

void DeleteSelected() {
    int sel, i = SelectedItem(&sel);
    if (i < 0) return;
    std::error_code ec;
    fs::remove(items[i].path, ec);
    items.erase(items.begin() + i);
    Refill(sel);
}

void TogglePin() {
    int sel, i = SelectedItem(&sel);
    if (i < 0) return;
    items[i].pinned = !items[i].pinned;
    if (!Rename(items[i])) items[i].pinned = !items[i].pinned;
    Refill(sel);
}

void CycleFilter(int d) {
    int n = (int)std::size(kFilters);
    filter = (filter + d + n) % n;
    Refill(0);
    InvalidateRect(popup, nullptr, FALSE);
}

LRESULT CALLBACK KeysProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        switch (w) {
            case VK_RETURN: PasteSelected(!shift); return 0;
            case VK_ESCAPE: HidePopup(); return 0;
            case VK_TAB: CycleFilter(shift ? -1 : 1); return 0;
            case VK_DELETE: if (shift) { DeleteSelected(); return 0; } break;
            case 'P': if (ctrl) { TogglePin(); return 0; } break;
            case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
                if (h == edit) return SendMessageW(list, m, w, l);
                break;
        }
    }
    if (m == WM_CHAR && (w == L'\r' || w == 0x1B || w == L'\t' || w == 0x10)) return 0;  // handled above, no beep
    if (h == list && m == WM_CHAR) {  // typing while the list has focus goes to search
        SetFocus(edit);
        return SendMessageW(edit, m, w, l);
    }
    if (h == list && m == WM_LBUTTONUP) {
        LRESULT r = DefSubclassProc(h, m, w, l);
        if (!HIWORD(SendMessageW(list, LB_ITEMFROMPOINT, 0, l))) PasteSelected(true);
        return r;
    }
    return DefSubclassProc(h, m, w, l);
}

// ---------- hotkey ----------

UINT ModsDown() {
    UINT m = 0;
    if (GetAsyncKeyState(VK_CONTROL) < 0) m |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT) < 0) m |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_MENU) < 0) m |= MOD_ALT;
    if (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0) m |= MOD_WIN;
    return m;
}

LRESULT CALLBACK KbHook(int code, WPARAM wp, LPARAM lp) {
    auto* k = (KBDLLHOOKSTRUCT*)lp;
    if (code == HC_ACTION && k->vkCode == hkVk && !(k->flags & LLKHF_INJECTED)) {
        bool up = k->flags & LLKHF_UP;
        if (!up && (hkHeld || ModsDown() == hkMods)) {
            if (!hkHeld) {
                hkHeld = true;
                if (hkMods & MOD_WIN) {  // a dummy key stops Start from opening when Win is released
                    INPUT in[2] = {};
                    in[0].type = in[1].type = INPUT_KEYBOARD;
                    in[0].ki.wVk = in[1].ki.wVk = 0xE8;
                    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(2, in, sizeof(INPUT));
                }
                PostMessageW(popup, WM_APP_SHOW, 0, 0);
            }
            return 1;
        }
        if (up && hkHeld) {
            hkHeld = false;
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Own thread so a slow clipboard capture never stalls system-wide typing.
DWORD WINAPI HookMain(void*) {
    HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, KbHook, GetModuleHandleW(nullptr), 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {}
    UnhookWindowsHookEx(h);
    return 0;
}

void StopHook() {
    if (!hookThread) return;
    while (!PostThreadMessageW(hookTid, WM_QUIT, 0, 0)) Sleep(1);  // fails until the thread has a queue
    WaitForSingleObject(hookThread, INFINITE);
    CloseHandle(hookThread);
    hookThread = nullptr;
}

void ApplyHotkey() {
    UnregisterHotKey(popup, 1);
    StopHook();
    if (!ParseHotkey(hotkey, hkMods, hkVk)) {
        MessageBoxW(nullptr, (L"Can't parse hotkey \"" + hotkey + L"\".\nFix it in config.ini, then tray > Reload config.").c_str(),
                    L"clipjournal", MB_ICONWARNING);
        return;
    }
    // Taken already (Win+V belongs to Windows): grab it with a keyboard hook instead.
    if (!RegisterHotKey(popup, 1, hkMods | MOD_NOREPEAT, hkVk))
        hookThread = CreateThread(nullptr, 0, HookMain, nullptr, 0, &hookTid);
}

// ---------- config & tray ----------

std::wstring Ini(const wchar_t* key, const wchar_t* def) {
    wchar_t b[1024];
    GetPrivateProfileStringW(L"clipjournal", key, def, b, 1024, cfgPath.c_str());
    return b;
}

void ReadConfig() {
    if (!fs::exists(cfgPath)) WriteBytes(cfgPath, kDefaultConfig);
    hotkey = Ini(L"hotkey", L"Win+V");
    std::wstring keep = Ini(L"keep", L"30d"), keepImages = Ini(L"keep_images", L"");
    keepMs = ParseDuration(keep);
    keepImagesMs = ParseDuration(keepImages.empty() ? keep : keepImages);
    maxItems = _wtoi(Ini(L"max_items", L"1000").c_str());
    ignoreApps.clear();
    for (auto& a : Split(Lower(Ini(L"ignore_apps", L"")), L','))
        if (!a.empty()) ignoreApps.push_back(a);
}

void TrayIcon(DWORD msg) {
    NOTIFYICONDATAW n{sizeof n};
    n.hWnd = popup;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    n.uCallbackMessage = WM_APP_TRAY;
    n.hIcon = LoadIconW(nullptr, IDI_APPLICATION);  // ponytail: stock icon, add a .rc with a real one when it matters
    wcsncpy_s(n.szTip, (L"clipjournal: " + hotkey).c_str(), _TRUNCATE);
    Shell_NotifyIconW(msg, &n);
}

bool Autostart() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"clipjournal", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k)) return;
    std::wstring v = L"\"" + exePath + L"\"";
    if (on) RegSetValueExW(k, L"clipjournal", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    else RegDeleteValueW(k, L"clipjournal");
    RegCloseKey(k);
}

void TrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Open history");
    AppendMenuW(m, MF_STRING, 2, L"Edit config");
    AppendMenuW(m, MF_STRING, 3, L"Reload config");
    AppendMenuW(m, MF_STRING, 4, L"Open data folder");
    AppendMenuW(m, MF_STRING | (Autostart() ? MF_CHECKED : 0), 5, L"Start with Windows");
    AppendMenuW(m, MF_STRING, 6, L"Clear history (keeps pinned)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 7, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(popup);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, popup, nullptr);
    PostMessageW(popup, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case 1: ShowPopup(); break;
        case 2: ShellExecuteW(nullptr, L"open", cfgPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case 3: ReadConfig(); ApplyHotkey(); Prune(); TrayIcon(NIM_MODIFY); break;
        case 4: ShellExecuteW(nullptr, L"open", dataDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case 5: SetAutostart(!Autostart()); break;
        case 6:
            if (MessageBoxW(nullptr, L"Delete all unpinned items?", L"clipjournal", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                std::error_code ec;
                std::erase_if(items, [&](const Item& it) { return !it.pinned && (fs::remove(it.path, ec), true); });
            }
            break;
        case 7: DestroyWindow(popup); break;
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CLIPBOARDUPDATE: Capture(); return 0;
        case WM_HOTKEY: case WM_APP_SHOW: ShowPopup(); return 0;
        case WM_TIMER: Prune(); Changed(); return 0;
        case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) HidePopup(); break;
        case WM_COMMAND: if (HIWORD(w) == EN_CHANGE && IsWindowVisible(h)) Refill(0); return 0;
        case WM_MEASUREITEM: {
            auto* mi = (MEASUREITEMSTRUCT*)l;
            mi->itemHeight = RowHeight(items[mi->itemData]);
            return TRUE;
        }
        case WM_DRAWITEM: DrawRow((const DRAWITEMSTRUCT*)l); return TRUE;
        case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
            SetTextColor((HDC)w, col.fg);
            SetBkColor((HDC)w, col.bg);
            return (LRESULT)bgBrush;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {  // search bar background, separator, current filter
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(dc, &rc, bgBrush);
            RECT line{0, SearchH(), rc.right, SearchH() + 1};
            FillRect(dc, &line, selBrush);
            RECT fr{0, 0, rc.right - S(12), SearchH()};
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, col.sub);
            SelectObject(dc, font);
            std::wstring f = std::wstring(L"Tab: ") + kFilters[filter];
            DrawTextW(dc, f.c_str(), -1, &fr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_APP_TRAY:
            if (LOWORD(l) == WM_LBUTTONUP) ShowPopup();
            else if (LOWORD(l) == WM_RBUTTONUP) TrayMenu();
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    if (m == msgTaskbarCreated) TrayIcon(NIM_ADD);  // Explorer restarted
    return DefWindowProcW(h, m, w, l);
}

// clipjournal.exe --selftest  -> exit code 0, or the failing line
int SelfTest() {
#define CHECK(x) if (!(x)) return __LINE__
    UINT m, v;
    CHECK(ParseHotkey(L"Win+V", m, v) && m == MOD_WIN && v == 'V');
    CHECK(ParseHotkey(L" ctrl + Shift + F12 ", m, v) && m == (MOD_CONTROL | MOD_SHIFT) && v == VK_F12);
    CHECK(ParseHotkey(L"Alt+Space", m, v) && m == MOD_ALT && v == VK_SPACE);
    CHECK(!ParseHotkey(L"Ctrl+", m, v) && !ParseHotkey(L"Hyper+V", m, v) && !ParseHotkey(L"V+Ctrl", m, v));
    CHECK(!ParseHotkey(L"F25", m, v) && !ParseHotkey(L"Ctrl+Shift", m, v));
    CHECK(ParseDuration(L"30m") == 30 * 60000LL && ParseDuration(L"12 h") == 12 * 3600000LL);
    CHECK(ParseDuration(L"7") == 7 * 86400000LL && ParseDuration(L"2w") == 14 * 86400000LL && ParseDuration(L"0") == 0);
    CHECK(IsLink(L"https://example.com/a?b=1\n") && IsLink(L"HTTP://x.y"));
    CHECK(!IsLink(L"see https://example.com") && !IsLink(L"https://a b") && !IsLink(L"ftp://x"));
    CHECK(ImageLabel(std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\x07\x80\0\0\x04\x38", 24)) == L"Image 1920×1080");
    CHECK(Split(L"a, b ,,c", L',').size() == 4 && Split(L"a, b ,,c", L',')[1] == L"b");
    return 0;
#undef CHECK
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int) {
    if (wcsstr(cmd, L"--selftest")) return SelfTest();
    CreateMutexW(nullptr, FALSE, L"clipjournal.single-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    exePath = buf;
    fs::path exeDir = fs::path(exePath).parent_path();
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    // Portable mode: a config.ini next to the exe keeps everything in that folder.
    dataDir = fs::exists(exeDir / L"config.ini") ? exeDir : fs::path(buf) / L"clipjournal";
    itemsDir = dataDir / L"items";
    cfgPath = dataDir / L"config.ini";
    std::error_code ec;
    fs::create_directories(itemsDir, ec);

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdip;
    Gdiplus::GdiplusStartup(&gdip, &gsi, nullptr);
    cfPng = RegisterClipboardFormatW(L"PNG");
    cfExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    cfIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    cfHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"clipjournal";
    RegisterClassW(&wc);
    popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"clipjournal", L"clipjournal", WS_POPUP | WS_CLIPCHILDREN,
                            0, 0, 0, 0, nullptr, nullptr, hi, nullptr);
    edit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, popup, nullptr, hi, nullptr);
    list = CreateWindowExW(0, L"LISTBOX", nullptr,
                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_OWNERDRAWVARIABLE | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                           0, 0, 0, 0, popup, nullptr, hi, nullptr);
    SetWindowSubclass(edit, KeysProc, 0, 0);
    SetWindowSubclass(list, KeysProc, 0, 0);
    SendMessageW(edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search…");
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(popup, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    measureDC = CreateCompatibleDC(nullptr);
    ApplyDpi(GetDpiForSystem());
    ApplyTheme();

    ReadConfig();
    Load();
    Prune();
    AddClipboardFormatListener(popup);
    TrayIcon(NIM_ADD);
    ApplyHotkey();
    SetTimer(popup, 1, 60 * 1000, nullptr);  // expiry check

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopHook();
    TrayIcon(NIM_DELETE);
    items.clear();  // thumbnails must go before GDI+ shuts down
    Gdiplus::GdiplusShutdown(gdip);
    return 0;
}
