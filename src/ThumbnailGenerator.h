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
    // fWaitがfalseなら処理中の生成の終わりを待たずに戻る(スレッドは打ち切ったところで自分で終わる)
    // 待たずに止めたスレッドも、次にfWaitをtrueにして止めたときにまとめて待つ
    void Stop(bool fWait);
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
    // スレッドと共有する状態。待たずに止めたスレッドが終わるまで、スレッドの側も持ち続ける
    struct SHARED {
        SHARED();
        ~SHARED();
        bool IsSuperseded(int serial);
        HANDLE hEvent;
        HWND hwndNotify;
        UINT notifyMsg;
        recursive_mutex_ lock;
        bool fStop;
        int serial;
        REQUEST request;
        bool fRequested;
        std::unique_ptr<THUMBNAIL_IMAGE> result;
    };
    class CDecoder;
    static unsigned int __stdcall ThreadProc(LPVOID pParam);
    static void Run(SHARED &shared);
    void ReapStoppedThreads(bool fWait);

    HANDLE m_hThread;
    std::shared_ptr<SHARED> m_shared;
    std::vector<HANDLE> m_stoppedThreads;
};

#endif // INCLUDE_THUMBNAIL_GENERATOR_H
