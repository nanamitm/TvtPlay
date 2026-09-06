#ifdef ENABLE_MMT4K

#include "ReadOnlyMmtsFile.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <filesystem>
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

bool FindJsonValue(const std::string &text, const char *key, size_t &valueBegin, size_t searchBegin = 0)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = searchBegin;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        pos += needle.size();
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
        if (pos >= text.size() || text[pos] != ':') continue;
        ++pos;
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
        valueBegin = pos;
        return true;
    }
    return false;
}

bool ReadJsonInteger(const std::string &text, size_t valueBegin, __int64 &value)
{
    if (valueBegin >= text.size()) return false;
    const char *begin = text.data() + valueBegin;
    const char *end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc() && result.ptr != begin;
}

void AppendUtf8CodePoint(std::string &value, unsigned codePoint)
{
    if (codePoint <= 0x7F) value.push_back(static_cast<char>(codePoint));
    else if (codePoint <= 0x7FF) {
        value.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        value.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else {
        value.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        value.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        value.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

bool ReadJsonString(const std::string &text, size_t valueBegin, std::string &value)
{
    if (valueBegin >= text.size() || text[valueBegin] != '"') return false;
    value.clear();
    for (size_t pos = valueBegin + 1; pos < text.size(); ++pos) {
        const char ch = text[pos];
        if (ch == '"') return true;
        if (ch != '\\') {
            if (static_cast<unsigned char>(ch) < 0x20) return false;
            value.push_back(ch);
            continue;
        }
        if (++pos >= text.size()) return false;
        switch (text[pos]) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u': {
            if (pos + 4 >= text.size()) return false;
            unsigned codePoint = 0;
            for (int i = 1; i <= 4; ++i) {
                const char hex = text[pos + i];
                codePoint <<= 4;
                if ('0' <= hex && hex <= '9') codePoint |= hex - '0';
                else if ('a' <= hex && hex <= 'f') codePoint |= hex - 'a' + 10;
                else if ('A' <= hex && hex <= 'F') codePoint |= hex - 'A' + 10;
                else return false;
            }
            AppendUtf8CodePoint(value, codePoint);
            pos += 4;
            break;
        }
        default: return false;
        }
    }
    return false;
}

std::wstring Utf8ToWide(const std::string &text)
{
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size) == size ? result : std::wstring();
}

} // namespace

CReadOnlyMmtsFile::CReadOnlyMmtsFile() = default;
CReadOnlyMmtsFile::~CReadOnlyMmtsFile() { Close(); }

bool CReadOnlyMmtsFile::LoadSettings(std::string &readerName, std::string &proxyServer,
                                     std::string &winscardDll, bool &convertResolutionGaiji,
                                     bool &useSmartCard)
{
    TCHAR iniPath[MAX_PATH] = {};
    if (!::GetModuleFileName(g_hinstDLL, iniPath, _countof(iniPath)) ||
        !::PathRenameExtension(iniPath, TEXT(".ini"))) return false;
    if (::GetPrivateProfileInt(TEXT("MMTS"), TEXT("Enabled"), 1, iniPath) == 0) return false;
    const int version = ::GetPrivateProfileInt(TEXT("MMTS"), TEXT("Version"), 0, iniPath);
    if (version < 2) {
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("Version"), TEXT("2"), iniPath);
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("Enabled"), TEXT("1"), iniPath);
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("SmartCardReaderName"), TEXT(""), iniPath);
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("CasProxyServer"), TEXT(""), iniPath);
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("CustomWinscardDLL"), TEXT(""), iniPath);
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("ConvertResolutionGaiji"), TEXT("1"), iniPath);
    }
    if (version < 3) {
        // Only the key this version adds is written, so an .ini someone has
        // already configured keeps the values in it.
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("UseSmartCard"), TEXT("0"), iniPath);
        ::WritePrivateProfileString(TEXT("MMTS"), TEXT("Version"), TEXT("3"), iniPath);
    }
    readerName = ReadProfileUtf8(iniPath, TEXT("SmartCardReaderName"));
    proxyServer = ReadProfileUtf8(iniPath, TEXT("CasProxyServer"));
    winscardDll = ReadProfileUtf8(iniPath, TEXT("CustomWinscardDLL"));
    convertResolutionGaiji = ::GetPrivateProfileInt(TEXT("MMTS"), TEXT("ConvertResolutionGaiji"), 1, iniPath) != 0;
    // Off by default: a recording is expected to be descrambled already, and
    // reaching for a card reader that is not needed pulls a winscard DLL into
    // the process for nothing.
    useSmartCard = ::GetPrivateProfileInt(TEXT("MMTS"), TEXT("UseSmartCard"), 0, iniPath) != 0;
    return true;
}

bool CReadOnlyMmtsFile::LoadSidecarMap(LPCTSTR mediaPath, LPCTSTR explicitMapPath)
{
    TCHAR mapPath[MAX_PATH] = {};
    LPCTSTR resolvedMapPath = explicitMapPath;
    if (!resolvedMapPath) {
        if (_tcslen(mediaPath) >= _countof(mapPath)) return false;
        _tcscpy_s(mapPath, mediaPath);
        if (!::PathRenameExtension(mapPath, TEXT(".mmtsmap"))) return false;
        resolvedMapPath = mapPath;
    }
    std::ifstream map(resolvedMapPath, std::ios::binary);
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
        m_sourceDurationMsec = duration > (std::numeric_limits<int>::max)() ? 0 : static_cast<int>(duration);
        m_durationMsec = m_sourceDurationMsec;
        m_firstPtsMsec = firstPts > 0 ? firstPts : 0;
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
            else if (line.rfind("duration_ms=", 0) == 0) { if (ParseInteger(std::string_view(line).substr(12), time) && time > 0 && time <= (std::numeric_limits<int>::max)()) m_durationMsec = m_sourceDurationMsec = static_cast<int>(time); }
            else if (line.rfind("first_video_pts_ms=", 0) == 0) { ParseInteger(std::string_view(line).substr(19), firstPts); }
            else if ((line.rfind("rap ", 0) == 0 || line.rfind("seek ", 0) == 0) &&
                     ExtractTextValue(line, "time_ms", time) && ExtractTextValue(line, "offset", offset) &&
                     time >= 0 && offset >= 0 && offset <= m_inputSize) {
                (line.rfind("rap ", 0) == 0 ? m_rapPoints : m_seekPoints).push_back({time, offset});
            }
        }
        if (source != m_inputSize || m_durationMsec <= 0) return false;
        m_firstPtsMsec = firstPts > 0 ? firstPts : 0;
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

bool CReadOnlyMmtsFile::LoadEdit(LPCTSTR editPath, const char *&errorMessage)
{
    std::ifstream edit(std::filesystem::path(editPath), std::ios::binary | std::ios::ate);
    if (!edit) {
        errorMessage = "CReadOnlyMmtsFile: Cannot open .mmtsedit";
        return false;
    }
    const std::streamoff size = edit.tellg();
    if (size <= 0 || size > 1024 * 1024) {
        errorMessage = "CReadOnlyMmtsFile: Invalid .mmtsedit size";
        return false;
    }
    edit.seekg(0);
    std::string json(static_cast<size_t>(size), '\0');
    if (!edit.read(json.data(), size)) {
        errorMessage = "CReadOnlyMmtsFile: Cannot read .mmtsedit";
        return false;
    }

    size_t valueBegin = 0;
    __int64 version = 0;
    __int64 sourceSize = -1;
    std::string source;
    std::string map;
    if (!FindJsonValue(json, "version", valueBegin) || !ReadJsonInteger(json, valueBegin, version) || version != 1 ||
        !FindJsonValue(json, "sourceSize", valueBegin) || !ReadJsonInteger(json, valueBegin, sourceSize) || sourceSize < 0 ||
        !FindJsonValue(json, "source", valueBegin) || !ReadJsonString(json, valueBegin, source) || source.empty()) {
        errorMessage = "CReadOnlyMmtsFile: Invalid .mmtsedit header";
        return false;
    }
    if (FindJsonValue(json, "map", valueBegin) && !ReadJsonString(json, valueBegin, map)) {
        errorMessage = "CReadOnlyMmtsFile: Invalid .mmtsedit map path";
        return false;
    }

    std::vector<EditSegment> segments;
    size_t scan = 0;
    int totalDuration = 0;
    for (;;) {
        __int64 start = 0, end = 0;
        if (!FindJsonValue(json, "sourceStartMs", valueBegin, scan)) break;
        const size_t afterStart = valueBegin + 1;
        if (!ReadJsonInteger(json, valueBegin, start) || !FindJsonValue(json, "sourceEndMs", valueBegin, afterStart) ||
            !ReadJsonInteger(json, valueBegin, end)) {
            errorMessage = "CReadOnlyMmtsFile: Invalid .mmtsedit timeline";
            return false;
        }
        scan = valueBegin + 1;
        if (start < 0 || end <= start || end > (std::numeric_limits<int>::max)() ||
            end - start > (std::numeric_limits<int>::max)() || totalDuration > (std::numeric_limits<int>::max)() - (end - start)) {
            errorMessage = "CReadOnlyMmtsFile: Invalid .mmtsedit segment";
            return false;
        }
        segments.push_back({static_cast<int>(start), static_cast<int>(end), totalDuration});
        totalDuration += static_cast<int>(end - start);
    }
    if (segments.empty()) {
        errorMessage = "CReadOnlyMmtsFile: .mmtsedit has no timeline segments";
        return false;
    }

    const std::wstring sourceWide = Utf8ToWide(source);
    const std::wstring mapWide = map.empty() ? std::wstring() : Utf8ToWide(map);
    if (sourceWide.empty() || (!map.empty() && mapWide.empty())) {
        errorMessage = "CReadOnlyMmtsFile: .mmtsedit path is not valid UTF-8";
        return false;
    }
    try {
        const std::filesystem::path base = std::filesystem::path(editPath).parent_path();
        std::filesystem::path sourcePath(sourceWide);
        if (sourcePath.is_relative()) sourcePath = base / sourcePath;
        m_mediaPath = std::filesystem::absolute(sourcePath).lexically_normal().native();
        if (!mapWide.empty()) {
            std::filesystem::path mapPath(mapWide);
            if (mapPath.is_relative()) mapPath = base / mapPath;
            m_mapPath = std::filesystem::absolute(mapPath).lexically_normal().native();
        }
    }
    catch (...) {
        errorMessage = "CReadOnlyMmtsFile: Cannot resolve .mmtsedit paths";
        return false;
    }
    if (_tcsicmp(::PathFindExtension(m_mediaPath.c_str()), TEXT(".mmts")) != 0) {
        errorMessage = "CReadOnlyMmtsFile: .mmtsedit source is not an .mmts file";
        return false;
    }
    m_editSourceSize = sourceSize;
    m_editSegments = std::move(segments);
    return true;
}

bool CReadOnlyMmtsFile::Open(LPCTSTR path, int flags, const char *&errorMessage)
{
    Close();
    const bool editFile = _tcsicmp(::PathFindExtension(path), TEXT(".mmtsedit")) == 0;
    if (!(flags & OPEN_FLAG_NORMAL)) return false;
    if (editFile) {
        if (!LoadEdit(path, errorMessage)) { Close(); return false; }
    } else {
        m_mediaPath = path;
    }
    // The caller may retry with OPEN_FLAG_SHARE_WRITE: CReadOnlyLocalFile refuses
    // remote paths without it.  The stream still has to be a finished recording,
    // which the .mmtsmap size check below enforces.
    if (!m_input.Open(m_mediaPath.c_str(), flags, errorMessage)) { Close(); return false; }
    m_inputSize = m_input.GetSize();
    if (m_inputSize <= 0 || (m_editSourceSize >= 0 && m_editSourceSize != m_inputSize) ||
        !LoadSidecarMap(m_mediaPath.c_str(), m_mapPath.empty() ? nullptr : m_mapPath.c_str())) {
        errorMessage = "CReadOnlyMmtsFile: A valid .mmtsmap sidecar is required for seekable MMTS playback";
        Close(); return false;
    }
    if (!m_editSegments.empty()) {
        for (const auto &segment : m_editSegments) {
            if (segment.endMsec > m_sourceDurationMsec) {
                errorMessage = "CReadOnlyMmtsFile: .mmtsedit segment exceeds source duration";
                Close(); return false;
            }
        }
        m_durationMsec = m_editSegments.back().programStartMsec +
                         (m_editSegments.back().endMsec - m_editSegments.back().startMsec);
    }
    std::string readerName, proxyServer, winscardDll;
    bool convertResolutionGaiji = true, useSmartCard = false;
    if (!LoadSettings(readerName, proxyServer, winscardDll, convertResolutionGaiji, useSmartCard)) {
        errorMessage = "CReadOnlyMmtsFile: MMTS playback is disabled or settings cannot be loaded";
        Close(); return false;
    }
    m_converter = std::make_unique<Mmt4kConverter>();
    if (!m_converter->Init(readerName, proxyServer, winscardDll, convertResolutionGaiji, useSmartCard)) {
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
    m_editSegments.clear(); m_mediaPath.clear(); m_mapPath.clear();
    m_outputOffset = 0; m_inputSize = m_virtualSize = -1; m_position = 0; m_durationMsec = m_sourceDurationMsec = 0;
    m_firstPtsMsec = 0; m_editSourceSize = -1; m_editCurrentSegment = -1; m_eof = m_started = false;
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
    if (!m_editSegments.empty()) {
        int segmentIndex = static_cast<int>(m_editSegments.size()) - 1;
        int sourceTarget = m_editSegments.back().endMsec;
        for (size_t i = 0; i < m_editSegments.size(); ++i) {
            const EditSegment &segment = m_editSegments[i];
            const int duration = segment.endMsec - segment.startMsec;
            if (targetMsec < segment.programStartMsec + duration || i + 1 == m_editSegments.size()) {
                segmentIndex = static_cast<int>(i);
                sourceTarget = segment.startMsec + (std::max)(0, (std::min)(targetMsec - segment.programStartMsec, duration));
                break;
            }
        }
        if (!StartEditSegment(segmentIndex, sourceTarget)) return false;
        m_position = position;
        return true;
    }
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

bool CReadOnlyMmtsFile::StartEditSegment(int segmentIndex, int sourceTargetMsec)
{
    if (segmentIndex < 0 || segmentIndex >= static_cast<int>(m_editSegments.size())) return false;
    const EditSegment &segment = m_editSegments[segmentIndex];
    sourceTargetMsec = (std::max)(segment.startMsec, (std::min)(sourceTargetMsec, segment.endMsec));
    if (m_input.SetPointer(FindSourceOffset(sourceTargetMsec), MOVE_METHOD_BEGIN) < 0) return false;
    if (m_started) m_converter->Reset();
    else m_started = true;
    m_converter->SetEditSegment(m_firstPtsMsec + segment.startMsec, m_firstPtsMsec + segment.endMsec,
                                m_firstPtsMsec + segment.programStartMsec);
    m_output.clear(); m_outputOffset = 0; m_eof = false; m_editCurrentSegment = segmentIndex;
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
        if (m_editCurrentSegment >= 0 && m_converter->IsEditSegmentComplete()) {
            const int nextSegment = m_editCurrentSegment + 1;
            if (nextSegment >= static_cast<int>(m_editSegments.size())) {
                m_eof = true;
            } else if (!StartEditSegment(nextSegment, m_editSegments[nextSegment].startMsec)) {
                return false;
            }
        }
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
