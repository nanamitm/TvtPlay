#pragma once

#ifdef ENABLE_MMT4K

#include <Windows.h>
#include "ReadOnlyFile.h"
#include <memory>
#include <string>
#include <vector>

class Mmt4kConverter;

// Exposes an MMTS file as a seekable, virtual TS byte stream.  TS data is
// produced only on demand; no converted file is written to disk.
class CReadOnlyMmtsFile : public IReadOnlyFile
{
public:
    CReadOnlyMmtsFile();
    ~CReadOnlyMmtsFile();
    bool Open(LPCTSTR path, int flags, const char *&errorMessage) override;
    void Close() override;
    int Read(BYTE *pBuf, int numToRead) override;
    __int64 SetPointer(__int64 distanceToMove, MOVE_METHOD moveMethod) override;
    __int64 GetSize() const override { return m_virtualSize; }
    bool IsShareWrite() const override { return false; }

    int GetDurationMsec() const { return m_durationMsec; }
    int GetPositionMsecFromBytes(__int64 bytes) const;
    __int64 GetPositionBytesFromMsec(int msec) const;

private:
    struct MapPoint { __int64 timeMsec; __int64 offset; };
    bool LoadSettings(std::string &readerName, std::string &proxyServer,
                      std::string &winscardDll, bool &convertResolutionGaiji);
    bool LoadSidecarMap(LPCTSTR mediaPath);
    bool SeekToVirtualPosition(__int64 position);
    bool FillOutput();
    __int64 FindSourceOffset(int msec) const;

    CReadOnlyLocalFile m_input;
    std::unique_ptr<Mmt4kConverter> m_converter;
    std::vector<BYTE> m_output;
    size_t m_outputOffset{};
    std::vector<MapPoint> m_rapPoints;
    std::vector<MapPoint> m_seekPoints;
    __int64 m_inputSize{-1};
    __int64 m_virtualSize{-1};
    __int64 m_position{};
    int m_durationMsec{};
    bool m_eof{};
    bool m_started{};
};

#endif // ENABLE_MMT4K
