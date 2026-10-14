// Link the production reader to deterministic input/converter doubles.
#include <Windows.h>
#include <memory>
#include <string>
#include <vector>
#include <cassert>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <tchar.h>
#include "../src/ReadOnlyMmtsFile.h"
#include "../src/Mmt4kConverter.h"

HINSTANCE g_hinstDLL = nullptr;
namespace {
int chunksRead, resets;
bool emptyFirst;
bool completeOnPush = true, rewindInput;
__int64 lastInputSeek;
int64_t lastSourceStart, lastProgramStart;
}
bool CReadOnlyLocalFile::Open(LPCTSTR, int, const char *&) { return true; }
void CReadOnlyLocalFile::Close() {}
int CReadOnlyLocalFile::Read(BYTE *p, int) {
    if (chunksRead == 2) return 0;
    *p = static_cast<BYTE>(++chunksRead);
    return 1;
}
__int64 CReadOnlyLocalFile::SetPointer(__int64 p, MOVE_METHOD) { lastInputSeek = p; if (rewindInput) chunksRead = 0; return p; }
__int64 CReadOnlyLocalFile::GetSize() const { return 2; }

struct Mmt4kConverter::Impl { BYTE marker{}; bool complete{}; };
Mmt4kConverter::Mmt4kConverter() : m_impl(std::make_unique<Impl>()) {}
Mmt4kConverter::~Mmt4kConverter() = default;
bool Mmt4kConverter::Init(const std::string &, const std::string &, const std::string &, bool, bool) { return true; }
void Mmt4kConverter::InitWithoutCas() {}
void Mmt4kConverter::Push(const uint8_t *p, size_t) { m_impl->marker = *p; m_impl->complete = completeOnPush; }
std::vector<uint8_t> Mmt4kConverter::TakeOutput() {
    return emptyFirst && m_impl->marker == 1 ? std::vector<uint8_t>() : std::vector<uint8_t>(188, m_impl->marker);
}
void Mmt4kConverter::Reset() { ++resets; m_impl->complete = false; }
void Mmt4kConverter::SetEditSegment(int64_t start, int64_t, int64_t program) {
    lastSourceStart = start; lastProgramStart = program;
    m_impl->complete = false;
}
bool Mmt4kConverter::IsEditSegmentComplete() const { return m_impl->complete; }

void Initialize(CReadOnlyMmtsFile &file, bool edited = true) {
    chunksRead = resets = 0;
    emptyFirst = false;
    completeOnPush = true; rewindInput = false;
    file.m_converter = std::make_unique<Mmt4kConverter>();
    file.m_virtualSize = 10000;
    file.m_durationMsec = 2000;
    file.m_sourceDurationMsec = 3000;
    file.m_rapPoints = {{0, 0}, {1000, 10}, {2000, 20}, {2500, 25}};
    file.m_started = true;
    if (edited) {
        file.m_editSegments = {{0, 1000, 0}, {2000, 3000, 1000}};
        file.m_editCurrentSegment = 0;
    }
}

bool LoadJson(CReadOnlyMmtsFile &reader, const std::string &json) {
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, dir) && GetTempFileNameW(dir, L"tvt", 0, path));
    { std::ofstream file(std::filesystem::path(path), std::ios::binary); file << json; }
    const char *error = nullptr;
    bool ok = reader.LoadEdit(path, error);
    DeleteFileW(path);
    return ok;
}

int main() {
    {
        CReadOnlyMmtsFile reader;
        assert(LoadJson(reader, R"({"version":1,"sourceSize":2,"source":"dummy.mmts",
            "metadata":{"sourceStartMs":999,"sourceEndMs":1000},
            "timeline":[{"sourceEndMs":1000,"sourceStartMs":0},
                        {"sourceStartMs":2000,"sourceEndMs":3000}]})"));
        assert(reader.m_editSegments.size() == 2);
        assert(reader.m_editSegments[0].startMsec == 0 && reader.m_editSegments[0].endMsec == 1000);
        assert(reader.m_editSegments[1].startMsec == 2000 && reader.m_editSegments[1].programStartMsec == 1000);
        assert(!LoadJson(reader, R"({"version":1,"sourceSize":2,"source":"dummy.mmts",
            "timeline":[{"sourceStartMs":0},{"sourceEndMs":1000}]})"));
    }
    {
        CReadOnlyMmtsFile reader;
        assert(LoadJson(reader, R"({"version":1,"sourceSize":2,"source":"dummy\uD83D\uDE00.mmts",
            "map":"index\uD83D\uDE00.mmtsmap","timeline":[{"sourceEndMs":1000,"sourceStartMs":0}]})"));
        assert(std::filesystem::path(reader.m_mediaPath).filename().wstring() == L"dummy\xD83D\xDE00.mmts");
        assert(std::filesystem::path(reader.m_mapPath).filename().wstring() == L"index\xD83D\xDE00.mmtsmap");
        for (const char *escape : {"\\uD83D", "\\uDE00", "\\uD83D\\u0041", "\\uD83D\\uD83D"}) {
            const std::string json = std::string(R"({"version":1,"sourceSize":2,"source":"dummy)") + escape +
                R"(.mmts","timeline":[{"sourceStartMs":0,"sourceEndMs":1000}]})";
            assert(!LoadJson(reader, json));
        }
    }
    BYTE buf[188];
    {
        CReadOnlyMmtsFile file;
        Initialize(file);
        assert(file.Read(buf, 94) == 94 && buf[0] == 1);
        assert(resets == 0);
        // Rewinding the last output must not trigger a segment transition.
        assert(file.SetPointer(-94, IReadOnlyFile::MOVE_METHOD_CURRENT) == 0);
        assert(file.Read(buf, 188) == 188 && buf[0] == 1);
        assert(resets == 0);
        assert(file.Read(buf, 94) == 94 && buf[0] == 2);
        assert(resets == 1);
        assert(file.Read(buf, 188) == 94 && buf[0] == 2);
        assert(file.Read(buf, 188) == 0);
    }
    {
        CReadOnlyMmtsFile file;
        Initialize(file);
        emptyFirst = true;
        assert(file.Read(buf, 188) == 188 && buf[0] == 2);
        assert(file.Read(buf, 188) == 0);
    }
    {
        CReadOnlyMmtsFile file;
        Initialize(file, false);
        file.m_virtualSize = 1; // Input is smaller than the remuxed output.
        assert(file.Read(buf, 188) == 188 && buf[0] == 1);
        assert(file.SetPointer(0, IReadOnlyFile::MOVE_METHOD_CURRENT) == 188);
        assert(file.Read(buf, 188) == 188 && buf[0] == 2);
        assert(file.Read(buf, 188) == 0 && chunksRead == 2);
    }
    {
        CReadOnlyMmtsFile file;
        Initialize(file, false);
        assert(file.SeekToMsec(0));
        assert(file.Read(buf, 94) == 94 && buf[0] == 1);
        const int before = resets;
        assert(file.SeekToMsec(0));
        assert(file.Read(buf, 188) == 188 && buf[0] == 1);
        assert(resets == before && chunksRead == 1);
    }
    {
        CReadOnlyMmtsFile file;
        Initialize(file);
        file.SetRewindSize(376);
        assert(file.Read(buf, 188) == 188 && buf[0] == 1);
        assert(file.Read(buf, 188) == 188 && buf[0] == 2);
        assert(file.Read(buf, 188) == 0);
        // Flush must be able to rewind across both chunks and an edit boundary.
        assert(file.SetPointer(-376, IReadOnlyFile::MOVE_METHOD_CURRENT) == 0);
        assert(file.Read(buf, 188) == 188 && buf[0] == 1);
        assert(file.Read(buf, 188) == 188 && buf[0] == 2);
        assert(resets == 1 && chunksRead == 2);
        assert(file.Read(buf, 188) == 0);
        // Explicit time seeks bypass the output cache and use the EDL/map.
        assert(file.SeekToMsec(1500));
        assert(lastInputSeek == 25 && lastSourceStart == 2000 && lastProgramStart == 1000);
        assert(file.SetPointer(0, IReadOnlyFile::MOVE_METHOD_CURRENT) == 0);
        assert(file.SeekToMsec(2000));
        assert(file.Read(buf, 188) == 0);
        assert(!file.SeekToMsec(-1) && !file.SeekToMsec(2001));
        assert(file.SeekToMsec(0) && lastInputSeek == 0);
    }
    {
        CReadOnlyMmtsFile file;
        Initialize(file, false);
        file.SetRewindSize(94);
        assert(file.Read(buf, 188) == 188);
        assert(file.Read(buf, 188) == 188);
        assert(file.m_output.size() == 94 + 188);
        assert(file.SetPointer(10, IReadOnlyFile::MOVE_METHOD_BEGIN) == -1);
        assert(file.SetPointer(94, IReadOnlyFile::MOVE_METHOD_BEGIN) == 94);
        assert(file.Read(buf, 94) == 94 && buf[0] == 1);
        assert(resets == 0);
    }
    {
        CReadOnlyMmtsFile file;
        Initialize(file);
        completeOnPush = false; rewindInput = true;
        file.m_editSegments = {{1000, 3000, 0}, {0, 1000, 2000}};
        file.SetRewindSize(752);
        assert(file.Read(buf, 188) == 188 && buf[0] == 1);
        assert(file.Read(buf, 188) == 188 && buf[0] == 2);
        assert(file.Read(buf, 188) == 188 && buf[0] == 1);
        assert(file.m_editCurrentSegment == 1 && resets == 1 && lastInputSeek == 0);
        assert(file.Read(buf, 188) == 188 && buf[0] == 2);
        assert(file.Read(buf, 188) == 0);
        assert(file.SetPointer(-752, IReadOnlyFile::MOVE_METHOD_CURRENT) == 0);
        for (int i = 0; i < 4; ++i)
            assert(file.Read(buf, 188) == 188 && buf[0] == (i % 2) + 1);
        assert(file.Read(buf, 188) == 0 && resets == 1);
    }
    std::cout << "MMTS reader tests passed\n";
}
