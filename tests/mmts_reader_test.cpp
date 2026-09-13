// Link the production reader to deterministic input/converter doubles.
#include <Windows.h>
#include <memory>
#include <string>
#include <vector>
#include <cassert>
#include <iostream>
#include <tchar.h>
#define private public
#include "../src/ReadOnlyMmtsFile.h"
#undef private
#include "../src/Mmt4kConverter.h"

HINSTANCE g_hinstDLL = nullptr;
namespace {
int chunksRead, resets;
bool emptyFirst;
}
bool CReadOnlyLocalFile::Open(LPCTSTR, int, const char *&) { return true; }
void CReadOnlyLocalFile::Close() {}
int CReadOnlyLocalFile::Read(BYTE *p, int) {
    if (chunksRead == 2) return 0;
    *p = static_cast<BYTE>(++chunksRead);
    return 1;
}
__int64 CReadOnlyLocalFile::SetPointer(__int64 p, MOVE_METHOD) { return p; }
__int64 CReadOnlyLocalFile::GetSize() const { return 2; }

struct Mmt4kConverter::Impl { BYTE marker{}; bool complete{}; };
Mmt4kConverter::Mmt4kConverter() : m_impl(std::make_unique<Impl>()) {}
Mmt4kConverter::~Mmt4kConverter() = default;
bool Mmt4kConverter::Init(const std::string &, const std::string &, const std::string &, bool, bool) { return true; }
void Mmt4kConverter::Push(const uint8_t *p, size_t) { m_impl->marker = *p; m_impl->complete = true; }
std::vector<uint8_t> Mmt4kConverter::TakeOutput() {
    return emptyFirst && m_impl->marker == 1 ? std::vector<uint8_t>() : std::vector<uint8_t>(188, m_impl->marker);
}
void Mmt4kConverter::Reset() { ++resets; m_impl->complete = false; }
void Mmt4kConverter::SetEditSegment(int64_t, int64_t, int64_t) { m_impl->complete = false; }
bool Mmt4kConverter::IsEditSegmentComplete() const { return m_impl->complete; }

void Initialize(CReadOnlyMmtsFile &file, bool edited = true) {
    chunksRead = resets = 0;
    emptyFirst = false;
    file.m_converter = std::make_unique<Mmt4kConverter>();
    file.m_virtualSize = 10000;
    file.m_started = true;
    if (edited) {
        file.m_editSegments = {{0, 1000, 0}, {2000, 3000, 1000}};
        file.m_editCurrentSegment = 0;
    }
}

int main() {
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
    std::cout << "MMTS reader tests passed\n";
}
