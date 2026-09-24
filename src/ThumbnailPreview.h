#ifndef INCLUDE_THUMBNAIL_PREVIEW_H
#define INCLUDE_THUMBNAIL_PREVIEW_H

#include <map>
#include <string>
#include "ThumbnailGenerator.h"

// シークバーの上にサムネイルをポップアップ表示する
class CThumbnailPreview
{
public:
    CThumbnailPreview();
    ~CThumbnailPreview();
    static bool Register(HINSTANCE hinst);
    void SetOptions(int width, int cacheMax);
    void Open(LPCTSTR path, HWND hwndNotify, UINT notifyMsg);
    void Close();
    void Show(HWND hwndStatus, const RECT &itemRect, int cursorX, int msec, int durMsec,
              int dpi, const LOGFONT &font, COLORREF crText, COLORREF crBk);
    void Hide();
    void Destroy();
    void OnResult();
private:
    struct ENTRY {
        HBITMAP hbm;
        int width;
        int height;
        DWORD lastUsed;
    };
    int KeyOf(int msec, int durMsec) const;
    const ENTRY *FindEntry(int key);
    void AddEntry(int key, const ENTRY &entry);
    void ClearCache();
    void Layout();
    void Paint(HDC hdc);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    static HINSTANCE s_hinst;
    CThumbnailGenerator m_generator;
    HWND m_hwnd;
    HWND m_hwndOwner;
    HFONT m_hfont;
    LOGFONT m_logFont;
    COLORREF m_crText, m_crBk;
    int m_baseWidth, m_cacheMax;
    std::basic_string<TCHAR> m_path;
    int m_generation;
    bool m_fUnsupported;
    std::map<int, ENTRY> m_cache;
    DWORD m_useCounter;
    int m_step;
    int m_requestedKey;
    int m_width;
    int m_key, m_msec;
    // 表示する画像がまだないときは直前の画像を出しておく
    int m_shownKey;
    RECT m_anchorRect;
    int m_cursorX;
};

#endif // INCLUDE_THUMBNAIL_PREVIEW_H
