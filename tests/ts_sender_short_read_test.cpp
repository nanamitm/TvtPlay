// Exercise the production sender with a reader that returns short TS chunks.
#include <WinSock2.h>
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "../src/Util.h"
#include "../src/BufferedFileReader.h"
// Limit test access to this header; production objects keep their normal build.
#define private public
#include "../src/TsSender.h"
#undef private
// Compile the sender here because MSVC decorates member access in symbols.
#include "../src/TsSender.cpp"

HINSTANCE g_hinstDLL = nullptr;

namespace {
class ChunkedFile : public IReadOnlyFile {
public:
    std::vector<BYTE> data;
    std::vector<int> chunks;
    size_t position = 0, reads = 0;
    int endResult = 0;
    bool failSeek = false;
    bool Open(LPCTSTR, int, const char *&) override { return true; }
    void Close() override {}
    int Read(BYTE *dest, int count) override {
        const size_t call = reads++;
        if (position == data.size()) return endResult;
        const size_t chunk = chunks[call % chunks.size()];
        const size_t n = (std::min)({chunk, static_cast<size_t>(count), data.size() - position});
        std::memcpy(dest, data.data() + position, n);
        position += n;
        return static_cast<int>(n);
    }
    __int64 SetPointer(__int64 offset, MOVE_METHOD method) override {
        if (failSeek) return -1;
        const __int64 base = method == MOVE_METHOD_CURRENT ? position :
                             method == MOVE_METHOD_END ? data.size() : 0;
        const __int64 target = base + offset;
        if (target < 0 || target > static_cast<__int64>(data.size())) return -1;
        position = static_cast<size_t>(target);
        return target;
    }
    __int64 GetSize() const override { return data.size(); }
    bool IsShareWrite() const override { return false; }
};

std::vector<BYTE> Packets() {
    std::vector<BYTE> data(188 * 4, 0xff);
    for (int i = 0; i < 4; ++i) {
        BYTE *p = data.data() + 188 * i;
        p[0] = 0x47; p[1] = 1; p[2] = 0xff; p[3] = 0x20 | i;
        p[4] = 183; p[5] = 0x10;
        const uint64_t base = 90000ULL + i * 18000ULL;
        p[6] = static_cast<BYTE>(base >> 25);
        p[7] = static_cast<BYTE>(base >> 17);
        p[8] = static_cast<BYTE>(base >> 9);
        p[9] = static_cast<BYTE>(base >> 1);
        p[10] = static_cast<BYTE>((base & 1) << 7 | 0x7e); p[11] = 0;
    }
    return data;
}

bool Run(const char *name, std::vector<int> chunks, size_t length, int endResult, bool expected, bool sync) {
    CTsSender sender;
    auto file = std::make_unique<ChunkedFile>();
    file->data = Packets();
    file->data.resize(length);
    file->chunks = std::move(chunks);
    file->endResult = endResult;
    sender.m_file = std::move(file);
    sender.m_unitSize = 188;
    sender.m_fPause = true;
    sender.m_pcrPid = -1;
    sender.m_pcrPidsLen = 0;
    sender.m_reader.SetFile(sender.m_file.get());
    if (!sender.m_reader.SetupBuffer(188 * 1536, 188, 1)) return false;
    const bool result = sender.ReadToPcr(false, sync);
    const bool ok = result == expected &&
        (result ? (sender.m_pcrPid == 511 && sender.m_pcr == 63000 &&
                   sender.m_pcrReadResult.error == CTsSender::PcrReadError::None) :
                  sender.m_pcrReadResult.error == (endResult < 0 ? CTsSender::PcrReadError::Read :
                                                               CTsSender::PcrReadError::EndOfFile));
    const char *diagnostic = result ? "success" : sender.FormatOpenSeekError("SeekToBegin()");
    const bool reasonOk = result || std::strstr(diagnostic, endResult < 0 ? "stream read failed" :
                                                                            "EOF before reference PCR");
    std::printf("%s: %s (%s)\n", name, ok && reasonOk ? "PASS" : "FAIL", diagnostic);
    return ok && reasonOk;
}

bool Diagnostic(const char *name, std::vector<BYTE> data, int chunk,
                CTsSender::PcrReadError expectedError, const char *reason,
                int packets, int pcrs, int tail, bool failSeek = false, int offset = 0) {
    CTsSender sender;
    auto file = std::make_unique<ChunkedFile>();
    file->data = std::move(data);
    file->chunks = {chunk};
    file->failSeek = failSeek;
    sender.m_file = std::move(file);
    sender.m_unitSize = 188;
    sender.m_fPause = true;
    sender.m_pcrPid = -1;
    sender.m_pcrPidsLen = 0;
    sender.m_reader.SetFile(sender.m_file.get());
    if (!sender.m_reader.SetupBuffer(188 * 1536, 188, 1)) return false;
    // Use the actual seek path, including its recovery attempt on failure.
    const bool result = sender.Seek(offset, IReadOnlyFile::MOVE_METHOD_BEGIN);
    const auto report = sender.m_pcrReadResult;
    const char *text = sender.FormatOpenSeekError("SeekToBegin()");
    const std::string saved(text);
    sender.Close();
    const bool ok = !result && report.error == expectedError &&
        report.packets == packets && report.pcrs == pcrs && report.trailingBytes == tail &&
        saved.find(reason) != std::string::npos && saved == text && saved.size() < 256;
    std::printf("%s: %s (%s)\n", name, ok ? "PASS" : "FAIL", text);
    return ok;
}

bool OpenDiagnostic() {
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"tvt", 0, path)) return false;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    bool written = false;
    if (file != INVALID_HANDLE_VALUE) {
        auto packet = Packets();
        packet[5] = 0; // Valid TS with no PCR.
        written = true;
        for (int i = 0; i < 64; ++i) {
            DWORD bytes = 0;
            if (!WriteFile(file, packet.data(), 188, &bytes, nullptr) || bytes != 188) written = false;
        }
        CloseHandle(file);
    }
    CTsSender sender;
    const char *error = nullptr;
    const bool opened = written && sender.Open(path, 0, 1024 * 1024, false, false, false, 0, error);
    const std::string saved = error ? error : "";
    sender.Close();
    DeleteFileW(path);
    const bool ok = written && !opened && error && saved == error && saved ==
        "CTsSender::Open(): SeekToBegin() failed: EOF before reference PCR [TS=63, PCR=0, tail=188]";
    std::printf("Open failure diagnostic lifetime: %s (%s)\n", ok ? "PASS" : "FAIL", saved.c_str());
    return ok;
}
}

int wmain(int argc, wchar_t **argv) {
    bool ok = true;
    ok &= Run("single-packet chunks, sync", {188}, 752, 0, true, true);
    ok &= Run("single-packet chunks, async", {188}, 752, 0, true, false);
    ok &= Run("sub-packet chunks", {1, 3, 17, 167}, 752, 0, true, true);
    ok &= Run("ordinary full read", {752}, 752, 0, true, true);
    ok &= Run("EOF after one packet", {188}, 188, 0, false, true);
    ok &= Run("error after one packet", {188}, 188, -1, false, true);
    ok &= Run("empty file", {188}, 0, 0, false, true);
    ok &= Run("truncated packet", {17}, 187, 0, false, true);
    auto noPcr = Packets();
    for (size_t i = 0; i < noPcr.size(); i += 188) noPcr[i + 5] = 0;
    ok &= Diagnostic("EOF without PCR", noPcr, 752, CTsSender::PcrReadError::EndOfFile,
                     "EOF before reference PCR", 3, 0, 188);
    auto twoPcr = Packets();
    twoPcr.resize(188 * 3);
    ok &= Diagnostic("too few reference PCRs", twoPcr, 564, CTsSender::PcrReadError::EndOfFile,
                     "EOF before reference PCR", 2, 2, 188);
    ok &= Diagnostic("seek recovery retains failure", Packets(), 752, CTsSender::PcrReadError::EndOfFile,
                     "EOF before reference PCR", 2, 2, 188, false, 188);
    ok &= Diagnostic("invalid TS sync", std::vector<BYTE>(752 * 3, 0), 752,
                     CTsSender::PcrReadError::Sync, "TS synchronization failed", 0, 0, 0);
    std::vector<BYTE> longNoPcr(188 * 120001);
    for (size_t i = 0; i < longNoPcr.size(); i += 188)
        std::memcpy(longNoPcr.data() + i, noPcr.data(), 188);
    ok &= Diagnostic("PCR search limit", std::move(longNoPcr), 188 * 1536,
                     CTsSender::PcrReadError::Limit, "reference PCR search limit reached", 120000, 0, 0);
    ok &= Diagnostic("failed seek", Packets(), 752, CTsSender::PcrReadError::Seek,
                     "stream seek failed", 0, 0, 0, true);
    ok &= OpenDiagnostic();
    if (argc > 1) {
        CTsSender sender;
        const char *error = nullptr;
        const bool opened = sender.Open(argv[1], 0, 1024 * 1024, false, false, false, 0, error);
        std::printf("MMTS Open: %s (%s)\n", opened ? "PASS" : "FAIL", error ? error : "no error");
        ok &= opened;
        if (opened) {
            const bool rewind = sender.SeekToBegin();
            std::printf("MMTS rewind: %s\n", rewind ? "PASS" : "FAIL");
            ok &= rewind;
        }
    }
    return ok ? 0 : 1;
}
