#ifndef INCLUDE_CHAPTER_MAP_H
#define INCLUDE_CHAPTER_MAP_H

class CChapterMap
{
    static const int RETRY_LIMIT = 3;
public:
    static const LPCTSTR DEFAULT_CHAPTER_IN;
    static const LPCTSTR DEFAULT_CHAPTER_OUT;
    static const LPCTSTR DEFAULT_CHAPTER_X_IN;
    static const LPCTSTR DEFAULT_CHAPTER_X_OUT;
    static const int CHAPTER_POS_MAX = 99*3600000+59*60000+59*1000+999;
    struct CHAPTER {
        // 最終要素のみにNULを必ず格納する
        std::vector<TCHAR> name;
        CHAPTER(LPCTSTR name_ = TEXT("")) : name(name_, name_ + _tcslen(name_) + 1) {}
        bool IsMatchPattern(LPCTSTR pattern, std::pair<size_t, size_t> *pos = nullptr) const;
        void SetPattern(LPCTSTR pattern, bool f);
    };
    CChapterMap();
    ~CChapterMap();
    bool Open(LPCTSTR path, LPCTSTR subDirName);
    void Close();
    bool Sync();
    bool Insert(const std::pair<int, CHAPTER> &ch, int pos = -1);
    bool Erase(int pos);
    void ShiftAll(int offset);
    const std::map<int, CHAPTER>& Get() const { return m_map; }
    bool IsOpen() const { return m_path[0] != 0; }
    bool NeedToSync() const { return m_hDir != INVALID_HANDLE_VALUE; }
    const TCHAR (&GetChapterIn() const)[64] { return m_chapterIn; }
    const TCHAR (&GetChapterOut() const)[64] { return m_chapterOut; }
    const TCHAR (&GetChapterXIn() const)[64] { return m_chapterXIn; }
    const TCHAR (&GetChapterXOut() const)[64] { return m_chapterXOut; }
    TCHAR (&GetChapterIn())[64] { return m_chapterIn; }
    TCHAR (&GetChapterOut())[64] { return m_chapterOut; }
    TCHAR (&GetChapterXIn())[64] { return m_chapterXIn; }
    TCHAR (&GetChapterXOut())[64] { return m_chapterXOut; }
private:
    bool Save() const;
    bool InsertCommand(LPCTSTR p);
    bool InsertOgmStyleCommand(LPCTSTR p);
    std::map<int, CHAPTER> m_map;
    TCHAR m_path[MAX_PATH];
    TCHAR m_chapterIn[64];
    TCHAR m_chapterOut[64];
    TCHAR m_chapterXIn[64];
    TCHAR m_chapterXOut[64];
    HANDLE m_hDir, m_hEvent;
    bool m_fWritable;
    int m_retryCount;
    OVERLAPPED m_ol;
    BYTE m_buf[2048];
};

#endif // INCLUDE_CHAPTER_MAP_H
