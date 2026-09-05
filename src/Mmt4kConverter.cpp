#ifdef ENABLE_MMT4K

#include "Mmt4kConverter.h"

#include <exception>
#include <iostream>
#include "acasHandler.h"
#include "casProxy.h"
#include "config.h"
#include "mmtTlvDemuxer.h"
#include "remuxerHandler.h"
#include "smartCard.h"
#include "stream.h"

namespace {

class EditRemuxerHandler final : public RemuxerHandler
{
public:
    explicit EditRemuxerHandler(MmtTlv::MmtTlvDemuxer &demuxer) : RemuxerHandler(demuxer) {}

    void SetEditSegment(int64_t sourceStartMsec, int64_t sourceEndMsec, int64_t programStartMsec)
    {
        m_editEnabled = true;
        m_segmentComplete = false;
        m_start90k = sourceStartMsec * 90;
        m_end90k = sourceEndMsec * 90;
        // RemuxerHandler subtracts this from PTS/DTS/PCR.  Keeping the first
        // segment's original base while subtracting each cut gap gives the
        // virtual program one continuous TS clock.
        setPtsOffset((sourceStartMsec - programStartMsec) * 90);
    }

    bool IsEditSegmentComplete() const { return m_segmentComplete; }

    void onVideoData(const MmtTlv::MmtStream &stream, const MmtTlv::MfuData &mfu) override
    {
        if (!Accept(stream, mfu, true)) return;
        RemuxerHandler::onVideoData(stream, mfu);
    }

    void onAudioData(const MmtTlv::MmtStream &stream, const MmtTlv::MfuData &mfu) override
    {
        if (!Accept(stream, mfu, false)) return;
        RemuxerHandler::onAudioData(stream, mfu);
    }

    void onSubtitleData(const MmtTlv::MmtStream &stream, const MmtTlv::MfuData &mfu) override
    {
        if (!Accept(stream, mfu, false)) return;
        RemuxerHandler::onSubtitleData(stream, mfu);
    }

private:
    static int64_t To90k(const MmtTlv::MmtStream &stream, const MmtTlv::MfuData &mfu)
    {
        if (mfu.pts == MmtTlv::NOPTS_VALUE) return -1;
        const auto timeBase = stream.getTimeBase();
        if (timeBase.den <= 0) return -1;
        return static_cast<int64_t>(static_cast<long double>(mfu.pts) * timeBase.num * 90000.0L / timeBase.den);
    }

    bool Accept(const MmtTlv::MmtStream &stream, const MmtTlv::MfuData &mfu, bool isVideo)
    {
        if (!m_editEnabled) return true;
        const int64_t pts90k = To90k(stream, mfu);
        if (pts90k < 0) return false;
        if (pts90k >= m_end90k) {
            if (isVideo) m_segmentComplete = true;
            return false;
        }
        return pts90k >= m_start90k;
    }

    bool m_editEnabled{};
    bool m_segmentComplete{};
    int64_t m_start90k{};
    int64_t m_end90k{};
};

} // namespace

struct Mmt4kConverter::Impl
{
    MmtTlv::MmtTlvDemuxer demuxer;
    EditRemuxerHandler remuxer{demuxer};
    std::vector<uint8_t> input;
    std::vector<uint8_t> output;

    Impl()
    {
        remuxer.setOutputCallback([this](const uint8_t *data, size_t size) {
            if (size == 188) {
                output.insert(output.end(), data, data + size);
            }
        });
        demuxer.setDemuxerHandler(remuxer);
    }
};

Mmt4kConverter::Mmt4kConverter() : m_impl(std::make_unique<Impl>()) {}
Mmt4kConverter::~Mmt4kConverter() = default;

bool Mmt4kConverter::Init(const std::string &smartCardReaderName, const std::string &casProxyServer,
                           const std::string &customWinscardDLL, bool convertResolutionGaiji,
                           bool useSmartCard)
{
    config.smartCardReaderName = smartCardReaderName;
    config.casProxyServer = casProxyServer;
    config.customWinscardDLL = customWinscardDLL;
    config.convertResolutionGaiji = convertResolutionGaiji;
    if (!useSmartCard) {
        // Nothing is attached, so a scrambled packet would stall waiting for an
        // ECM that is never answered. Take the payload as plaintext instead,
        // which is what a descrambled recording is even when the flag in the
        // header was left set.
        m_impl->demuxer.setAssumeDescrambled(true);
        return true;
    }
    try {
        auto acasHandler = std::make_unique<AcasHandler>();
        std::unique_ptr<ISmartCard> smartCard;
        if (casProxyServer.empty()) {
            smartCard = std::make_unique<LocalSmartCard>();
        } else {
            auto address = casproxy::parseAddress(casProxyServer);
            if (!address) {
                return false;
            }
            smartCard = std::make_unique<RemoteSmartCard>(address->first, address->second);
        }
        smartCard->setSmartCardReaderName(smartCardReaderName);
        acasHandler->setSmartCard(std::move(smartCard));
        m_impl->demuxer.setCasHandler(std::move(acasHandler));
    }
    catch (const std::exception &e) {
        std::cerr << "Mmt4kConverter::Init: " << e.what() << std::endl;
        return false;
    }
    return true;
}

void Mmt4kConverter::Push(const uint8_t *data, size_t size)
{
    if (!data || size == 0) return;
    m_impl->input.insert(m_impl->input.end(), data, data + size);
    MmtTlv::Common::ReadStream input(m_impl->input);
    while (!input.isEof()) {
        if (m_impl->demuxer.demux(input) == MmtTlv::DemuxStatus::NotEnoughBuffer) {
            break;
        }
    }
    m_impl->input.erase(m_impl->input.begin(),
                        m_impl->input.begin() + (m_impl->input.size() - input.leftBytes()));
}

std::vector<uint8_t> Mmt4kConverter::TakeOutput()
{
    std::vector<uint8_t> output = std::move(m_impl->output);
    m_impl->output.clear();
    return output;
}

void Mmt4kConverter::Reset()
{
    m_impl->input.clear();
    m_impl->output.clear();
    m_impl->demuxer.resetStreams();
    m_impl->remuxer.clear();
}

void Mmt4kConverter::SetEditSegment(int64_t sourceStartMsec, int64_t sourceEndMsec,
                                    int64_t programStartMsec)
{
    m_impl->remuxer.SetEditSegment(sourceStartMsec, sourceEndMsec, programStartMsec);
}

bool Mmt4kConverter::IsEditSegmentComplete() const
{
    return m_impl->remuxer.IsEditSegmentComplete();
}

#endif // ENABLE_MMT4K
