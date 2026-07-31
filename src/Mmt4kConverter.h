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

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // ENABLE_MMT4K
