#include <Windows.h>
#include <WindowsX.h>
#include <tchar.h>
#include "Util.h"
#include "DrawUtil.h"
#include "ThumbnailPreview.h"

namespace
{
const TCHAR THUMBNAIL_WINDOW_CLASS[] = TEXT("TvtPlay Thumbnail");
// シークバーとの間隔
const int ANCHOR_GAP = 4;
}

HINSTANCE CThumbnailPreview::s_hinst = nullptr;

CThumbnailPreview::CThumbnailPreview()
    : m_hwnd(nullptr)
    , m_hwndOwner(nullptr)
    , m_hfont(nullptr)
    , m_logFont()
    , m_crText(RGB(255, 255, 255))
    , m_crBk(RGB(0, 0, 0))
    , m_baseWidth(160)
    , m_cacheMax(128)
    , m_generation(0)
    , m_fUnsupported(false)
    , m_useCounter(0)
    , m_step(0)
    , m_requestedKey(-1)
    , m_width(0)
    , m_key(-1)
    , m_msec(0)
    , m_shownKey(-1)
    , m_anchorRect()
    , m_cursorX(0)
{
}

CThumbnailPreview::~CThumbnailPreview()
{
    Destroy();
}

bool CThumbnailPreview::Register(HINSTANCE hinst)
{
    s_hinst = hinst;
    WNDCLASS wc = {};
    wc.lpfnWndProc      = WndProc;
    wc.hInstance        = hinst;
    wc.hCursor          = ::LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName    = THUMBNAIL_WINDOW_CLASS;
    return ::RegisterClass(&wc) != 0;
}

void CThumbnailPreview::SetOptions(int width, int cacheMax)
{
    m_baseWidth = min(max(width, 64), 640);
    m_cacheMax = min(max(cacheMax, 1), 4096);
}

void CThumbnailPreview::Open(LPCTSTR path, HWND hwndNotify, UINT notifyMsg)
{
    Close();
    if (!CThumbnailGenerator::IsSupportedFile(path)) return;
    if (!m_generator.Start(hwndNotify, notifyMsg)) return;
    m_path = path;
    m_fUnsupported = false;
}

void CThumbnailPreview::Close()
{
    Hide();
    // ファイルを開いたままにしないようにスレッドごと止める
    m_generator.Stop();
    m_path.clear();
    ++m_generation;
    m_step = 0;
    m_requestedKey = -1;
    m_shownKey = -1;
    ClearCache();
}

void CThumbnailPreview::Show(HWND hwndStatus, const RECT &itemRect, int cursorX, int msec, int durMsec,
                             int dpi, const LOGFONT &font, COLORREF crText, COLORREF crBk)
{
    if (m_path.empty() || m_fUnsupported || durMsec <= 0) {
        Hide();
        return;
    }

    int width = ::MulDiv(m_baseWidth, dpi > 0 ? dpi : 96, 96);
    if (width != m_width) {
        // DPIが変わったら作り直す
        ClearCache();
        m_width = width;
        m_requestedKey = -1;
        m_shownKey = -1;
    }
    if (m_step <= 0) {
        // 長いファイルほど粗く区切ってキャッシュを効かせる
        m_step = max(durMsec / 1000, 1000);
    }
    m_msec = msec;
    m_key = KeyOf(msec, durMsec);
    const ENTRY *pEntry = FindEntry(m_key);
    if (pEntry) {
        m_shownKey = m_key;
    }
    else if (m_key != m_requestedKey) {
        m_generator.Request(m_generation, m_path.c_str(), m_key, durMsec, m_width);
        m_requestedKey = m_key;
    }

    HWND hwndOwner = ::GetAncestor(hwndStatus, GA_ROOT);
    if (m_hwnd && (!::IsWindow(m_hwnd) || m_hwndOwner != hwndOwner)) {
        // 全画面表示の切り替えなどでステータスバーの親が変わる
        if (::IsWindow(m_hwnd)) ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (!m_hwnd) {
        m_hwnd = ::CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, THUMBNAIL_WINDOW_CLASS, nullptr, WS_POPUP,
                                  0, 0, 0, 0, hwndOwner, nullptr, s_hinst, this);
        if (!m_hwnd) return;
        m_hwndOwner = hwndOwner;
    }
    if (!m_hfont || memcmp(&font, &m_logFont, sizeof(LOGFONT))) {
        if (m_hfont) ::DeleteObject(m_hfont);
        m_logFont = font;
        m_hfont = ::CreateFontIndirect(&font);
    }
    m_crText = crText;
    m_crBk = crBk;

    m_anchorRect = itemRect;
    ::MapWindowPoints(hwndStatus, nullptr, reinterpret_cast<POINT*>(&m_anchorRect), 2);
    POINT pt = {cursorX, 0};
    ::ClientToScreen(hwndStatus, &pt);
    m_cursorX = pt.x;
    Layout();
}

void CThumbnailPreview::Hide()
{
    if (m_hwnd && ::IsWindow(m_hwnd) && ::IsWindowVisible(m_hwnd)) {
        ::ShowWindow(m_hwnd, SW_HIDE);
    }
}

void CThumbnailPreview::Destroy()
{
    Close();
    if (m_hwnd && ::IsWindow(m_hwnd)) ::DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
    m_hwndOwner = nullptr;
    if (m_hfont) ::DeleteObject(m_hfont);
    m_hfont = nullptr;
}

// 生成スレッドから結果が届いた
void CThumbnailPreview::OnResult()
{
    std::unique_ptr<THUMBNAIL_IMAGE> image = m_generator.TakeResult();
    if (!image || image->generation != m_generation) return;
    if (image->status == 2) {
        // 暗号化されている、MPEG-2でもH.264でもないなど
        m_fUnsupported = true;
        Hide();
        return;
    }
    // DPIが変わる前の要求の結果は使わない
    if (image->status == 0 && image->width != m_width) return;
    ENTRY entry = {};
    if (image->status == 0) {
        void *pBits;
        entry.hbm = DrawUtil::CreateDIB(image->width, -image->height, 32, &pBits);
        if (entry.hbm) {
            memcpy(pBits, image->bits.data(), image->bits.size());
            entry.width = image->width;
            entry.height = image->height;
        }
    }
    // 作れなかった位置も覚えておき、何度も要求しないようにする
    AddEntry(image->msec, entry);
    if (image->msec == m_key && m_hwnd && ::IsWindowVisible(m_hwnd)) {
        m_shownKey = m_key;
        Layout();
    }
}

int CThumbnailPreview::KeyOf(int msec, int durMsec) const
{
    msec = min(max(msec, 0), durMsec);
    return msec / m_step * m_step;
}

const CThumbnailPreview::ENTRY *CThumbnailPreview::FindEntry(int key)
{
    std::map<int, ENTRY>::iterator it = m_cache.find(key);
    if (it == m_cache.end()) return nullptr;
    it->second.lastUsed = ++m_useCounter;
    return &it->second;
}

void CThumbnailPreview::AddEntry(int key, const ENTRY &entry)
{
    std::map<int, ENTRY>::iterator it = m_cache.find(key);
    if (it != m_cache.end()) {
        if (it->second.hbm) ::DeleteObject(it->second.hbm);
        m_cache.erase(it);
    }
    while (static_cast<int>(m_cache.size()) >= m_cacheMax) {
        // 表示中のもの以外で最も長く使われていないものを捨てる
        std::map<int, ENTRY>::iterator itOld = m_cache.end();
        for (it = m_cache.begin(); it != m_cache.end(); ++it) {
            if (it->first != m_shownKey && (itOld == m_cache.end() || it->second.lastUsed < itOld->second.lastUsed)) {
                itOld = it;
            }
        }
        if (itOld == m_cache.end()) break;
        if (itOld->second.hbm) ::DeleteObject(itOld->second.hbm);
        m_cache.erase(itOld);
    }
    ENTRY newEntry = entry;
    newEntry.lastUsed = ++m_useCounter;
    m_cache[key] = newEntry;
}

void CThumbnailPreview::ClearCache()
{
    for (std::map<int, ENTRY>::iterator it = m_cache.begin(); it != m_cache.end(); ++it) {
        if (it->second.hbm) ::DeleteObject(it->second.hbm);
    }
    m_cache.clear();
}

// 画像と時刻表示の大きさに合わせてシークバーの上に置く
void CThumbnailPreview::Layout()
{
    if (!m_hwnd) return;
    std::map<int, ENTRY>::const_iterator it = m_cache.find(m_shownKey);
    int imageHeight = it != m_cache.end() && it->second.hbm ? it->second.height : m_width * 9 / 16;

    int textHeight = 16;
    HDC hdc = ::GetDC(m_hwnd);
    if (hdc) {
        HFONT hfontOld = SelectFont(hdc, m_hfont);
        TEXTMETRIC tm;
        if (::GetTextMetrics(hdc, &tm)) textHeight = tm.tmHeight;
        SelectFont(hdc, hfontOld);
        ::ReleaseDC(m_hwnd, hdc);
    }
    int width = m_width + 2;
    int height = imageHeight + textHeight + 4 + 2;

    MONITORINFO mi = {sizeof(mi)};
    RECT rcMonitor = m_anchorRect;
    if (::GetMonitorInfo(::MonitorFromRect(&m_anchorRect, MONITOR_DEFAULTTONEAREST), &mi)) {
        rcMonitor = mi.rcMonitor;
    }
    int x = min(max(m_cursorX - width / 2, rcMonitor.left), rcMonitor.right - width);
    int y = m_anchorRect.top - ANCHOR_GAP - height;
    if (y < rcMonitor.top) {
        // 上に入りきらなければ下に出す
        y = m_anchorRect.bottom + ANCHOR_GAP;
    }
    ::SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CThumbnailPreview::Paint(HDC hdc)
{
    RECT rcClient;
    ::GetClientRect(m_hwnd, &rcClient);
    DrawUtil::Fill(hdc, &rcClient, MixColor(m_crText, m_crBk, 96));

    RECT rc = rcClient;
    ::InflateRect(&rc, -1, -1);
    DrawUtil::Fill(hdc, &rc, m_crBk);

    int textHeight = 16;
    HFONT hfontOld = SelectFont(hdc, m_hfont);
    TEXTMETRIC tm;
    if (::GetTextMetrics(hdc, &tm)) textHeight = tm.tmHeight;

    RECT rcImage = rc;
    rcImage.bottom = rc.bottom - textHeight - 4;
    std::map<int, ENTRY>::const_iterator it = m_cache.find(m_shownKey);
    if (it != m_cache.end() && it->second.hbm) {
        HDC hdcMem = ::CreateCompatibleDC(hdc);
        if (hdcMem) {
            HBITMAP hbmOld = SelectBitmap(hdcMem, it->second.hbm);
            ::SetStretchBltMode(hdc, HALFTONE);
            ::StretchBlt(hdc, rcImage.left, rcImage.top, rcImage.right - rcImage.left, rcImage.bottom - rcImage.top,
                         hdcMem, 0, 0, it->second.width, it->second.height, SRCCOPY);
            SelectBitmap(hdcMem, hbmOld);
            ::DeleteDC(hdcMem);
        }
    }
    else {
        DrawUtil::Fill(hdc, &rcImage, MixColor(m_crText, m_crBk, 24));
    }

    // シークバーと同じ書式で時刻を添える
    TCHAR text[32];
    int sec = m_msec / 1000;
    if (sec < 3600) _stprintf_s(text, TEXT("%02d:%02d"), sec / 60 % 60, sec % 60);
    else _stprintf_s(text, TEXT("%d:%02d:%02d"), sec / 60 / 60, sec / 60 % 60, sec % 60);
    RECT rcText = rc;
    rcText.top = rcImage.bottom;
    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, m_crText);
    ::DrawText(hdc, text, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectFont(hdc, hfontOld);
}

LRESULT CALLBACK CThumbnailPreview::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    CThumbnailPreview *pThis = reinterpret_cast<CThumbnailPreview*>(::GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_CREATE:
        {
            LPCREATESTRUCT pcs = reinterpret_cast<LPCREATESTRUCT>(lParam);
            ::SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pcs->lpCreateParams));
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        // カーソルの下に来ても下のウィンドウの操作を妨げない
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = ::BeginPaint(hwnd, &ps);
            if (pThis) pThis->Paint(hdc);
            ::EndPaint(hwnd, &ps);
        }
        return 0;
    case WM_DESTROY:
        if (pThis && pThis->m_hwnd == hwnd) {
            pThis->m_hwnd = nullptr;
            pThis->m_hwndOwner = nullptr;
        }
        ::SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return ::DefWindowProc(hwnd, uMsg, wParam, lParam);
}
