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

struct Mmt4kConverter::Impl
{
    MmtTlv::MmtTlvDemuxer demuxer;
    RemuxerHandler remuxer{demuxer};
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
                           const std::string &customWinscardDLL, bool convertResolutionGaiji)
{
    config.smartCardReaderName = smartCardReaderName;
    config.casProxyServer = casProxyServer;
    config.customWinscardDLL = customWinscardDLL;
    config.convertResolutionGaiji = convertResolutionGaiji;
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

#endif // ENABLE_MMT4K
