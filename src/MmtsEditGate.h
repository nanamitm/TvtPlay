#pragma once
#include <cstdint>
#include <unordered_map>

// Retain the AU's edit decision for fragments that carry no timestamp.
class MmtsEditGate {
public:
    void SetRange(int64_t start, int64_t end) { m_start = start; m_end = end; Clear(); }
    void Clear() { m_frames.clear(); }
    void Drop(int stream) { m_frames.erase(stream); }
    bool Accept(int stream, int64_t pts, bool first, bool last) {
        if (first) m_frames.erase(stream);
        if (pts >= 0) m_frames[stream] = pts >= m_start && pts < m_end;
        const auto it = m_frames.find(stream);
        const bool accepted = it != m_frames.end() && it->second;
        if (last) m_frames.erase(stream);
        return accepted;
    }
private:
    int64_t m_start{}, m_end{};
    std::unordered_map<int, bool> m_frames;
};
