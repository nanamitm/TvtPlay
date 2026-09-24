// Generates thumbnails from real TS files through CThumbnailGenerator and
// writes them as BMP files, so the seek and decode path can be checked by eye.
// Usage: thumbnail_generator_test.exe <output dir> <file.ts>...
#include <Windows.h>
#include <tchar.h>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../src/Util.h"
#include "../src/ThumbnailGenerator.h"

namespace
{
const UINT WM_RESULT = WM_APP + 1;

// The last PCR of the file gives the duration, as CTsSender measures it.
int GetDurationMsec(LPCWSTR path)
{
    HANDLE hFile = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER size;
    ::GetFileSizeEx(hFile, &size);
    std::vector<BYTE> head(4 * 1024 * 1024), tail(4 * 1024 * 1024);
    DWORD n;
    ::ReadFile(hFile, head.data(), static_cast<DWORD>(head.size()), &n, nullptr);
    head.resize(n);
    LARGE_INTEGER pos;
    pos.QuadPart = max(size.QuadPart - static_cast<LONGLONG>(tail.size()), 0LL);
    ::SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN);
    ::ReadFile(hFile, tail.data(), static_cast<DWORD>(tail.size()), &n, nullptr);
    tail.resize(n);
    ::CloseHandle(hFile);

    int unitSize = select_unit_size(head.data(), head.data() + head.size());
    int pcrPid = -1;
    DWORD first = 0, last = 0;
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<BYTE> &buf = pass == 0 ? head : tail;
        BYTE *p = resync(buf.data(), buf.data() + buf.size(), unitSize);
        bool fFound = false;
        for (; p && p + 188 <= buf.data() + buf.size(); p += unitSize) {
            if (p[0] != 0x47 || !(p[3] & 0x20) || p[4] < 7 || !(p[5] & 0x10)) continue;
            int pid = (p[1] & 0x1f) << 8 | p[2];
            if (pcrPid < 0) pcrPid = pid;
            if (pid != pcrPid) continue;
            DWORD pcr = static_cast<DWORD>(p[6]) << 24 | p[7] << 16 | p[8] << 8 | p[9];
            if (pass == 0) {
                first = pcr;
                break;
            }
            last = pcr;
            fFound = true;
        }
        if (pass == 1 && !fFound) return -1;
    }
    return static_cast<int>((last - first) / PCR_PER_MSEC);
}

bool WriteBmp(LPCWSTR path, const THUMBNAIL_IMAGE &image)
{
    BITMAPINFOHEADER bih = {sizeof(bih)};
    bih.biWidth = image.width;
    bih.biHeight = -image.height;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    BITMAPFILEHEADER bfh = {};
    bfh.bfType = 'MB';
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
    bfh.bfSize = bfh.bfOffBits + static_cast<DWORD>(image.bits.size());
    FILE *fp;
    if (_wfopen_s(&fp, path, L"wb") != 0) return false;
    fwrite(&bfh, sizeof(bfh), 1, fp);
    fwrite(&bih, sizeof(bih), 1, fp);
    fwrite(image.bits.data(), image.bits.size(), 1, fp);
    fclose(fp);
    return true;
}

std::unique_ptr<THUMBNAIL_IMAGE> WaitResult(CThumbnailGenerator &generator)
{
    MSG msg;
    while (::GetMessage(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_RESULT) return generator.TakeResult();
        ::DispatchMessage(&msg);
    }
    return nullptr;
}
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) {
        fwprintf(stderr, L"usage: %s <output dir> <file.ts>...\n", argv[0]);
        return 2;
    }
    HWND hwnd = ::CreateWindowW(L"STATIC", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    CThumbnailGenerator generator;
    if (!hwnd || !generator.Start(hwnd, WM_RESULT)) return 1;

    // Each file is a new generation on the same worker, as when a playlist
    // moves on, so the decoder has to follow a change of video format.
    int failures = 0;
    for (int fileIndex = 2; fileIndex < argc; ++fileIndex) {
        LPCWSTR path = argv[fileIndex];
        int generation = fileIndex;
        int dur = GetDurationMsec(path);
        wprintf(L"%s\nduration: %d ms\n", path, dur);
        if (dur <= 0) {
            ++failures;
            continue;
        }
        const int percents[] = {0, 10, 25, 50, 75, 90, 100};
        for (int percent : percents) {
            int msec = static_cast<int>(static_cast<long long>(dur) * percent / 100);
            DWORD tick = ::GetTickCount();
            generator.Request(generation, path, msec, dur, 160);
            std::unique_ptr<THUMBNAIL_IMAGE> image = WaitResult(generator);
            DWORD elapsed = ::GetTickCount() - tick;
            if (!image || image->status != 0) {
                wprintf(L"%3d%% (%d ms): status %d, %lu ms\n", percent, msec, image ? image->status : -1, elapsed);
                ++failures;
                continue;
            }
            wchar_t out[MAX_PATH];
            swprintf_s(out, L"%s\\thumb_%d_%03d.bmp", argv[1], fileIndex - 1, percent);
            WriteBmp(out, *image);
            wprintf(L"%3d%% (%d ms): %dx%d, %lu ms -> %s\n", percent, msec, image->width, image->height, elapsed, out);
        }

        // A newer request must cancel the one being worked on; only the last result arrives.
        generator.Request(generation, path, dur / 3, dur, 160);
        generator.Request(generation, path, dur / 2, dur, 160);
        std::unique_ptr<THUMBNAIL_IMAGE> image = WaitResult(generator);
        if (!image || image->msec != dur / 2) {
            wprintf(L"superseding: expected %d, got %d\n", dur / 2, image ? image->msec : -1);
            ++failures;
        }
    }

    generator.Stop();
    ::DestroyWindow(hwnd);
    wprintf(failures ? L"FAILED (%d)\n" : L"OK\n", failures);
    return failures ? 1 : 0;
}
