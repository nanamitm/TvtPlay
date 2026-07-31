#pragma once

// In-process wrapper around dantto4k's MMT/TLV demuxer, ACAS decryptor, and
// MPEG-2 TS remuxer.  It is built only for x64 (see TvtPlay.vcxproj).
#ifdef ENABLE_MMT4K

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Mmt4kConverter
{
public:
    Mmt4kConverter();
    ~Mmt4kConverter();

    bool Init(const std::string &smartCardReaderName, const std::string &casProxyServer,
              const std::string &customWinscardDLL, bool convertResolutionGaiji);
    void Push(const uint8_t *data, size_t size);
    std::vector<uint8_t> TakeOutput();
    void Reset();

    // Restrict output to one source-time range and rebase it onto the
    // concatenated edit timeline.  Times are in the source MMT clock's
    // millisecond representation used by .mmtsmap.
    void SetEditSegment(int64_t sourceStartMsec, int64_t sourceEndMsec,
                        int64_t programStartMsec);
    bool IsEditSegmentComplete() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // ENABLE_MMT4K
