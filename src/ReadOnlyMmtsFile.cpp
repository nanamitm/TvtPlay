#ifdef ENABLE_MMT4K

#include "ReadOnlyMmtsFile.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>
#include <tchar.h>
#include <vector>
#include <Shlwapi.h>
#include "Mmt4kConverter.h"

extern HINSTANCE g_hinstDLL;

namespace {

constexpr size_t INPUT_CHUNK_SIZE = 1024 * 1024;
constexpr size_t MAX_MAP_SIZE = 64 * 1024 * 1024;
constexpr uint32_t MAX_MAP_POINTS = 1000000;

template <class T> bool ReadPod(std::ifstream &stream, T &value)
{
    return static_cast<bool>(stream.read(reinterpret_cast<char *>(&value), sizeof(value)));
}

std::string ReadProfileUtf8(LPCTSTR iniPath, LPCTSTR key)
{
    TCHAR value[1024] = {};
    ::GetPrivateProfileString(TEXT("MMTS"), key, TEXT(""), value, _countof(value), iniPath);
#ifdef UNICODE
    int size = ::WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? static_cast<size_t>(size) : 0, '\0');
    if (size > 0) ::WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    if (!result.empty()) result.pop_back();
    return result;
#else
    return value;
#endif
}

bool IsValidOffset(uint64_t offset, __int64 fileSize)
{
    return offset <= static_cast<uint64_t>((std::max)(__int64(0), fileSize));
}

bool ParseInteger(std::string_view text, __int64 &value)
{
    try {
        size_t used = 0;
        value = std::stoll(std::string(text), &used, 0);
        return used == text.size();
    }
    catch (...) { return false; }
}

bool ExtractTextValue(const std::string &line, const char *key, __int64 &value)
{
    const std::string needle = std::string(key) + "=";
    const size_t begin = line.find(needle);
    if (begin == std::string::npos) return false;
    const size_t valueBegin = begin + needle.size();
    const size_t end = line.find(' ', valueBegin);
    return ParseInteger(std::string_view(line).substr(valueBegin, end - valueBegin), value);
}

} // namespace

CReadOnlyMmtsFile::CReadOnlyMmtsFile() = default;
CReadOnlyMmtsFile::~CReadOnlyMmtsFile() { Close(); }

bool CReadOnlyMmtsFile::LoadSettings(std::string &readerName, std::string &proxyServer,
                                     std::string &winscardDll, bool &convertResolutionGaiji)
{
    TCHAR iniPath[MAX_PATH] = {};
    if (!::GetModuleFileName(g_hinstDLL, iniPath, _countof(iniPath)) ||
        !::PathRenameExtension(iniPath, TEXT(".ini"))) return false;
    if (::GetPrivateProfileInt(TEXT("MMTS"), TEXT("Enabled"), 1, iniPath) == 0) return false;
    if (::GetPrivateProfileInt(TEXT("MMTS"), TEXT("Version"), 0, iniPath) < 2) {
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("Version"), TEXT("2"), iniPath);
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("Enabled"), TEXT("1"), iniPath);
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("SmartCardReaderName"), TEXT(""), iniPath);
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("CasProxyServer"), TEXT(""), iniPath);
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("CustomWinscardDLL"), TEXT(""), iniPath);
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("ConvertResolutionGaiji"), TEXT("1"), iniPath);
    }
    readerName = ReadProfileUtf8(iniPath, TEXT("SmartCardReaderName"));
    proxyServer = ReadProfileUtf8(iniPath, TEXT("CasProxyServer"));
    winscardDll = ReadProfileUtf8(iniPath, TEXT("CustomWinscardDLL"));
    convertResolutionGaiji = ::GetPrivateProfileInt(TEXT("MMTS"), TEXT("ConvertResolutionGaiji"), 1, iniPath) != 0;
    return true;
}

bool CReadOnlyMmtsFile::LoadSidecarMap(LPCTSTR mediaPath)
{
    TCHAR mapPath[MAX_PATH] = {};
    if (_tcslen(mediaPath) >= _countof(mapPath)) return false;
    _tcscpy_s(mapPath, mediaPath);
    if (!::PathRenameExtension(mapPath, TEXT(".mmtsmap"))) return false;
    std::ifstream map(mapPath, std::ios::binary);
    if (!map) return false;
    map.seekg(0, std::ios::end);
    const std::streamoff mapSize = map.tellg();
    if (mapSize < 8 || mapSize > static_cast<std::streamoff>(MAX_MAP_SIZE)) return false;
    map.seekg(0);

    char magic[8] = {};
    if (!map.read(magic, sizeof(magic))) return false;
    const bool binary2 = std::memcmp(magic, "MMTSMAP2", 8) == 0;
    const bool binary3 = std::memcmp(magic, "MMTSMAP3", 8) == 0;
    uint64_t sourceSize = 0;
    if (binary2 || binary3) {
        uint32_t version = 0, flags = 0, tracks = 0, mpts = 0, raps = 0, seeks = 0;
        int64_t duration = 0, firstPts = -1, lastPts = -1;
        if (!ReadPod(map, version) || !ReadPod(map, flags) || !ReadPod(map, sourceSize) ||
            !ReadPod(map, duration) || !ReadPod(map, firstPts) || !ReadPod(map, lastPts) ||
            !ReadPod(map, tracks) || !ReadPod(map, mpts) || !ReadPod(map, raps) || !ReadPod(map, seeks) ||
            version != (binary3 ? 3U : 2U) || flags != 0 || tracks > MAX_MAP_POINTS ||
            mpts > MAX_MAP_POINTS || raps > MAX_MAP_POINTS || seeks > MAX_MAP_POINTS ||
            sourceSize != static_cast<uint64_t>(m_inputSize) || duration <= 0) return false;
        m_durationMsec = duration > (std::numeric_limits<int>::max)() ? 0 : static_cast<int>(duration);
        for (uint32_t i = 0; i < tracks; ++i) {
            char ignored[20];
            if (!map.read(ignored, sizeof(ignored))) return false;
        }
        for (uint32_t i = 0; i < mpts; ++i) {
            int64_t ignoredTime; uint64_t ignoredOffset; uint32_t count;
            if (!ReadPod(map, ignoredTime) || !ReadPod(map, ignoredOffset) || !ReadPod(map, count) || count > MAX_MAP_POINTS) return false;
            map.seekg(static_cast<std::streamoff>(count) * sizeof(uint32_t), std::ios::cur);
            if (!map) return false;
        }
        auto readPoints = [&](uint32_t count, std::vector<MapPoint> &points) {
            points.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                int64_t time; uint64_t offset;
                if (!ReadPod(map, time) || !ReadPod(map, offset) || time < 0 || !IsValidOffset(offset, m_inputSize)) return false;
                points.push_back({time - (firstPts > 0 ? firstPts : 0), static_cast<__int64>(offset)});
            }
            return true;
        };
        if (!readPoints(raps, m_rapPoints) || !readPoints(seeks, m_seekPoints)) return false;
    } else {
        map.clear(); map.seekg(0);
        std::string line;
        if (!std::getline(map, line) || line != "MMTSMAP 1") return false;
        __int64 firstPts = -1, source = -1;
        while (std::getline(map, line)) {
            __int64 time = -1, offset = -1;
            if (line.rfind("source_size=", 0) == 0) { ParseInteger(std::string_view(line).substr(12), source); }
            else if (line.rfind("duration_ms=", 0) == 0) { if (ParseInteger(std::string_view(line).substr(12), time) && time > 0 && time <= (std::numeric_limits<int>::max)()) m_durationMsec = static_cast<int>(time); }
            else if (line.rfind("first_video_pts_ms=", 0) == 0) { ParseInteger(std::string_view(line).substr(19), firstPts); }
            else if ((line.rfind("rap ", 0) == 0 || line.rfind("seek ", 0) == 0) &&
                     ExtractTextValue(line, "time_ms", time) && ExtractTextValue(line, "offset", offset) &&
                     time >= 0 && offset >= 0 && offset <= m_inputSize) {
                (line.rfind("rap ", 0) == 0 ? m_rapPoints : m_seekPoints).push_back({time, offset});
            }
        }
        if (source != m_inputSize || m_durationMsec <= 0) return false;
        if (firstPts > 0) for (auto *points : {&m_rapPoints, &m_seekPoints})
            for (auto &point : *points) point.timeMsec -= firstPts;
    }
    auto normalize = [](std::vector<MapPoint> &points) {
        points.erase(std::remove_if(points.begin(), points.end(), [](const MapPoint &p) { return p.timeMsec < 0; }), points.end());
        std::sort(points.begin(), points.end(), [](const MapPoint &a, const MapPoint &b) { return a.timeMsec < b.timeMsec; });
    };
    normalize(m_rapPoints); normalize(m_seekPoints);
    return m_durationMsec > 0;
}

bool CReadOnlyMmtsFile::Open(LPCTSTR path, int flags, const char *&errorMessage)
{
    Close();
    if (!(flags & OPEN_FLAG_NORMAL) || (flags & OPEN_FLAG_SHARE_WRITE) || !m_input.Open(path, OPEN_FLAG_NORMAL, errorMessage)) return false;
    m_inputSize = m_input.GetSize();
    if (m_inputSize <= 0 || !LoadSidecarMap(path)) {
        errorMessage = "CReadOnlyMmtsFile: A valid .mmtsmap sidecar is required for seekable MMTS playback";
        Close(); return false;
    }
    std::string readerName, proxyServer, winscardDll; bool convertResolutionGaiji = true;
    if (!LoadSettings(readerName, proxyServer, winscardDll, convertResolutionGaiji)) {
        errorMessage = "CReadOnlyMmtsFile: MMTS playback is disabled or settings cannot be loaded";
        Close(); return false;
    }
    m_converter = std::make_unique<Mmt4kConverter>();
    if (!m_converter->Init(readerName, proxyServer, winscardDll, convertResolutionGaiji)) {
        errorMessage = "CReadOnlyMmtsFile: Cannot initialize dantto4k";
        Close(); return false;
    }
    // The map's source size is a much better estimate of the remuxed TS
    // bitrate than a fixed rate (8K services can exceed 80 Mbps).
    m_virtualSize = m_inputSize;
    if (m_virtualSize <= 0 || !SeekToVirtualPosition(0)) {
        errorMessage = "CReadOnlyMmtsFile: Cannot initialize MMTS stream";
        Close(); return false;
    }
    return true;
}

void CReadOnlyMmtsFile::Close()
{
    m_converter.reset(); m_input.Close(); m_output.clear(); m_rapPoints.clear(); m_seekPoints.clear();
    m_outputOffset = 0; m_inputSize = m_virtualSize = -1; m_position = 0; m_durationMsec = 0; m_eof = m_started = false;
}

int CReadOnlyMmtsFile::GetPositionMsecFromBytes(__int64 bytes) const
{
    if (m_virtualSize <= 0 || m_durationMsec <= 0) return 0;
    return static_cast<int>((std::max)(__int64(0), (std::min)(bytes, m_virtualSize)) * m_durationMsec / m_virtualSize);
}

__int64 CReadOnlyMmtsFile::GetPositionBytesFromMsec(int msec) const
{
    if (m_virtualSize <= 0 || m_durationMsec <= 0) return 0;
    return static_cast<__int64>((std::max)(0, (std::min)(msec, m_durationMsec))) * m_virtualSize / m_durationMsec;
}

__int64 CReadOnlyMmtsFile::FindSourceOffset(int msec) const
{
    const auto &points = m_rapPoints.empty() ? m_seekPoints : m_rapPoints;
    __int64 offset = 0;
    for (const auto &point : points) { if (point.timeMsec > msec) break; offset = point.offset; }
    return offset;
}

bool CReadOnlyMmtsFile::SeekToVirtualPosition(__int64 position)
{
    const int targetMsec = GetPositionMsecFromBytes(position);
    const __int64 sourceOffset = FindSourceOffset(targetMsec);
    if (m_input.SetPointer(sourceOffset, MOVE_METHOD_BEGIN) < 0) return false;
    // A newly-created demuxer must see the stream from its initial state.  A
    // reset before the first input chunk clears ACAS state and can suppress all
    // output. Subsequent seeks retain stream registration and reset sequence
    // state through Mmt4kConverter::Reset().
    if (m_started) m_converter->Reset();
    else m_started = true;
    m_output.clear(); m_outputOffset = 0; m_eof = false; m_position = position;
    return true;
}

bool CReadOnlyMmtsFile::FillOutput()
{
    std::vector<BYTE> input(INPUT_CHUNK_SIZE);
    while (!m_eof && m_outputOffset == m_output.size()) {
        const int read = m_input.Read(input.data(), static_cast<int>(input.size()));
        if (read < 0) return false;
        if (read == 0) { m_eof = true; break; }
        m_converter->Push(input.data(), static_cast<size_t>(read));
        m_output = m_converter->TakeOutput(); m_outputOffset = 0;
    }
    return true;
}

int CReadOnlyMmtsFile::Read(BYTE *pBuf, int numToRead)
{
    if (!pBuf || numToRead <= 0 || !m_converter) return -1;
    if (m_position >= m_virtualSize) return 0;
    if (!FillOutput()) return -1;
    if (m_outputOffset == m_output.size()) return 0;

    // Keep each return value inside one remuxed chunk.  The first probe read in
    // CTsSender is intentionally unaligned (8192 bytes), then it rewinds to
    // zero. Crossing chunks here would evict the beginning of that probe from
    // the bounded cache and force a demuxer reset before playback begins.
    const size_t available = m_output.size() - m_outputOffset;
    const size_t count = (std::min)(available, static_cast<size_t>(numToRead));
    std::memcpy(pBuf, m_output.data() + m_outputOffset, count);
    m_outputOffset += count;
    m_position += static_cast<__int64>(count);
    return static_cast<int>(count);
}

__int64 CReadOnlyMmtsFile::SetPointer(__int64 distanceToMove, MOVE_METHOD moveMethod)
{
    const __int64 base = moveMethod == MOVE_METHOD_CURRENT ? m_position : moveMethod == MOVE_METHOD_END ? m_virtualSize : 0;
    if (base < 0 || distanceToMove > m_virtualSize - base || distanceToMove < -base) return -1;
    const __int64 target = base + distanceToMove;
    if (target == m_position) return target;
    // CBufferedFileReader frequently rewinds inside the most recently produced
    // chunk. Keep that chunk as the bounded cache instead of restarting dantto4k.
    const __int64 cachedBegin = m_position - static_cast<__int64>(m_outputOffset);
    const __int64 cachedEnd = cachedBegin + static_cast<__int64>(m_output.size());
    if (target >= cachedBegin && target <= cachedEnd) {
        m_outputOffset = static_cast<size_t>(target - cachedBegin);
        m_position = target;
        return target;
    }
    return SeekToVirtualPosition(target) ? target : -1;
}

#endif // ENABLE_MMT4K
