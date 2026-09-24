#ifndef INCLUDE_SETTINGS_DIALOG_H
#define INCLUDE_SETTINGS_DIALOG_H

// 設定ダイアログで編集する[Settings]セクションの値
// [MP4][MMTS]セクションとTvtpCmdOptionはファイルを開くときや起動時に読まれるので、ダイアログが直接読み書きする
struct TVTPLAY_SETTINGS {
    static const int LIST_MAX = 26;
    static const int BUTTON_MAX = 18;
    static const int BUTTON_TEXT_MAX = 192;
    static const int PATTERN_MAX = 64;
    // 全般
    bool fShowOpenDialog;
    bool fAutoClose;
    int fileInfoMax;
    bool fFileInfoAutoUpdate;
    TCHAR popupPattern[MAX_PATH];
    int popupMax;
    bool fPopupDesc;
    int playlistPopupMax;
    // シークバー
    int seekMode;
    bool fSeekDrawOfs;
    bool fSeekDrawTot;
    bool fPosDrawTot;
    bool fThumbnail;
    int thumbnailWidth;
    int thumbnailCacheMax;
    int seekItemMinWidth;
    int posItemWidth;
    int seekItemOrder;
    int posItemOrder;
    TCHAR iconFileName[MAX_PATH];
    // シーク・倍速
    int seekList[LIST_MAX];
    int seekListNum;
    int stretchList[LIST_MAX];
    int stretchListNum;
    int stretchMode;
    int noMuteMax;
    int noMuteMin;
    TCHAR buttonList[BUTTON_MAX][BUTTON_TEXT_MAX];
    // チャプター
    TCHAR chaptersDirName[MAX_PATH];
    TCHAR chapterIn[PATTERN_MAX];
    TCHAR chapterOut[PATTERN_MAX];
    TCHAR chapterXIn[PATTERN_MAX];
    TCHAR chapterXOut[PATTERN_MAX];
    // 詳細
    int readBufSizeKB;
    int supposedDispDelay;
    int resetMode;
    int resetDropInterval;
    int pcrThresholdMsec;
    int threadPriority;
    int modTimestampMode;
    bool fConvTo188;
    bool fUnderrunCtrl;
    bool fUseQpc;
    bool fTryGaplessPause;
};

// 設定ダイアログを表示する。OKで閉じられればtrueを返し、settingsを更新する
// iniFileNameの[MP4][MMTS]セクションとTvtpCmdOptionはOKのときにここで書き込む
bool ShowSettingsDialog(TVTest::CTVTestApp *pApp, HINSTANCE hinst, HWND hwndOwner,
                        LPCTSTR iniFileName, TVTPLAY_SETTINGS &settings);

#endif // INCLUDE_SETTINGS_DIALOG_H
