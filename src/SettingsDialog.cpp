#include <Windows.h>
#include <objbase.h>
#include <WindowsX.h>
#include <CommCtrl.h>
#include <CommDlg.h>
#include <tchar.h>
#include <string>
#include <vector>
#include "Util.h"
#define TVTEST_PLUGIN_VERSION TVTEST_PLUGIN_VERSION_(0,0,14)
#include "TVTestPlugin.h"
#include "resource.h"
#include "SettingsDialog.h"

namespace
{
// 各タブのコントロールはIDの百の位でタブが決まる(1100番台が全般、1200番台がシークバー、…)
const int TAB_ID_BASE = 1100;
const LPCTSTR TAB_NAMES[] = {
    TEXT("全般"), TEXT("シークバー"), TEXT("シーク・倍速"), TEXT("チャプター"), TEXT("MP4"), TEXT("MMTS"), TEXT("詳細"),
};

struct COMBO_ITEM {
    int value;
    LPCTSTR text;
};
const COMBO_ITEM SEEK_MODE_ITEMS[] = {
    {0, TEXT("クリックした位置にすぐシークする")},
    {1, TEXT("ドラッグして離した位置にシークする")},
    {2, TEXT("ドラッグ中も追いかけてシークする")},
};
const COMBO_ITEM STRETCH_MODE_ITEMS[] = {
    {1, TEXT("再生速度だけ調整する (速さに応じて音が高くなる)")},
    {2, TEXT("ピッチだけ上げ下げする")},
    {3, TEXT("ピッチと再生速度の両方を調整する")},
};
const COMBO_ITEM RESET_MODE_ITEMS[] = {
    {0, TEXT("ビューアをリセットする")},
    {1, TEXT("全体をリセットする")},
    {2, TEXT("空のPATを送ってビューアをリセットする")},
    {3, TEXT("空のPATを送る")},
    {4, TEXT("何もしない")},
};
const COMBO_ITEM THREAD_PRIORITY_ITEMS[] = {
    {THREAD_PRIORITY_LOWEST, TEXT("最低")},
    {THREAD_PRIORITY_BELOW_NORMAL, TEXT("低い")},
    {THREAD_PRIORITY_NORMAL, TEXT("通常")},
    {THREAD_PRIORITY_ABOVE_NORMAL, TEXT("高い")},
    {THREAD_PRIORITY_HIGHEST, TEXT("最高")},
    {THREAD_PRIORITY_TIME_CRITICAL, TEXT("リアルタイム")},
};
const COMBO_ITEM MOD_TIMESTAMP_ITEMS[] = {
    {0, TEXT("回避しない")},
    {1, TEXT("ファイルの読み込みで修正する")},
    {2, TEXT("ストリームコールバックで修正する")},
};

// ファイルを開くときや起動時に読まれるので、ダイアログがiniを直接読み書きする設定
struct INI_ITEM {
    int id;
    LPCTSTR section;
    LPCTSTR key;
    // チェックボックスなら0か1、それ以外は文字列
    LPCTSTR defaultValue;
    bool fCheck;
};
const INI_ITEM INI_ITEMS[] = {
    {IDC_S_CMD_OPTION, TEXT("Settings"), TEXT("TvtpCmdOption"), TEXT(""), false},
    {IDC_S_MP4_ENABLED, TEXT("MP4"), TEXT("Enabled"), TEXT("1"), true},
    {IDC_S_MP4_CHAPTER_TRACK, TEXT("MP4"), TEXT("LoadChapterTrack"), TEXT("1"), true},
    {IDC_S_MP4_CHECK_ATTRIBUTES, TEXT("MP4"), TEXT("CheckFileAttributes"), TEXT("1"), true},
    {IDC_S_MP4_VTT, TEXT("MP4"), TEXT("VttExtension"), TEXT(".vtt"), false},
    {IDC_S_MP4_PSI, TEXT("MP4"), TEXT("PsiDataExtension"), TEXT(".psc"), false},
    {IDC_S_MP4_PROGRAM_TEXT, TEXT("MP4"), TEXT("ProgramTextExtension"), TEXT(".program.txt"), false},
    {IDC_S_MP4_META, TEXT("MP4"), TEXT("Meta"), TEXT("metadata.ini"), false},
    {IDC_S_MP4_BROADCAST_ID, TEXT("MP4"), TEXT("BroadcastID"), TEXT("0x000100020003"), false},
    {IDC_S_MP4_TIME, TEXT("MP4"), TEXT("Time"), TEXT(""), false},
    {IDC_S_MP4_CHAPTER_CUT_SEC, TEXT("MP4"), TEXT("ChapterCutSec"), TEXT("_cut=*s"), false},
    {IDC_S_MP4_CHAPTER_CUT_MSEC, TEXT("MP4"), TEXT("ChapterCutMsec"), TEXT("_cut=*ms"), false},
    {IDC_S_MMTS_ENABLED, TEXT("MMTS"), TEXT("Enabled"), TEXT("1"), true},
    {IDC_S_MMTS_GAIJI, TEXT("MMTS"), TEXT("ConvertResolutionGaiji"), TEXT("1"), true},
    {IDC_S_MMTS_USE_SMART_CARD, TEXT("MMTS"), TEXT("UseSmartCard"), TEXT("0"), true},
    {IDC_S_MMTS_READER, TEXT("MMTS"), TEXT("SmartCardReaderName"), TEXT(""), false},
    {IDC_S_MMTS_PROXY, TEXT("MMTS"), TEXT("CasProxyServer"), TEXT(""), false},
    {IDC_S_MMTS_WINSCARD, TEXT("MMTS"), TEXT("CustomWinscardDLL"), TEXT(""), false},
};
// 読み込む側はVersionが古いと既定値を書き戻すので、書き込むときは今のVersionにそろえる
// ([MMTS]はVersion 2未満だとカードリーダーなどの設定を空にしてしまう)
const struct {
    LPCTSTR section;
    int version;
} INI_SECTION_VERSIONS[] = {
    {TEXT("MP4"), 1},
    {TEXT("MMTS"), 3},
};

struct DIALOG_CONTEXT {
    TVTPLAY_SETTINGS *pSettings;
    LPCTSTR iniFileName;
};

void InitCombo(HWND hDlg, int id, const COMBO_ITEM *items, int count, int value)
{
    HWND hwnd = ::GetDlgItem(hDlg, id);
    int sel = -1;
    for (int i = 0; i < count; ++i) {
        int index = ComboBox_AddString(hwnd, items[i].text);
        ComboBox_SetItemData(hwnd, index, items[i].value);
        if (items[i].value == value) sel = index;
    }
    if (sel < 0) {
        // iniに一覧にない値が書かれていても消さずに選べるようにする
        TCHAR text[32];
        _stprintf_s(text, TEXT("その他 (%d)"), value);
        sel = ComboBox_AddString(hwnd, text);
        ComboBox_SetItemData(hwnd, sel, value);
    }
    ComboBox_SetCurSel(hwnd, sel);
}

int GetCombo(HWND hDlg, int id, int defaultValue)
{
    HWND hwnd = ::GetDlgItem(hDlg, id);
    int sel = ComboBox_GetCurSel(hwnd);
    return sel < 0 ? defaultValue : static_cast<int>(ComboBox_GetItemData(hwnd, sel));
}

void SetCheck(HWND hDlg, int id, bool fCheck)
{
    ::CheckDlgButton(hDlg, id, fCheck ? BST_CHECKED : BST_UNCHECKED);
}

bool GetCheck(HWND hDlg, int id)
{
    return ::IsDlgButtonChecked(hDlg, id) == BST_CHECKED;
}

void SetInt(HWND hDlg, int id, int value)
{
    TCHAR text[16];
    _stprintf_s(text, TEXT("%d"), value);
    ::SetDlgItemText(hDlg, id, text);
}

// 数値として読めなければ元の値のままにする
int GetInt(HWND hDlg, int id, int defaultValue)
{
    TCHAR text[16];
    ::GetDlgItemText(hDlg, id, text, _countof(text));
    LPTSTR end;
    long value = _tcstol(text, &end, 10);
    return end == text ? defaultValue : static_cast<int>(value);
}

std::basic_string<TCHAR> GetText(HWND hDlg, int id)
{
    HWND hwnd = ::GetDlgItem(hDlg, id);
    std::vector<TCHAR> text(::GetWindowTextLength(hwnd) + 1);
    ::GetWindowText(hwnd, text.data(), static_cast<int>(text.size()));
    return text.data();
}

template<size_t N>
void GetText(HWND hDlg, int id, TCHAR (&buf)[N])
{
    ::GetDlgItemText(hDlg, id, buf, N);
}

// シーク量や倍速の並びは "60000, 30000, …" のようにカンマ区切りで編集する
void SetList(HWND hDlg, int id, const int *list, int num)
{
    std::basic_string<TCHAR> text;
    for (int i = 0; i < num; ++i) {
        TCHAR value[16];
        _stprintf_s(value, TEXT("%s%d"), i ? TEXT(", ") : TEXT(""), list[i]);
        text += value;
    }
    ::SetDlgItemText(hDlg, id, text.c_str());
}

// 0は並びの終わりを表すので読み飛ばす
int GetList(HWND hDlg, int id, int *list, int minValue, int maxValue)
{
    std::basic_string<TCHAR> text = GetText(hDlg, id);
    int num = 0;
    for (LPCTSTR p = text.c_str(); *p && num < TVTPLAY_SETTINGS::LIST_MAX;) {
        LPTSTR end;
        long value = _tcstol(p, &end, 10);
        if (end == p) {
            ++p;
            continue;
        }
        p = end;
        if (value) list[num++] = min(max(static_cast<int>(value), minValue), maxValue);
    }
    return num;
}

void SetButtonList(HWND hDlg, const TVTPLAY_SETTINGS &settings)
{
    std::basic_string<TCHAR> text;
    for (int i = 0; i < TVTPLAY_SETTINGS::BUTTON_MAX; ++i) {
        if (i) text += TEXT("\r\n");
        text += settings.buttonList[i];
    }
    ::SetDlgItemText(hDlg, IDC_S_BUTTON_LIST, text.c_str());
}

// 1行が1つのボタン。行が足りなければ残りのボタンは空(消える)になる
void GetButtonList(HWND hDlg, TVTPLAY_SETTINGS &settings)
{
    std::basic_string<TCHAR> text = GetText(hDlg, IDC_S_BUTTON_LIST);
    size_t pos = 0;
    for (int i = 0; i < TVTPLAY_SETTINGS::BUTTON_MAX; ++i) {
        size_t end = pos < text.size() ? text.find(TEXT('\n'), pos) : pos;
        std::basic_string<TCHAR> line = pos < text.size() ? text.substr(pos, end == std::basic_string<TCHAR>::npos ? end : end - pos) : TEXT("");
        if (!line.empty() && line.back() == TEXT('\r')) line.pop_back();
        _tcsncpy_s(settings.buttonList[i], line.c_str(), _TRUNCATE);
        pos = end == std::basic_string<TCHAR>::npos ? text.size() : end + 1;
    }
}

void LoadIniItems(HWND hDlg, LPCTSTR iniFileName)
{
    for (const INI_ITEM &item : INI_ITEMS) {
        TCHAR value[MAX_PATH];
        ::GetPrivateProfileString(item.section, item.key, item.defaultValue, value, _countof(value), iniFileName);
        if (item.fCheck) {
            SetCheck(hDlg, item.id, _tcstol(value, nullptr, 10) != 0);
        }
        else {
            ::SetDlgItemText(hDlg, item.id, value);
        }
    }
}

void SaveIniItems(HWND hDlg, LPCTSTR iniFileName)
{
    for (const auto &section : INI_SECTION_VERSIONS) {
        if (static_cast<int>(::GetPrivateProfileInt(section.section, TEXT("Version"), 0, iniFileName)) < section.version) {
            WritePrivateProfileInt(section.section, TEXT("Version"), section.version, iniFileName);
        }
    }
    for (const INI_ITEM &item : INI_ITEMS) {
        if (item.fCheck) {
            WritePrivateProfileInt(item.section, item.key, GetCheck(hDlg, item.id), iniFileName);
        }
        else {
            ::WritePrivateProfileString(item.section, item.key, GetText(hDlg, item.id).c_str(), iniFileName);
        }
    }
}

void LoadControls(HWND hDlg, const TVTPLAY_SETTINGS &s)
{
    SetCheck(hDlg, IDC_S_SHOW_OPEN_DIALOG, s.fShowOpenDialog);
    SetCheck(hDlg, IDC_S_AUTO_CLOSE, s.fAutoClose);
    SetInt(hDlg, IDC_S_FILE_INFO_MAX, s.fileInfoMax);
    SetCheck(hDlg, IDC_S_FILE_INFO_AUTO_UPDATE, s.fFileInfoAutoUpdate);
    ::SetDlgItemText(hDlg, IDC_S_POPUP_PATTERN, s.popupPattern);
    SetInt(hDlg, IDC_S_POPUP_MAX, s.popupMax);
    SetCheck(hDlg, IDC_S_POPUP_DESC, s.fPopupDesc);
    SetInt(hDlg, IDC_S_PLAYLIST_POPUP_MAX, s.playlistPopupMax);

    InitCombo(hDlg, IDC_S_SEEK_MODE, SEEK_MODE_ITEMS, _countof(SEEK_MODE_ITEMS), s.seekMode);
    SetCheck(hDlg, IDC_S_SEEK_DRAW_OFS, s.fSeekDrawOfs);
    SetCheck(hDlg, IDC_S_SEEK_DRAW_TOT, s.fSeekDrawTot);
    SetCheck(hDlg, IDC_S_POS_DRAW_TOT, s.fPosDrawTot);
    SetCheck(hDlg, IDC_S_THUMBNAIL, s.fThumbnail);
    SetInt(hDlg, IDC_S_THUMBNAIL_WIDTH, s.thumbnailWidth);
    SetInt(hDlg, IDC_S_THUMBNAIL_CACHE, s.thumbnailCacheMax);
    SetInt(hDlg, IDC_S_SEEK_ITEM_MIN_WIDTH, s.seekItemMinWidth);
    SetInt(hDlg, IDC_S_POS_ITEM_WIDTH, s.posItemWidth);
    SetInt(hDlg, IDC_S_SEEK_ITEM_ORDER, s.seekItemOrder);
    SetInt(hDlg, IDC_S_POS_ITEM_ORDER, s.posItemOrder);
    ::SetDlgItemText(hDlg, IDC_S_ICON_IMAGE, s.iconFileName);

    SetList(hDlg, IDC_S_SEEK_LIST, s.seekList, s.seekListNum);
    SetList(hDlg, IDC_S_STRETCH_LIST, s.stretchList, s.stretchListNum);
    InitCombo(hDlg, IDC_S_STRETCH_MODE, STRETCH_MODE_ITEMS, _countof(STRETCH_MODE_ITEMS), s.stretchMode);
    SetInt(hDlg, IDC_S_NO_MUTE_MAX, s.noMuteMax);
    SetInt(hDlg, IDC_S_NO_MUTE_MIN, s.noMuteMin);
    SetButtonList(hDlg, s);

    ::SetDlgItemText(hDlg, IDC_S_CHAPTERS_DIR, s.chaptersDirName);
    ::SetDlgItemText(hDlg, IDC_S_CHAPTER_IN, s.chapterIn);
    ::SetDlgItemText(hDlg, IDC_S_CHAPTER_OUT, s.chapterOut);
    ::SetDlgItemText(hDlg, IDC_S_CHAPTER_X_IN, s.chapterXIn);
    ::SetDlgItemText(hDlg, IDC_S_CHAPTER_X_OUT, s.chapterXOut);

    SetInt(hDlg, IDC_S_READ_BUF, s.readBufSizeKB);
    SetInt(hDlg, IDC_S_DISP_DELAY, s.supposedDispDelay);
    InitCombo(hDlg, IDC_S_RESET_MODE, RESET_MODE_ITEMS, _countof(RESET_MODE_ITEMS), s.resetMode);
    SetInt(hDlg, IDC_S_RESET_DROP, s.resetDropInterval);
    SetInt(hDlg, IDC_S_PCR_THRESHOLD, s.pcrThresholdMsec);
    InitCombo(hDlg, IDC_S_THREAD_PRIORITY, THREAD_PRIORITY_ITEMS, _countof(THREAD_PRIORITY_ITEMS), s.threadPriority);
    InitCombo(hDlg, IDC_S_MOD_TIMESTAMP, MOD_TIMESTAMP_ITEMS, _countof(MOD_TIMESTAMP_ITEMS), s.modTimestampMode);
    SetCheck(hDlg, IDC_S_CONV_TO_188, s.fConvTo188);
    SetCheck(hDlg, IDC_S_UNDERRUN_CTRL, s.fUnderrunCtrl);
    SetCheck(hDlg, IDC_S_USE_QPC, s.fUseQpc);
    SetCheck(hDlg, IDC_S_GAPLESS_PAUSE, s.fTryGaplessPause);
}

void SaveControls(HWND hDlg, TVTPLAY_SETTINGS &s)
{
    s.fShowOpenDialog = GetCheck(hDlg, IDC_S_SHOW_OPEN_DIALOG);
    s.fAutoClose = GetCheck(hDlg, IDC_S_AUTO_CLOSE);
    s.fileInfoMax = GetInt(hDlg, IDC_S_FILE_INFO_MAX, s.fileInfoMax);
    s.fFileInfoAutoUpdate = GetCheck(hDlg, IDC_S_FILE_INFO_AUTO_UPDATE);
    GetText(hDlg, IDC_S_POPUP_PATTERN, s.popupPattern);
    s.popupMax = GetInt(hDlg, IDC_S_POPUP_MAX, s.popupMax);
    s.fPopupDesc = GetCheck(hDlg, IDC_S_POPUP_DESC);
    s.playlistPopupMax = GetInt(hDlg, IDC_S_PLAYLIST_POPUP_MAX, s.playlistPopupMax);

    s.seekMode = GetCombo(hDlg, IDC_S_SEEK_MODE, s.seekMode);
    s.fSeekDrawOfs = GetCheck(hDlg, IDC_S_SEEK_DRAW_OFS);
    s.fSeekDrawTot = GetCheck(hDlg, IDC_S_SEEK_DRAW_TOT);
    s.fPosDrawTot = GetCheck(hDlg, IDC_S_POS_DRAW_TOT);
    s.fThumbnail = GetCheck(hDlg, IDC_S_THUMBNAIL);
    s.thumbnailWidth = min(max(GetInt(hDlg, IDC_S_THUMBNAIL_WIDTH, s.thumbnailWidth), 64), 640);
    s.thumbnailCacheMax = max(GetInt(hDlg, IDC_S_THUMBNAIL_CACHE, s.thumbnailCacheMax), 1);
    s.seekItemMinWidth = GetInt(hDlg, IDC_S_SEEK_ITEM_MIN_WIDTH, s.seekItemMinWidth);
    s.posItemWidth = max(GetInt(hDlg, IDC_S_POS_ITEM_WIDTH, s.posItemWidth), -1);
    s.seekItemOrder = GetInt(hDlg, IDC_S_SEEK_ITEM_ORDER, s.seekItemOrder);
    s.posItemOrder = GetInt(hDlg, IDC_S_POS_ITEM_ORDER, s.posItemOrder);
    GetText(hDlg, IDC_S_ICON_IMAGE, s.iconFileName);

    s.seekListNum = GetList(hDlg, IDC_S_SEEK_LIST, s.seekList, INT_MIN, INT_MAX);
    s.stretchListNum = GetList(hDlg, IDC_S_STRETCH_LIST, s.stretchList, 25, 800);
    s.stretchMode = GetCombo(hDlg, IDC_S_STRETCH_MODE, s.stretchMode);
    s.noMuteMax = GetInt(hDlg, IDC_S_NO_MUTE_MAX, s.noMuteMax);
    s.noMuteMin = GetInt(hDlg, IDC_S_NO_MUTE_MIN, s.noMuteMin);
    GetButtonList(hDlg, s);

    GetText(hDlg, IDC_S_CHAPTERS_DIR, s.chaptersDirName);
    GetText(hDlg, IDC_S_CHAPTER_IN, s.chapterIn);
    GetText(hDlg, IDC_S_CHAPTER_OUT, s.chapterOut);
    GetText(hDlg, IDC_S_CHAPTER_X_IN, s.chapterXIn);
    GetText(hDlg, IDC_S_CHAPTER_X_OUT, s.chapterXOut);

    s.readBufSizeKB = GetInt(hDlg, IDC_S_READ_BUF, s.readBufSizeKB);
    s.supposedDispDelay = min(max(GetInt(hDlg, IDC_S_DISP_DELAY, s.supposedDispDelay), 0), 5000);
    s.resetMode = GetCombo(hDlg, IDC_S_RESET_MODE, s.resetMode);
    s.resetDropInterval = GetInt(hDlg, IDC_S_RESET_DROP, s.resetDropInterval);
    s.pcrThresholdMsec = GetInt(hDlg, IDC_S_PCR_THRESHOLD, s.pcrThresholdMsec);
    s.threadPriority = GetCombo(hDlg, IDC_S_THREAD_PRIORITY, s.threadPriority);
    s.modTimestampMode = GetCombo(hDlg, IDC_S_MOD_TIMESTAMP, s.modTimestampMode);
    s.fConvTo188 = GetCheck(hDlg, IDC_S_CONV_TO_188);
    s.fUnderrunCtrl = GetCheck(hDlg, IDC_S_UNDERRUN_CTRL);
    s.fUseQpc = GetCheck(hDlg, IDC_S_USE_QPC);
    s.fTryGaplessPause = GetCheck(hDlg, IDC_S_GAPLESS_PAUSE);
}

// 選んだタブのコントロールだけを表示する
void ShowTab(HWND hDlg, int tab)
{
    for (HWND hwnd = ::GetWindow(hDlg, GW_CHILD); hwnd; hwnd = ::GetWindow(hwnd, GW_HWNDNEXT)) {
        int id = ::GetDlgCtrlID(hwnd);
        int page = (id - TAB_ID_BASE) / 100;
        if (id >= TAB_ID_BASE && page < _countof(TAB_NAMES)) {
            ::ShowWindow(hwnd, page == tab ? SW_SHOW : SW_HIDE);
        }
    }
}

void BrowseIconImage(HWND hDlg)
{
    TCHAR fileName[MAX_PATH];
    GetText(hDlg, IDC_S_ICON_IMAGE, fileName);
    OPENFILENAME ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hDlg;
    ofn.lpstrFilter = TEXT("ビットマップ (*.bmp)\0*.bmp\0すべてのファイル\0*.*\0");
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = _countof(fileName);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (::GetOpenFileName(&ofn)) {
        ::SetDlgItemText(hDlg, IDC_S_ICON_IMAGE, fileName);
    }
}

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void *pClientData)
{
    DIALOG_CONTEXT *pContext = static_cast<DIALOG_CONTEXT*>(pClientData);

    switch (uMsg) {
    case WM_INITDIALOG:
        {
            HWND hwndTab = ::GetDlgItem(hDlg, IDC_SETTINGS_TAB);
            for (int i = 0; i < _countof(TAB_NAMES); ++i) {
                TCITEM item = {};
                item.mask = TCIF_TEXT;
                item.pszText = const_cast<LPTSTR>(TAB_NAMES[i]);
                TabCtrl_InsertItem(hwndTab, i, &item);
            }
            LoadControls(hDlg, *pContext->pSettings);
            LoadIniItems(hDlg, pContext->iniFileName);
            ShowTab(hDlg, 0);
        }
        return TRUE;
    case WM_NOTIFY:
        {
            LPNMHDR pnmh = reinterpret_cast<LPNMHDR>(lParam);
            if (pnmh->idFrom == IDC_SETTINGS_TAB && pnmh->code == TCN_SELCHANGE) {
                ShowTab(hDlg, TabCtrl_GetCurSel(pnmh->hwndFrom));
                return TRUE;
            }
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_S_ICON_IMAGE_BROWSE:
            BrowseIconImage(hDlg);
            return TRUE;
        case IDOK:
            SaveControls(hDlg, *pContext->pSettings);
            SaveIniItems(hDlg, pContext->iniFileName);
            ::EndDialog(hDlg, IDOK);
            return TRUE;
        case IDCANCEL:
            ::EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}
}

bool ShowSettingsDialog(TVTest::CTVTestApp *pApp, HINSTANCE hinst, HWND hwndOwner,
                        LPCTSTR iniFileName, TVTPLAY_SETTINGS &settings)
{
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TAB_CLASSES};
    ::InitCommonControlsEx(&icc);

    DIALOG_CONTEXT context = {&settings, iniFileName};
    TVTest::ShowDialogInfo info = {};
    info.Size = sizeof(info);
    info.Flags = 0;
    info.hinst = hinst;
    info.pszTemplate = MAKEINTRESOURCEW(IDD_SETTINGS);
    info.pMessageFunc = DlgProc;
    info.pClientData = &context;
    info.hwndOwner = hwndOwner;
    return pApp->ShowDialog(&info) == IDOK;
}
