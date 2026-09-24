#ifndef INCLUDE_THUMBNAIL_GENERATOR_H
#define INCLUDE_THUMBNAIL_GENERATOR_H

#include <memory>
#include <string>
#include <vector>

// デコード済みのサムネイル画像(トップダウンのBGRA)
struct THUMBNAIL_IMAGE {
    int generation;
    int msec;
    // 0:成功, 1:この位置では作れなかった, 2:このファイルでは作れない
    int status;
    int width;
    int height;
    std::vector<BYTE> bits;
};

// シークバーのサムネイルを専用スレッドで生成する
// 要求は最新の1件だけを保持し、処理中に新しい要求が来れば古いほうを打ち切る
class CThumbnailGenerator
{
public:
    CThumbnailGenerator();
    ~CThumbnailGenerator();
    bool Start(HWND hwndNotify, UINT notifyMsg);
    void Stop();
    void Request(int generation, LPCTSTR path, int msec, int durMsec, int width);
    std::unique_ptr<THUMBNAIL_IMAGE> TakeResult();
    static bool IsSupportedFile(LPCTSTR path);
private:
    struct REQUEST {
        int serial;
        int generation;
        std::basic_string<TCHAR> path;
        int msec;
        int durMsec;
        int width;
    };
    class CDecoder;
    static unsigned int __stdcall ThreadProc(LPVOID pParam);
    void Run();
    bool IsSuperseded(int serial);

    HANDLE m_hThread;
    HANDLE m_hEvent;
    HWND m_hwndNotify;
    UINT m_notifyMsg;
    recursive_mutex_ m_lock;
    bool m_fStop;
    int m_serial;
    REQUEST m_request;
    bool m_fRequested;
    std::unique_ptr<THUMBNAIL_IMAGE> m_result;
};

#endif // INCLUDE_THUMBNAIL_GENERATOR_H
