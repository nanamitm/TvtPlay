#include <Windows.h>
#include <Shlwapi.h>
#include <process.h>
#include <algorithm>
#include "Util.h"
#include "ReadOnlyFile.h"
#include "ThumbnailGenerator.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace
{
const int HEAD_READ_SIZE = 4 * 1024 * 1024;
const int PCR_READ_SIZE = 512 * 1024;
const int ES_READ_CHUNK = 1024 * 1024;
const int ES_READ_MAX = 8 * 1024 * 1024;
// 目標時刻との差がこれ以内になればバイト位置の探索をやめる
const int SEEK_TOLERANCE_MSEC = 1000;
const int SEEK_TRIES_MAX = 6;

const BYTE *PayloadOf(const BYTE *packet, int *pSize)
{
    int afc = (packet[3] >> 4) & 3;
    int offset = 4;
    if (afc & 2) offset += 1 + packet[4];
    *pSize = (afc & 1) && offset < 188 ? 188 - offset : 0;
    return packet + offset;
}

bool PcrOf(const BYTE *packet, DWORD *pPcr45khz)
{
    // adaptation_field_controlとadaptation_field_lengthとPCR_flag
    if (!(packet[3] & 0x20) || packet[4] < 7 || !(packet[5] & 0x10)) return false;
    *pPcr45khz = static_cast<DWORD>(packet[6]) << 24 | packet[7] << 16 | packet[8] << 8 | packet[9];
    return true;
}

int PidOf(const BYTE *packet)
{
    return (packet[1] & 0x1f) << 8 | packet[2];
}

// バッファ内のTSパケットを順に辿る
class CPacketIterator
{
public:
    CPacketIterator(std::vector<BYTE> &buf, int unitSize) : m_unitSize(unitSize), m_tail(buf.data() + buf.size()) {
        m_curr = buf.size() >= static_cast<size_t>(unitSize) * 8 ? resync(buf.data(), m_tail, unitSize) : nullptr;
    }
    const BYTE *Next() {
        while (m_curr && m_curr + 188 <= m_tail) {
            const BYTE *packet = m_curr;
            m_curr += m_unitSize;
            if (packet[0] == 0x47) return packet;
            // 同期が外れたら取り直す
            m_curr = m_tail - packet >= m_unitSize * 8 ? resync(const_cast<BYTE*>(packet), m_tail, m_unitSize) : nullptr;
        }
        return nullptr;
    }
private:
    int m_unitSize;
    BYTE *m_tail;
    BYTE *m_curr;
};

// 指定PIDの最初の完全なセクションを取り出す
bool ReadSection(std::vector<BYTE> &buf, int unitSize, int pid, std::vector<BYTE> &section)
{
    section.clear();
    CPacketIterator it(buf, unitSize);
    for (const BYTE *packet; (packet = it.Next()) != nullptr;) {
        if (PidOf(packet) != pid) continue;
        int size;
        const BYTE *payload = PayloadOf(packet, &size);
        if (packet[1] & 0x40) {
            if (size < 1 || 1 + payload[0] >= size) continue;
            section.assign(payload + 1 + payload[0], payload + size);
        }
        else if (!section.empty()) {
            section.insert(section.end(), payload, payload + size);
        }
        if (section.size() >= 3) {
            size_t len = 3 + ((section[1] & 0x0f) << 8 | section[2]);
            if (section.size() >= len) {
                section.resize(len);
                return true;
            }
        }
    }
    return false;
}
}

class CThumbnailGenerator::CDecoder
{
public:
    CDecoder() : m_generation(-1), m_fUnsupported(false), m_unitSize(0), m_pcrPid(-1), m_videoPid(-1), m_initPcr(0)
               , m_codec(nullptr), m_frame(nullptr), m_packet(nullptr), m_sws(nullptr) {}
    ~CDecoder() {
        sws_freeContext(m_sws);
        av_packet_free(&m_packet);
        av_frame_free(&m_frame);
        avcodec_free_context(&m_codec);
    }
    int GetGeneration() const { return m_generation; }
    bool IsUnsupported() const { return m_fUnsupported; }
    void Open(int generation, LPCTSTR path);
    int Generate(const REQUEST &req, CThumbnailGenerator *pOwner, THUMBNAIL_IMAGE *pImage);
private:
    bool ReadAt(__int64 pos, int size, std::vector<BYTE> &buf);
    bool FindPcrAt(__int64 pos, DWORD *pPcr);
    __int64 FindBytePosition(int msec, int durMsec, int serial, CThumbnailGenerator *pOwner);
    bool DecodeFrom(__int64 pos, int serial, CThumbnailGenerator *pOwner, bool *pfScrambled);
    bool ReceiveFrame();
    bool Scale(int width, THUMBNAIL_IMAGE *pImage);

    int m_generation;
    bool m_fUnsupported;
    CReadOnlyLocalFile m_file;
    int m_unitSize;
    int m_pcrPid;
    int m_videoPid;
    DWORD m_initPcr;
    AVCodecContext *m_codec;
    AVFrame *m_frame;
    AVPacket *m_packet;
    SwsContext *m_sws;
};

void CThumbnailGenerator::CDecoder::Open(int generation, LPCTSTR path)
{
    m_generation = generation;
    m_fUnsupported = true;
    m_file.Close();
    const char *errorMessage = nullptr;
    // 録画中のファイルも開けるように書き込み共有する
    if (!m_file.Open(path, IReadOnlyFile::OPEN_FLAG_NORMAL | IReadOnlyFile::OPEN_FLAG_SHARE_WRITE, errorMessage)) return;

    std::vector<BYTE> head;
    if (!ReadAt(0, HEAD_READ_SIZE, head)) return;
    m_unitSize = select_unit_size(head.data(), head.data() + head.size());
    if (m_unitSize < 188 || 320 < m_unitSize) return;

    // 映像のパケットが実際に含まれている最初の番組を探す
    // (サービスを絞らずに録画するとPATには録画していない番組も並ぶ)
    std::vector<bool> pidExists(0x2000);
    {
        CPacketIterator it(head, m_unitSize);
        for (const BYTE *packet; (packet = it.Next()) != nullptr;) {
            pidExists[PidOf(packet)] = true;
        }
    }
    std::vector<BYTE> pat, section;
    if (!ReadSection(head, m_unitSize, 0, pat) || pat[0] != 0x00) return;
    int videoType = -1;
    for (size_t i = 8; videoType < 0 && i + 4 + 4 <= pat.size(); i += 4) {
        int pmtPid = (pat[i+2] & 0x1f) << 8 | pat[i+3];
        if ((pat[i] << 8 | pat[i+1]) == 0 || !pidExists[pmtPid] ||
            !ReadSection(head, m_unitSize, pmtPid, section) || section[0] != 0x02 || section.size() < 16) continue;
        for (size_t j = 12 + ((section[10] & 0x0f) << 8 | section[11]); j + 5 + 4 <= section.size();) {
            int streamType = section[j];
            int pid = (section[j+1] & 0x1f) << 8 | section[j+2];
            if ((streamType == H_262_VIDEO || streamType == AVC_VIDEO || streamType == H_265_VIDEO) && pidExists[pid]) {
                videoType = streamType;
                m_videoPid = pid;
                m_pcrPid = (section[8] & 0x1f) << 8 | section[9];
                break;
            }
            j += 5 + ((section[j+3] & 0x0f) << 8 | section[j+4]);
        }
    }
    // 今のところMPEG-2映像のみ
    if (videoType != H_262_VIDEO) return;

    bool fPcr = false;
    CPacketIterator it(head, m_unitSize);
    for (const BYTE *packet; !fPcr && (packet = it.Next()) != nullptr;) {
        fPcr = PidOf(packet) == m_pcrPid && PcrOf(packet, &m_initPcr);
    }
    if (!fPcr) return;

    if (!m_codec) {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
        if (!codec) return;
        m_codec = avcodec_alloc_context3(codec);
        m_frame = av_frame_alloc();
        m_packet = av_packet_alloc();
        if (!m_codec || !m_frame || !m_packet) return;
        // Bピクチャは表示に使わないので省く
        m_codec->skip_frame = AVDISCARD_NONREF;
        m_codec->thread_count = 1;
        if (avcodec_open2(m_codec, codec, nullptr) < 0) {
            avcodec_free_context(&m_codec);
            return;
        }
    }
    m_fUnsupported = false;
}

// 戻り値は THUMBNAIL_IMAGE::status と同じ。打ち切られたときは負を返す
int CThumbnailGenerator::CDecoder::Generate(const REQUEST &req, CThumbnailGenerator *pOwner, THUMBNAIL_IMAGE *pImage)
{
    if (m_fUnsupported) return 2;
    __int64 pos = FindBytePosition(req.msec, req.durMsec, req.serial, pOwner);
    if (pos < 0) return pos == -2 ? -1 : 1;
    bool fScrambled = false;
    if (!DecodeFrom(pos, req.serial, pOwner, &fScrambled)) {
        if (fScrambled) {
            // 復号されていないファイルは対象外
            m_fUnsupported = true;
            return 2;
        }
        return pOwner->IsSuperseded(req.serial) ? -1 : 1;
    }
    return Scale(req.width, pImage) ? 0 : 1;
}

bool CThumbnailGenerator::CDecoder::ReadAt(__int64 pos, int size, std::vector<BYTE> &buf)
{
    buf.resize(size);
    if (m_file.SetPointer(pos, IReadOnlyFile::MOVE_METHOD_BEGIN) < 0) return false;
    int total = 0;
    while (total < size) {
        int n = m_file.Read(buf.data() + total, size - total);
        if (n <= 0) break;
        total += n;
    }
    buf.resize(total);
    return total > 0;
}

bool CThumbnailGenerator::CDecoder::FindPcrAt(__int64 pos, DWORD *pPcr)
{
    std::vector<BYTE> buf;
    if (!ReadAt(pos, PCR_READ_SIZE, buf)) return false;
    CPacketIterator it(buf, m_unitSize);
    for (const BYTE *packet; (packet = it.Next()) != nullptr;) {
        if (PidOf(packet) == m_pcrPid && PcrOf(packet, pPcr)) return true;
    }
    return false;
}

// 再生位置に対応するバイト位置をPCRを手がかりに探す
// 失敗したときは-1、打ち切られたときは-2を返す
__int64 CThumbnailGenerator::CDecoder::FindBytePosition(int msec, int durMsec, int serial, CThumbnailGenerator *pOwner)
{
    __int64 size = m_file.GetSize();
    if (size <= 0 || durMsec <= 0) return -1;
    // 終端付近ではIピクチャが見つからないことがある
    msec = min(max(msec, 0), max(durMsec - 2000, 0));

    __int64 loPos = 0, hiPos = size;
    int loMsec = 0, hiMsec = durMsec;
    __int64 pos = size * msec / durMsec;
    for (int i = 0; i < SEEK_TRIES_MAX; ++i) {
        if (pOwner->IsSuperseded(serial)) return -2;
        pos -= pos % m_unitSize;
        DWORD pcr;
        if (!FindPcrAt(pos, &pcr)) break;
        int posMsec = static_cast<int>((pcr - m_initPcr) / PCR_PER_MSEC);
        // PCRの不連続などで範囲外になったらそれ以上詰めない
        if (posMsec < loMsec || hiMsec < posMsec) break;
        if (abs(posMsec - msec) <= SEEK_TOLERANCE_MSEC) break;
        if (posMsec < msec) {
            loPos = pos;
            loMsec = posMsec;
        }
        else {
            hiPos = pos;
            hiMsec = posMsec;
        }
        if (hiMsec <= loMsec) break;
        pos = loPos + (hiPos - loPos) * (msec - loMsec) / (hiMsec - loMsec);
    }
    return pos;
}

bool CThumbnailGenerator::CDecoder::DecodeFrom(__int64 pos, int serial, CThumbnailGenerator *pOwner, bool *pfScrambled)
{
    avcodec_flush_buffers(m_codec);
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    if (!parser) return false;

    bool fDone = false;
    bool fStarted = false;
    int scrambledCount = 0;
    int clearCount = 0;
    std::vector<BYTE> buf;
    for (int readSize = 0; !fDone && readSize < ES_READ_MAX; readSize += ES_READ_CHUNK) {
        if (pOwner->IsSuperseded(serial) || !ReadAt(pos + readSize, ES_READ_CHUNK, buf)) break;
        CPacketIterator it(buf, m_unitSize);
        for (const BYTE *packet; !fDone && (packet = it.Next()) != nullptr;) {
            if (PidOf(packet) != m_videoPid) continue;
            if (packet[3] & 0xc0) {
                ++scrambledCount;
                continue;
            }
            ++clearCount;
            int size;
            const BYTE *payload = PayloadOf(packet, &size);
            if (packet[1] & 0x40) {
                // PESヘッダを飛ばす
                if (size < 9 || payload[0] != 0 || payload[1] != 0 || payload[2] != 1 || 9 + payload[8] > size) {
                    fStarted = false;
                    continue;
                }
                int headerSize = 9 + payload[8];
                payload += headerSize;
                size -= headerSize;
                fStarted = true;
            }
            else if (!fStarted) {
                continue;
            }
            while (size > 0 && !fDone) {
                uint8_t *data;
                int dataSize;
                int n = av_parser_parse2(parser, m_codec, &data, &dataSize, payload, size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                if (n < 0) break;
                payload += n;
                size -= n;
                if (dataSize > 0) {
                    m_packet->data = data;
                    m_packet->size = dataSize;
                    if (avcodec_send_packet(m_codec, m_packet) >= 0) {
                        fDone = ReceiveFrame();
                    }
                }
            }
        }
        if (scrambledCount > 64 && scrambledCount > clearCount) {
            *pfScrambled = true;
            break;
        }
    }
    if (!fDone && !*pfScrambled && !pOwner->IsSuperseded(serial)) {
        // 読み切っても出てこなければ残りを吐き出させる
        if (avcodec_send_packet(m_codec, nullptr) >= 0) {
            fDone = ReceiveFrame();
        }
    }
    av_parser_close(parser);
    return fDone;
}

bool CThumbnailGenerator::CDecoder::ReceiveFrame()
{
    return avcodec_receive_frame(m_codec, m_frame) >= 0;
}

bool CThumbnailGenerator::CDecoder::Scale(int width, THUMBNAIL_IMAGE *pImage)
{
    int srcW = m_frame->width;
    int srcH = m_frame->height;
    if (srcW <= 0 || srcH <= 0) return false;
    // 表示アスペクト比で高さを決める(1440x1080は16:9になる)
    AVRational sar = m_frame->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) sar = AVRational{1, 1};
    int height = static_cast<int>((static_cast<__int64>(width) * srcH * sar.den + static_cast<__int64>(srcW) * sar.num / 2) /
                                  (static_cast<__int64>(srcW) * sar.num));
    height = min(max(height, width / 4), width * 2);

    m_sws = sws_getCachedContext(m_sws, srcW, srcH, static_cast<AVPixelFormat>(m_frame->format),
                                 width, height, AV_PIX_FMT_BGRA, SWS_AREA, nullptr, nullptr, nullptr);
    if (!m_sws) return false;
    pImage->width = width;
    pImage->height = height;
    pImage->bits.resize(static_cast<size_t>(width) * height * 4);
    uint8_t *dst[4] = {pImage->bits.data()};
    int dstStride[4] = {width * 4};
    bool fOk = sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, srcH, dst, dstStride) == height;
    av_frame_unref(m_frame);
    return fOk;
}


CThumbnailGenerator::CThumbnailGenerator()
    : m_hThread(nullptr)
    , m_hEvent(nullptr)
    , m_hwndNotify(nullptr)
    , m_notifyMsg(0)
    , m_fStop(false)
    , m_serial(0)
    , m_request()
    , m_fRequested(false)
{
}

CThumbnailGenerator::~CThumbnailGenerator()
{
    Stop();
}

bool CThumbnailGenerator::Start(HWND hwndNotify, UINT notifyMsg)
{
    if (m_hThread) return true;
    // 途中からのデコードで出るエラーは想定内なので黙らせる
    av_log_set_level(AV_LOG_QUIET);
    m_hwndNotify = hwndNotify;
    m_notifyMsg = notifyMsg;
    m_fStop = false;
    m_fRequested = false;
    m_hEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_hEvent) return false;
    m_hThread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, ThreadProc, this, 0, nullptr));
    if (!m_hThread) {
        ::CloseHandle(m_hEvent);
        m_hEvent = nullptr;
        return false;
    }
    // 再生を妨げないように
    ::SetThreadPriority(m_hThread, THREAD_PRIORITY_BELOW_NORMAL);
    return true;
}

void CThumbnailGenerator::Stop()
{
    if (m_hThread) {
        {
            lock_recursive_mutex lock(m_lock);
            m_fStop = true;
        }
        ::SetEvent(m_hEvent);
        ::WaitForSingleObject(m_hThread, INFINITE);
        ::CloseHandle(m_hThread);
        m_hThread = nullptr;
        ::CloseHandle(m_hEvent);
        m_hEvent = nullptr;
    }
    lock_recursive_mutex lock(m_lock);
    m_result.reset();
}

void CThumbnailGenerator::Request(int generation, LPCTSTR path, int msec, int durMsec, int width)
{
    if (!m_hThread) return;
    {
        lock_recursive_mutex lock(m_lock);
        m_request.serial = ++m_serial;
        m_request.generation = generation;
        m_request.path = path;
        m_request.msec = msec;
        m_request.durMsec = durMsec;
        m_request.width = width;
        m_fRequested = true;
    }
    ::SetEvent(m_hEvent);
}

std::unique_ptr<THUMBNAIL_IMAGE> CThumbnailGenerator::TakeResult()
{
    lock_recursive_mutex lock(m_lock);
    return std::move(m_result);
}

bool CThumbnailGenerator::IsSupportedFile(LPCTSTR path)
{
    LPCTSTR ext = ::PathFindExtension(path);
    return !_tcsicmp(ext, TEXT(".ts")) || !_tcsicmp(ext, TEXT(".m2t")) || !_tcsicmp(ext, TEXT(".m2ts"));
}

bool CThumbnailGenerator::IsSuperseded(int serial)
{
    lock_recursive_mutex lock(m_lock);
    return m_fStop || (m_fRequested && m_request.serial != serial);
}

unsigned int __stdcall CThumbnailGenerator::ThreadProc(LPVOID pParam)
{
    static_cast<CThumbnailGenerator*>(pParam)->Run();
    return 0;
}

void CThumbnailGenerator::Run()
{
    CDecoder decoder;
    for (;;) {
        ::WaitForSingleObject(m_hEvent, INFINITE);
        REQUEST req;
        {
            lock_recursive_mutex lock(m_lock);
            if (m_fStop) break;
            if (!m_fRequested) continue;
            req = m_request;
            m_fRequested = false;
        }
        if (decoder.GetGeneration() != req.generation) {
            decoder.Open(req.generation, req.path.c_str());
        }
        std::unique_ptr<THUMBNAIL_IMAGE> image(new THUMBNAIL_IMAGE());
        image->generation = req.generation;
        image->msec = req.msec;
        image->width = image->height = 0;
        image->status = decoder.Generate(req, this, image.get());
        if (image->status < 0) continue;
        {
            lock_recursive_mutex lock(m_lock);
            if (m_fStop) break;
            // 受け取られる前に次の結果ができたら古いほうは捨てる
            m_result = std::move(image);
        }
        ::PostMessage(m_hwndNotify, m_notifyMsg, 0, 0);
    }
}
